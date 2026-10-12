#include <Engine/Graphic/View/SceneViewState.hpp>

#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/View/TemporalUpscaler.hpp>

#include <algorithm>
#include <cmath>
#include <string_view>

namespace Desert::Graphic
{
    namespace
    {
        [[nodiscard]] bool SameTextureDesc( const RDG::TextureDesc& a, const RDG::TextureDesc& b )
        {
            return a.Size == b.Size && a.Format == b.Format && a.Mips == b.Mips && a.Layers == b.Layers &&
                   a.Dim == b.Dim && a.Samples == b.Samples;
        }

        // Bytes of one texture of @p desc: every mip of every layer, every sample.
        [[nodiscard]] uint64_t TextureBytes( const RDG::TextureDesc& desc )
        {
            uint64_t bytes  = 0;
            uint32_t width  = desc.Size.Width;
            uint32_t height = desc.Size.Height;
            uint32_t depth  = desc.Size.Depth;
            for ( uint32_t mip = 0; mip < desc.Mips; ++mip )
            {
                bytes += Core::Formats::CalculateImageSize( width, height, desc.Format ) * depth;
                width  = std::max( 1u, width / 2 );
                height = std::max( 1u, height / 2 );
                depth  = desc.Dim == RDG::TextureDim::Tex3D ? std::max( 1u, depth / 2 ) : depth;
            }
            return bytes * desc.Layers * desc.Samples;
        }

        [[nodiscard]] bool IsFinite( const glm::mat4& m )
        {
            for ( int c = 0; c < 4; ++c )
                for ( int r = 0; r < 4; ++r )
                    if ( !std::isfinite( m[c][r] ) )
                        return false;
            return true;
        }

        [[nodiscard]] std::string_view TemporalMethodName( const TemporalMethod method )
        {
            switch ( method )
            {
                case TemporalMethod::None:
                    return "None";
                case TemporalMethod::TAA:
                    return "TAA";
                case TemporalMethod::TAAU:
                    return "TAAU";
            }
            return "<unknown TemporalMethod>";
        }

        // HistoryTextureDesc's naming rule: every side of every history has its own non-empty graph name.
        [[nodiscard]] Common::BoolResultStr CheckHistoryNames( const std::span<const HistoryTextureDesc> descs,
                                                               const std::string_view upscalerName )
        {
            std::vector<std::string_view> seen;
            seen.reserve( descs.size() * 2 );
            for ( std::size_t i = 0; i < descs.size(); ++i )
            {
                for ( const char* side : { descs[i].Name, descs[i].PreviousName } )
                {
                    const std::string_view name = side != nullptr ? std::string_view( side ) : std::string_view();
                    if ( name.empty() )
                        return Common::MakeFormattedError<bool>(
                             "SceneViewState::BeginFrame: upscaler '{}' declares history {} with an empty {}",
                             upscalerName, i, side == descs[i].Name ? "Name" : "PreviousName" );
                    if ( std::find( seen.begin(), seen.end(), name ) != seen.end() )
                        return Common::MakeFormattedError<bool>(
                             "SceneViewState::BeginFrame: upscaler '{}' declares "
                             "the history name '{}' twice (history {})",
                             upscalerName, name, i );
                    seen.push_back( name );
                }
            }
            return Common::MakeSuccess( true );
        }
    } // namespace

    // ---- TemporalHistory ------------------------------------------------------------------------------------

    bool HistoryTextureDesc::operator==( const HistoryTextureDesc& other ) const
    {
        return SameTextureDesc( Desc, other.Desc ) && std::string_view( Name ) == std::string_view( other.Name ) &&
               std::string_view( PreviousName ) == std::string_view( other.PreviousName );
    }

