#include <Engine/Graphic/View/SceneViewState.hpp>

#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/View/TemporalUpscaler.hpp>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

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
                case TemporalMethod::DLSS:
                    return "DLSS";
                case TemporalMethod::FSR:
                    return "FSR";
                case TemporalMethod::TSR:
                    return "TSR";
                case TemporalMethod::DLSSRR:
                    return "DLSSRR";
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
                const bool persistent = descs[i].Lifetime == HistoryLifetime::Persistent;
                if ( persistent && descs[i].PreviousName != nullptr && descs[i].PreviousName[0] != '\0' )
                    return Common::MakeFormattedError<bool>(
                         "SceneViewState::BeginFrame: upscaler '{}' declares the persistent history {} ('{}') with a "
                         "PreviousName '{}' (a persistent history has one side)",
                         upscalerName, i, descs[i].Name, descs[i].PreviousName );
                for ( const char* side : { descs[i].Name, descs[i].PreviousName } )
                {
                    if ( persistent && side == descs[i].PreviousName )
                        continue;
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

    // ---- VolumetricCloudHistory -----------------------------------------------------------------------------

    RDG::TextureDesc VolumetricCloudHistory::Desc( const ViewExtent halfExtent )
    {
        return RDG::TextureDesc{ .Size   = { .Width = halfExtent.Width, .Height = halfExtent.Height },
                                 .Format = ViewTargetFormats::kCloudTrace };
    }

    VolumetricCloudHistory::Refs VolumetricCloudHistory::RegisterPrevious( RDG::Builder&    graph,
                                                                           const ViewExtent halfExtent )
    {
        m_Created                                     = false;
        m_WriterPass                                  = UINT32_MAX;
        std::array<RDG::ExternalTexture, 2>& previous = m_Slots[m_PreviousSlot];
        // Another extent is another view size: the pair describes pixels that no longer exist.
        if ( !m_HasPrevious || !previous[0].Physical || !previous[1].Physical ||
             m_PreviousExtent.Width != halfExtent.Width || m_PreviousExtent.Height != halfExtent.Height )
            return {};
        return { graph.RegisterExternal( previous[0], "Clouds.History.Previous" ),
                 graph.RegisterExternal( previous[1], "Clouds.HistoryGuide.Previous" ) };
    }

    VolumetricCloudHistory::Refs VolumetricCloudHistory::CreateCurrent( RDG::Builder&    graph,
                                                                        const ViewExtent halfExtent )
    {
        // The other slot: what it held is two frames old and nothing registers it any more. Emptied first, so a
        // frame whose extraction does not happen leaves no stale image in it.
        std::array<RDG::ExternalTexture, 2>& next = m_Slots[m_PreviousSlot ^ 1u];
        next                                      = {};
        const RDG::TextureDesc desc               = Desc( halfExtent );
        const Refs             current{ graph.CreateTexture( desc, "Clouds.History" ),
                            graph.CreateTexture( desc, "Clouds.HistoryGuide" ) };
        graph.Extract( current.Scatter, next[0], RDG::Access::SampledCompute );
        graph.Extract( current.Guide, next[1], RDG::Access::SampledCompute );
        m_CurrentExtent = halfExtent;
        m_Created       = true;
        m_WriterPass    = UINT32_MAX;
        return current;
    }

    void VolumetricCloudHistory::SetWriterPass( const uint32_t writerPass )
    {
        m_WriterPass = writerPass;
    }

    void VolumetricCloudHistory::EndFrame( const RDG::ExecuteReport& report )
    {
        const bool created = std::exchange( m_Created, false );
        const bool lost    = m_WriterPass == UINT32_MAX ||
                          std::ranges::find( report.FaultCulled, m_WriterPass ) != report.FaultCulled.end();
        const uint32_t next = m_PreviousSlot ^ 1u;
        if ( !created || lost || !m_Slots[next][0].Physical || !m_Slots[next][1].Physical )
        {
            // No pair of THIS frame: the next frame's previous matrices describe this frame, so nothing older may
            // be reprojected through them. The memory goes with it (a view whose clouds are off holds none).
            m_Slots       = {};
            m_HasPrevious = false;
            return;
        }
        m_PreviousSlot   = next;
        m_PreviousExtent = m_CurrentExtent;
        m_HasPrevious    = true;
    }

    void VolumetricCloudHistory::Reset()
    {
        m_Slots          = {};
        m_PreviousExtent = {};
        m_CurrentExtent  = {};
        m_PreviousSlot   = 0;
        m_HasPrevious    = false;
        m_Created        = false;
        m_WriterPass     = UINT32_MAX;
    }

    uint64_t VolumetricCloudHistory::HeldBytes() const
    {
        uint64_t bytes = 0;
        for ( const std::array<RDG::ExternalTexture, 2>& slot : m_Slots )
            for ( const RDG::ExternalTexture& side : slot )
                if ( side.Physical )
                    bytes += TextureBytes( side.Desc );
        return bytes;
    }

    // ---- ScreenProbeGatherHistory ---------------------------------------------------------------------------

    namespace
    {
        constexpr std::array<std::string_view, ScreenProbeGatherHistory::TextureCount> kScreenProbeHistoryNames{
             "ScreenProbes.DiffuseIndirect", "ScreenProbes.RoughSpecularIndirect",
             "ScreenProbes.BackfaceDiffuseIndirect", "ScreenProbes.HistoryGuide" };
        constexpr std::array<std::string_view, ScreenProbeGatherHistory::TextureCount> kScreenProbePreviousNames{
             "ScreenProbes.DiffuseIndirect.Previous", "ScreenProbes.RoughSpecularIndirect.Previous",
             "ScreenProbes.BackfaceDiffuseIndirect.Previous", "ScreenProbes.HistoryGuide.Previous" };

        using ScreenProbeHistorySet = std::array<RDG::ExternalTexture, ScreenProbeGatherHistory::TextureCount>;

        [[nodiscard]] bool AnyMissing( const ScreenProbeHistorySet& set )
        {
            return std::ranges::any_of( set,
                                        []( const RDG::ExternalTexture& texture ) { return !texture.Physical; } );
        }
    } // namespace

    RDG::TextureDesc ScreenProbeGatherHistory::Desc( const Texture texture, const ViewExtent extent )
    {
        return RDG::TextureDesc{ .Size   = { .Width = extent.Width, .Height = extent.Height },
                                 .Format = texture == Guide ? ViewTargetFormats::kGIFinalGatherGuide
                                                            : ViewTargetFormats::kGIFinalGather };
    }

    ScreenProbeGatherHistory::Refs ScreenProbeGatherHistory::RegisterPrevious( RDG::Builder&    graph,
                                                                               const ViewExtent extent )
    {
        m_Created                                                = false;
        m_WriterPass                                             = UINT32_MAX;
        std::array<RDG::ExternalTexture, TextureCount>& previous = m_Slots[m_PreviousSlot];
        // Another extent is another view size: the set describes pixels that no longer exist.
        if ( !m_HasPrevious || AnyMissing( previous ) || m_PreviousExtent.Width != extent.Width ||
             m_PreviousExtent.Height != extent.Height )
            return {};
        Refs refs;
        for ( uint32_t i = 0; i < TextureCount; ++i )
            refs[i] = graph.RegisterExternal( previous[i], kScreenProbePreviousNames[i] );
        return refs;
    }

    ScreenProbeGatherHistory::Refs ScreenProbeGatherHistory::CreateCurrent( RDG::Builder&    graph,
                                                                            const ViewExtent extent )
    {
        // The other slot held the set of two frames ago, which nothing registers any more; emptied first so a
        // frame whose extraction does not happen leaves no stale image in it.
        std::array<RDG::ExternalTexture, TextureCount>& next = m_Slots[m_PreviousSlot ^ 1u];
        next                                                 = {};
        Refs current;
        for ( uint32_t i = 0; i < TextureCount; ++i )
        {
            current[i] =
                 graph.CreateTexture( Desc( static_cast<Texture>( i ), extent ), kScreenProbeHistoryNames[i] );
            graph.Extract( current[i], next[i], RDG::Access::SampledCompute );
        }
        m_CurrentExtent = extent;
        m_Created       = true;
        m_WriterPass    = UINT32_MAX;
        return current;
    }

    void ScreenProbeGatherHistory::SetWriterPass( const uint32_t writerPass )
    {
        m_WriterPass = writerPass;
    }

    void ScreenProbeGatherHistory::EndFrame( const RDG::ExecuteReport& report )
    {
        const bool created = std::exchange( m_Created, false );
        const bool lost    = m_WriterPass == UINT32_MAX ||
                          std::ranges::find( report.FaultCulled, m_WriterPass ) != report.FaultCulled.end();
        const uint32_t next = m_PreviousSlot ^ 1u;
        if ( !created || lost || AnyMissing( m_Slots[next] ) )
        {
            // No set of THIS frame: nothing older may be reprojected through the next frame's previous matrices.
            // The memory goes with it (a view without the screen probe gather holds none).
            m_Slots       = {};
            m_HasPrevious = false;
            return;
        }
        m_PreviousSlot   = next;
        m_PreviousExtent = m_CurrentExtent;
        m_HasPrevious    = true;
    }

    void ScreenProbeGatherHistory::Reset()
    {
        m_Slots          = {};
        m_PreviousExtent = {};
        m_CurrentExtent  = {};
        m_PreviousSlot   = 0;
        m_HasPrevious    = false;
        m_Created        = false;
        m_WriterPass     = UINT32_MAX;
    }

    uint64_t ScreenProbeGatherHistory::HeldBytes() const
    {
        uint64_t bytes = 0;
        for ( const std::array<RDG::ExternalTexture, TextureCount>& slot : m_Slots )
            for ( const RDG::ExternalTexture& texture : slot )
                if ( texture.Physical )
                    bytes += TextureBytes( texture.Desc );
        return bytes;
    }

    // ---- TemporalHistory ------------------------------------------------------------------------------------

    bool HistoryTextureDesc::operator==( const HistoryTextureDesc& other ) const
    {
        return SameTextureDesc( Desc, other.Desc ) && Lifetime == other.Lifetime &&
               std::string_view( Name ) == std::string_view( other.Name ) &&
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
            if ( m_Descs[i].Lifetime == HistoryLifetime::Persistent )
            {
                // One side, never swapped: side 0 is the texture.
                history.Current  = graph.RegisterExternal( m_Pairs[i][0], m_Descs[i].Name );
                history.Previous = history.Current;
            }
            else
            {
                history.Previous = graph.RegisterExternal( m_Pairs[i][m_CurrentSlot ^ 1u], m_Descs[i].PreviousName );
                history.Current  = graph.RegisterExternal( m_Pairs[i][m_CurrentSlot], m_Descs[i].Name );
            }
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

        if ( upscaler != nullptr && inputs.Scalability == nullptr )
            return Common::MakeFormattedError<ViewFrame>(
                 "SceneViewState::BeginFrame: upscaler '{}' needs ViewInputs::Scalability (the renderer's "
                 "resolved "
                 "quality its history shape is read from), and none was given",
                 upscaler->DebugName() );
        const std::vector<HistoryTextureDesc> descs =
             upscaler != nullptr ? upscaler->HistoryDescs( split.GetValue(), *inputs.Scalability )
                                 : std::vector<HistoryTextureDesc>{};
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

        // The method's own sample pattern (ITemporalUpscaler::JitterSequenceLength / JitterPixels; the engine's
        // Halton sequence unless a vendor SDK prescribes another). No upscaler = method None = no jitter.
        f.JitterSequenceLength = upscaler != nullptr ? upscaler->JitterSequenceLength( f.Split ) : 0;
        f.JitterIndex  = ( isReset || f.JitterSequenceLength == 0 ) ? 0 : m_JitterIndex % f.JitterSequenceLength;
        f.JitterPixels         = f.JitterSequenceLength == 0
                                      ? glm::vec2( 0.0f )
                                      : upscaler->JitterPixels( f.JitterIndex, f.JitterSequenceLength );
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

        // Only a TEMPORAL method reconstructs output-pixel detail from the jittered samples; the spatial
        // upscale has one sample per render pixel, and a negative bias there only aliases. The rule is the
        // method's (ITemporalUpscaler::MaterialMipBias): UE's screen-percentage bias, or the vendor SDK's own.
        f.MaterialMipBias = upscaler != nullptr ? upscaler->MaterialMipBias( f.Split ) : 0.0f;
        // A method without one keeps no vendor state (UE drops the third-party history with the method).
        if ( upscaler == nullptr )
            m_UpscalerHistory.reset();

        f.HistoryReset = reset;
        f.Parallax     = inputs.Parallax;
        f.SunDirection = inputs.SunDirection;

        m_Pending               = f;
        m_PendingCameraIdentity = inputs.CameraIdentity;
        m_FrameOpen             = true;
        return Common::MakeSuccess( f );
    }

    VolumetricCloudHistory& SceneViewState::Clouds()
    {
        return m_Clouds;
    }

    const VolumetricCloudHistory& SceneViewState::Clouds() const
    {
        return m_Clouds;
    }

    ScreenProbeGatherHistory& SceneViewState::ScreenProbeGather()
    {
        return m_ScreenProbeGather;
    }

    const ScreenProbeGatherHistory& SceneViewState::ScreenProbeGather() const
    {
        return m_ScreenProbeGather;
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
        m_Clouds.EndFrame( report );
        m_ScreenProbeGather.EndFrame( report );
        m_History.Swap();
        m_Motion.EndFrame();
    }

    void SceneViewState::Reset()
    {
        m_Clouds.Reset();
        m_ScreenProbeGather.Reset();
        m_History.Release();
        m_UpscalerHistory.reset();
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

} // namespace Desert::Graphic