    bool TemporalHistory::Prepare( std::span<const HistoryTextureDesc> descs )
    {
        if ( std::equal( descs.begin(), descs.end(), m_Descs.begin(), m_Descs.end() ) )
            return false;

        Release();
        m_Descs.assign( descs.begin(), descs.end() );
        m_Pairs.reserve( m_Descs.size() );
        for ( const HistoryTextureDesc& history : m_Descs )
            m_Pairs.push_back( { RDG::ExternalTexture( history.Desc, RDG::Access::None ),
                                 RDG::ExternalTexture( history.Desc, RDG::Access::None ) } );
        // No physical image here: this file is device-free. AllocatePhysical (TemporalHistoryPhysical.cpp) fills
        // both sides of each new pair.
        return true;
    }

    std::vector<HistoryRefs> TemporalHistory::Register( RDG::Builder& graph )
    {
        std::vector<HistoryRefs> refs;
        refs.reserve( m_Pairs.size() );
        m_RegisteredCurrent.clear();
        for ( std::size_t i = 0; i < m_Pairs.size(); ++i )
        {
            HistoryRefs history;
            history.Previous = graph.RegisterExternal( m_Pairs[i][m_CurrentSlot ^ 1u], m_Descs[i].PreviousName );
            history.Current  = graph.RegisterExternal( m_Pairs[i][m_CurrentSlot], m_Descs[i].Name );
            graph.SetFaultPolicy( history.Current, kHistoryFaultPolicy );
            m_RegisteredCurrent.push_back( history.Current.Index );
            refs.push_back( history );
        }
        return refs;
    }

    bool TemporalHistory::LostToFault( const RDG::ExecuteReport& report ) const
    {
        return std::any_of( report.InvalidatedExternals.begin(), report.InvalidatedExternals.end(),
                            [this]( const uint32_t index )
                            {
                                return std::find( m_RegisteredCurrent.begin(), m_RegisteredCurrent.end(),
                                                  index ) != m_RegisteredCurrent.end();
                            } );
    }

    void TemporalHistory::Swap()
    {
        m_CurrentSlot ^= 1u;
        m_RegisteredCurrent.clear();
    }

    void TemporalHistory::Release()
    {
        m_Descs.clear();
        m_Pairs.clear();
        m_Images.clear();
        m_CurrentSlot = 0;
        m_RegisteredCurrent.clear();
    }

    uint64_t TemporalHistory::HeldBytes() const
    {
        uint64_t bytes = 0;
        for ( const HistoryTextureDesc& history : m_Descs )
            bytes += 2u * TextureBytes( history.Desc ); // the pair
        return bytes;
    }

    // ---- SceneViewState -------------------------------------------------------------------------------------

    Common::ResultStr<ViewFrame> SceneViewState::BeginFrame( const ViewInputs&        inputs,
                                                             const ITemporalUpscaler* upscaler )
    {
        // Validate everything before anything changes: a refused frame leaves the state as it was.
        if ( !IsFinite( inputs.View ) )
            return Common::MakeFormattedError<ViewFrame>(
                 "SceneViewState::BeginFrame: ViewInputs::View is not finite" );
        if ( !IsFinite( inputs.Projection ) )
            return Common::MakeFormattedError<ViewFrame>(
                 "SceneViewState::BeginFrame: ViewInputs::Projection is not finite" );
        if ( !std::isfinite( inputs.CameraPosition.x ) || !std::isfinite( inputs.CameraPosition.y ) ||
             !std::isfinite( inputs.CameraPosition.z ) )
            return Common::MakeFormattedError<ViewFrame>(
                 "SceneViewState::BeginFrame: ViewInputs::CameraPosition ({}, {}, {}) is not finite",
                 inputs.CameraPosition.x, inputs.CameraPosition.y, inputs.CameraPosition.z );

        const auto split = MakeResolutionSplit( inputs.Output, inputs.RenderScalePercent );
        if ( !split.IsSuccess() )
            return Common::MakeError<ViewFrame>( split.GetError() );
        const auto method = SelectTemporalMethod( inputs.AntiAliasing, split.GetValue(), inputs.Upscaler );
        if ( !method.IsSuccess() )
            return Common::MakeError<ViewFrame>( method.GetError() );

        if ( method.GetValue() == TemporalMethod::None )
        {
            if ( upscaler != nullptr )
                return Common::MakeFormattedError<ViewFrame>(
                     "SceneViewState::BeginFrame: upscaler '{}' passed for a frame with no temporal method",
                     upscaler->DebugName() );
        }
        else
        {
            if ( upscaler == nullptr )
                return Common::MakeFormattedError<ViewFrame>(
                     "SceneViewState::BeginFrame: the frame's temporal method is {} and no upscaler was passed",
                     TemporalMethodName( method.GetValue() ) );
            if ( upscaler->Method() != method.GetValue() )
                return Common::MakeFormattedError<ViewFrame>( "SceneViewState::BeginFrame: the frame's temporal "
                                                              "method is {} but upscaler '{}' implements {}",
                                                              TemporalMethodName( method.GetValue() ),
                                                              upscaler->DebugName(),
                                                              TemporalMethodName( upscaler->Method() ) );
            if ( !upscaler->Supports( split.GetValue() ) )
                return Common::MakeFormattedError<ViewFrame>(
                     "SceneViewState::BeginFrame: upscaler '{}' does not support {}x{} -> {}x{} at {} %",
                     upscaler->DebugName(), split.GetValue().Render.Width, split.GetValue().Render.Height,
                     split.GetValue().Output.Width, split.GetValue().Output.Height,
                     split.GetValue().RenderScalePercent );
        }

        const std::vector<HistoryTextureDesc> descs =
             upscaler != nullptr ? upscaler->HistoryDescs( split.GetValue() ) : std::vector<HistoryTextureDesc>{};
        if ( upscaler != nullptr )
        {
            const Common::BoolResultStr named = CheckHistoryNames( descs, upscaler->DebugName() );
            if ( !named.IsSuccess() )
                return Common::MakeError<ViewFrame>( named.GetError() );
        }

        // A BeginFrame whose frame never reached EndFrame (FrameFault, no graph): it never happened. Only ITS
        // motion records go; the previous frame's stay, since they belong to the committed frame that this
        // frame's Prev* matrices and history textures also belong to.
        if ( m_FrameOpen )
            m_Motion.DiscardCurrent();

        if ( inputs.SceneIdentity != m_SceneIdentity )
        {
            m_Motion.Clear();
            m_SceneIdentity = inputs.SceneIdentity;
        }

        const bool historyRecreated = m_History.Prepare( descs );

        // Compared with the camera of the COMMITTED frame (the one the history and every Prev* belong to); this
        // frame's camera is only pending until its EndFrame, so an unended frame changes nothing here.
        const bool cameraChanged = inputs.CameraIdentity != m_CommittedCameraIdentity;

        HistoryResetReason reset = HistoryResetReason::None;
        if ( !m_HasCommitted )
            reset = HistoryResetReason::FirstFrame;
        else if ( m_PendingFaultReset )
            reset = HistoryResetReason::PassFault;
        else if ( inputs.CameraCut || cameraChanged )
            reset = HistoryResetReason::CameraCut;
        else if ( const bool methodChanged = method.GetValue() != m_Committed.Method;
                  !( split.GetValue() == m_Committed.Split ) ||
                  ( historyRecreated && !methodChanged ) ) // recreated after an unended frame of another shape
            reset = HistoryResetReason::Resize;
        else if ( methodChanged )
            reset = HistoryResetReason::TemporalMethodChange;
        const bool isReset = reset != HistoryResetReason::None;
        if ( isReset )
            m_Motion.Clear(); // "or the history was reset": no previous transform survives a reset

        ViewFrame f;
        f.FrameIndex = m_HasCommitted ? m_Committed.FrameIndex + 1 : 0;
        f.Split      = split.GetValue();
        f.Method     = method.GetValue();
        f.Quality    = inputs.Quality;

        f.View           = inputs.View;
        f.InvView        = glm::inverse( inputs.View );
        f.Projection     = inputs.Projection;
        f.InvProjection  = glm::inverse( inputs.Projection );
        f.CameraPosition = inputs.CameraPosition;
        f.NearPlane      = inputs.NearPlane;
        f.FarPlane       = inputs.FarPlane;

        f.JitterSequenceLength = TemporalJitterSequenceLength( f.Method, f.Split );
        f.JitterIndex  = ( isReset || f.JitterSequenceLength == 0 ) ? 0 : m_JitterIndex % f.JitterSequenceLength;
        f.JitterPixels = TemporalJitterPixels( f.JitterIndex, f.JitterSequenceLength );
        f.JitterNdc    = JitterPixelsToNdc( f.JitterPixels, f.Split.Render );

        // Method None: no jitter at all, so the jittered matrices are bit-identical to the unjittered ones.
        f.JitteredProjection =
             f.JitterSequenceLength == 0 ? f.Projection : ApplyJitter( f.Projection, f.JitterNdc );
        f.ViewProjection            = f.Projection * f.View;
        f.InvViewProjection         = glm::inverse( f.ViewProjection );
        f.JitteredViewProjection    = f.JitteredProjection * f.View;
        f.InvJitteredViewProjection = glm::inverse( f.JitteredViewProjection );

        const ViewFrame& previous    = isReset ? f : m_Committed;
        f.PrevView                   = previous.View;
        f.PrevProjection             = previous.Projection;
        f.PrevViewProjection         = previous.ViewProjection;
        f.PrevInvViewProjection      = previous.InvViewProjection;
        f.PrevJitteredViewProjection = previous.JitteredViewProjection;
        f.PrevCameraPosition         = previous.CameraPosition;
        f.PrevJitterNdc              = previous.JitterNdc;

        f.TimeSeconds     = inputs.TimeSeconds;
        f.PrevTimeSeconds = isReset ? inputs.TimeSeconds : m_Committed.TimeSeconds;
        f.DeltaSeconds    = static_cast<float>( f.TimeSeconds - f.PrevTimeSeconds );

        // Only a TEMPORAL upscale reconstructs output-pixel detail from the jittered samples; the spatial
        // upscale has one sample per render pixel, and a negative bias there only aliases (UE applies the
        // screen-percentage mip bias under temporal upsampling only).
        f.MaterialMipBias = 0.0f;
        if ( f.Split.Mode == Common::Scalability::ScaleMode::Upscale && f.Method != TemporalMethod::None &&
             f.Split.Render.Width != 0 && f.Split.Output.Width != 0 )
            f.MaterialMipBias = std::log2( static_cast<float>( f.Split.Render.Width ) /
                                           static_cast<float>( f.Split.Output.Width ) );

        f.HistoryReset = reset;

        m_Pending               = f;
        m_PendingCameraIdentity = inputs.CameraIdentity;
        m_FrameOpen             = true;
        return Common::MakeSuccess( f );
    }

    TemporalHistory& SceneViewState::History()
    {
        return m_History;
    }

    MotionHistory& SceneViewState::Motion()
    {
        return m_Motion;
    }

    void SceneViewState::EndFrame( const RDG::ExecuteReport& report )
    {
        m_Committed               = m_Pending;
        m_CommittedCameraIdentity = m_PendingCameraIdentity;
        m_HasCommitted            = true;
        m_FrameOpen               = false;
        m_JitterIndex             = m_Committed.JitterSequenceLength == 0
                                         ? 0
                                         : ( m_Committed.JitterIndex + 1 ) % m_Committed.JitterSequenceLength;
        m_PendingFaultReset       = m_History.LostToFault( report );
        m_History.Swap();
        m_Motion.EndFrame();
    }

    void SceneViewState::Reset()
    {
        m_History.Release();
        m_Motion.Clear();
        m_HasCommitted            = false;
        m_Pending                 = ViewFrame{};
        m_Committed               = ViewFrame{};
        m_CommittedCameraIdentity = 0;
        m_PendingCameraIdentity   = 0;
        m_FrameOpen               = false;
        m_PendingFaultReset       = false;
        m_JitterIndex             = 0;
    }

    uint64_t SceneViewState::HeldBytes() const
    {
        return m_History.HeldBytes();
    }
} // namespace Desert::Graphic
