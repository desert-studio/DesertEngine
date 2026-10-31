#include <Engine/Graphic/Deformation/DeformationFieldRenderer.hpp>

#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>
#include <Engine/World/Deformation/DeformationWorld.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <vector>

namespace Desert::Graphic::Deformation
{
    namespace
    {
        constexpr std::string_view kApplyShaderName = "DeformStampApply";
        constexpr std::string_view kViewShaderName  = "DeformationFieldView";
        constexpr std::string_view kStampBodyName   = "Deformation/DeformStampBody.glslh";
        // DeformStampApply's tile (64^2 texels = 8x8 groups of 8x8 threads) and its buffers' bindings.
        constexpr int      kTile             = 64;
        constexpr uint32_t kGroupsPerTileRow = kTile / 8;
        constexpr uint32_t kStampBinding     = 2;
        constexpr uint32_t kTileBinding      = 3;
        constexpr uint32_t kTileStampBinding = 4;
        // The square a contact may write, in radii (DeformStampContract.glslh DEFORM_STAMP_REACH).
        constexpr float kStampReach = 2.0f;

        enum class ApplyMode : int32_t
        {
            Apply    = 0,
            Scroll   = 1,
            ClearAll = 2,
        };

        struct DeformApplyPush
        {
            glm::ivec4 Window{ 0 };   // min cell xy, N, mode
            glm::ivec4 Previous{ 0 }; // previous min cell xy, first tile
            glm::vec4  Level{ 0.0f }; // texel size
        };
        static_assert( sizeof( DeformApplyPush ) == 48 );

        struct DeformViewPush
        {
            glm::mat4  InverseViewProjection{ 1.0f };
            glm::ivec4 Windows{ 0 };
            glm::vec4  Levels{ 0.0f };
        };
        static_assert( sizeof( DeformViewPush ) == 96 );

        int FloorDiv( const int a, const int b )
        {
            return a >= 0 ? a / b : -( ( -a + b - 1 ) / b );
        }

        template <typename T>
        Common::BoolResultStr Upload( ShaderResources::StorageBuffer& buffer, const std::vector<T>& data,
                                      const std::string_view what )
        {
            // An empty list is uploaded as one zero element nobody reads (a zero-sized write has nowhere to go).
            static const T zero{};
            const void*    bytes = data.empty() ? static_cast<const void*>( &zero ) : data.data();
            const auto     size  = static_cast<uint32_t>( std::max<size_t>( 1, data.size() ) * sizeof( T ) );
            if ( const auto wrote = buffer.SetData( bytes, size ); !wrote )
                return Common::MakeFormattedError<bool>( "deformation field: {} upload ({} bytes): {}", what, size,
                                                         wrote.GetError() );
            return BOOLSUCCESS;
        }
    } // namespace

    DeformationLevel DeformationLevelOf( const int level )
    {
        return level == 0 ? DeformationLevel{ .Texel = 2.0f, .Resolution = 2048 }
                          : DeformationLevel{ .Texel = 8.0f, .Resolution = 1024 };
    }

    glm::ivec2 DeformationWindowMin( const glm::vec3& position, const int level )
    {
        const DeformationLevel l = DeformationLevelOf( level );
        const glm::ivec2       cell( static_cast<int>( std::floor( position.x / l.Texel ) ),
                                     static_cast<int>( std::floor( position.z / l.Texel ) ) );
        return cell - glm::ivec2( l.Resolution / 2 );
    }

    DeformationFieldRenderer::DeformationFieldRenderer()  = default;
    DeformationFieldRenderer::~DeformationFieldRenderer() = default;

    Common::BoolResultStr DeformationFieldRenderer::Initialize()
    {
        const auto shaders = Runtime::ResourceRegistry::GetShaderService();
        if ( shaders == nullptr )
            return Common::MakeError<bool>( "deformation field: no shader service" );
        m_ClearShader = shaders->GetByName( std::string( kApplyShaderName ) );
        if ( !m_ClearShader )
            return Common::MakeFormattedError<bool>( "deformation field: shader {} not found", kApplyShaderName );
        if ( const auto valid = m_ClearLayout.Validate( m_ClearShader, DeformStampApplyShaderParameters::kAll );
             !valid )
            return valid;
        auto pipeline = ComputePipeline::Create( { .Shader = m_ClearShader, .DebugName = "DeformStampApply" } );
        if ( !pipeline )
            return Common::MakeError<bool>( pipeline.GetError() );
        m_ClearPipeline = pipeline.GetValue();

        for ( int level = 0; level < kDeformationLevels; ++level )
        {
            const DeformationLevel l    = DeformationLevelOf( level );
            const auto             make = [&]( const std::string_view           what,
                                   const Core::Formats::ImageFormat format ) -> std::shared_ptr<Image2D>
            {
                const Core::Formats::Image2DSpecification spec{
                     .Tag        = std::format( "DeformationField.{}{}", what, level ),
                     .Width      = static_cast<uint32_t>( l.Resolution ),
                     .Height     = static_cast<uint32_t>( l.Resolution ),
                     .Format     = format,
                     .Mips       = 1,
                     .Samples    = 1,
                     .Data       = Core::Formats::EmptyPixelData{},
                     .Usage      = Core::Formats::Image2DUsage::Image2D,
                     .Properties = Core::Formats::Storage | Core::Formats::Sample,
                };
                return Image2D::Create( spec );
            };
            m_Height[level] = make( "Height", Core::Formats::ImageFormat::RGBA16F );
            m_Detail[level] = make( "Detail", Core::Formats::ImageFormat::RGBA8F );
            if ( !m_Height[level] || !m_Detail[level] )
                return Common::MakeFormattedError<bool>( "deformation field: level {} images ({}^2) not created",
                                                         level, l.Resolution );
            m_Window[level].reset();
        }
        // Per-frame buffers: each frame in flight uploads its own copy of the frame's contacts and tiles.
        m_StampBuffer = ShaderResources::StorageBuffer::Create( "DeformStamps", 64, kStampBinding, false );
        m_TileBuffer  = ShaderResources::StorageBuffer::Create( "DeformTiles", 16, kTileBinding, false );
        m_TileStampBuffer =
             ShaderResources::StorageBuffer::Create( "DeformTileStamps", 4, kTileStampBinding, false );
        if ( !m_StampBuffer || !m_TileBuffer || !m_TileStampBuffer )
            return Common::MakeError<bool>( "deformation field: a storage buffer could not be created" );
        return BOOLSUCCESS;
    }

    Common::ResultStr<DeformationFieldRenderer::StampProgram*>
    DeformationFieldRenderer::ProgramOf( const std::string& stamp )
    {
        const auto  shaders = Runtime::ResourceRegistry::GetShaderService();
        std::string source  = shaders->StampSourceOf( stamp );
        if ( source.empty() )
            return Common::MakeFormattedError<StampProgram*>( "no DeformStamp shader named '{}' is registered (a "
                                                              "`Domain DeformStamp` asset with a Stamp block)",
                                                              stamp );
        Core::ShaderVariant variant;
        variant.VirtualSources.push_back(
             { .Name = std::string( kStampBodyName ), .Source = std::move( source ) } );
        const uint64_t hash  = variant.Hash();
        StampProgram&  entry = m_Stamps[stamp];
        if ( entry.Pipeline && entry.SourceHash == hash )
            return Common::MakeSuccess( &entry );
        if ( entry.Pipeline )
        {
            // The stamp was edited (same name, new bytes): the old pipeline may be in a command buffer in flight.
            LOG_INFO( "[Deformation] stamp '{}' changed ({:016x} -> {:016x}); recompiling its program", stamp,
                      entry.SourceHash, hash );
            Renderer::GetInstance().WaitDeviceIdle();
        }
        auto acquired = shaders->AcquireVariant( std::string( kApplyShaderName ), variant );
        if ( !acquired )
            return Common::MakeFormattedError<StampProgram*>( "stamp '{}' does not compile into {}: {}", stamp,
                                                              kApplyShaderName, acquired.GetError() );
        std::shared_ptr<Shader> shader = acquired.ExtractValue();
        if ( const auto valid = entry.Layout.Validate( shader, DeformStampApplyShaderParameters::kAll ); !valid )
            return Common::MakeFormattedError<StampProgram*>( "stamp '{}': {}", stamp, valid.GetError() );
        auto pipeline = ComputePipeline::Create(
             { .Shader = shader, .DebugName = std::format( "DeformStampApply {}", stamp ) } );
        if ( !pipeline )
            return Common::MakeFormattedError<StampProgram*>( "stamp '{}': {}", stamp, pipeline.GetError() );
        entry.Shader     = std::move( shader );
        entry.Pipeline   = pipeline.GetValue();
        entry.SourceHash = hash;
        return Common::MakeSuccess( &entry );
    }

    void DeformationFieldRenderer::BindBuffers( ComputePipeline& pipeline ) const
    {
        pipeline.SetStorageBuffer( kStampBinding, m_StampBuffer.get() );
        pipeline.SetStorageBuffer( kTileBinding, m_TileBuffer.get() );
        pipeline.SetStorageBuffer( kTileStampBinding, m_TileStampBuffer.get() );
    }

    Common::ResultStr<std::optional<DeformationFieldFrame>>
    DeformationFieldRenderer::AddUpdatePasses( RDG::Builder& graph, const ImportImage& import,
                                               const ::Desert::World::DeformationWorld& world, const glm::vec3& viewPosition,
                                               const bool wanted )
    {
        using Result = Common::ResultStr<std::optional<DeformationFieldFrame>>;
        // Another world behind this view (a scene was opened): its log restarts; what the field holds is not its.
        if ( world.NextSequence() < m_Applied )
        {
            m_Applied = 0;
            for ( auto& window : m_Window )
                window.reset();
        }
        const bool pending = world.NextSequence() > m_Applied;
        if ( !m_ClearPipeline )
        {
            if ( !pending && !wanted )
                return Result( std::optional<DeformationFieldFrame>{} ); // never deformed: zero cost
            if ( const auto initialized = Initialize(); !initialized )
                return Common::MakeError<std::optional<DeformationFieldFrame>>( initialized.GetError() );
        }

        DeformationFieldFrame frame;
        for ( int level = 0; level < kDeformationLevels; ++level )
        {
            frame.Height[level]    = import( m_Height[level], std::format( "DeformationField.Height{}", level ) );
            frame.Detail[level]    = import( m_Detail[level], std::format( "DeformationField.Detail{}", level ) );
            frame.WindowMin[level] = DeformationWindowMin( viewPosition, level );
        }

        // THE SCROLL: a level whose window moved clears the texels whose cell left it (all of them the first
        // time).
        ComputePipeline* const clearPipeline = m_ClearPipeline.get();
        for ( int level = 0; level < kDeformationLevels; ++level )
        {
            const glm::ivec2 window = frame.WindowMin[level];
            if ( m_Window[level] && *m_Window[level] == window )
                continue;
            const DeformationLevel l = DeformationLevelOf( level );
            DeformApplyPush        push;
            const ApplyMode        mode  = m_Window[level] ? ApplyMode::Scroll : ApplyMode::ClearAll;
            push.Window                  = glm::ivec4( window, l.Resolution, static_cast<int32_t>( mode ) );
            push.Previous                = glm::ivec4( m_Window[level].value_or( window ), 0, 0 );
            push.Level                   = glm::vec4( l.Texel, 0.0f, 0.0f, 0.0f );
            m_Window[level]              = window;
            const RDG::TextureRef height = frame.Height[level];
            const RDG::TextureRef detail = frame.Detail[level];
            const uint32_t        groups = static_cast<uint32_t>( l.Resolution / 8 );
            BindBuffers( *clearPipeline );
            graph.AddPass(
                 std::format( "Deformation: {} L{}", mode == ApplyMode::Scroll ? "Scroll" : "Clear", level ),
                 RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_ClearLayout.Get( m_ClearShader ),
                                    Renderer::GetPipelineRouteFill( *clearPipeline ) )
                          .Storage( DeformStampApplyShaderParameters::DeformHeightTexture, height,
                                    RDG::Access::StorageWrite )
                          .Storage( DeformStampApplyShaderParameters::DeformDetailTexture, detail,
                                    RDG::Access::StorageWrite )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( DeformApplyPush ) ) );
                 },
                 [clearPipeline, push, groups]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     return Renderer::DispatchCompute( bindings, *clearPipeline, groups, groups, 1 );
                 } );
        }
        if ( !pending )
            return Result( std::optional<DeformationFieldFrame>{ frame } );

        // THE NEW CONTACTS, in submission order, cut into batches that name the same stamp.
        uint64_t                                missed   = 0;
        const std::vector<::Desert::World::DeformContact> contacts = world.Since( m_Applied, missed );
        m_Applied                                        = world.NextSequence();
        if ( missed > 0 )
            LOG_ERROR( "[Deformation] this view's field fell {} contacts behind the world's log (it keeps {}); "
                       "those contacts are not in the field",
                       missed, ::Desert::World::DeformationWorld::kMaxRetained );

        struct Dispatch
        {
            StampProgram* Program   = nullptr;
            int           Level     = 0;
            int32_t       FirstTile = 0;
            uint32_t      TileCount = 0;
        };
        std::vector<glm::vec4>  stamps;
        std::vector<glm::ivec4> tiles;
        std::vector<uint32_t>   tileStamps;
        std::vector<Dispatch>   dispatches;
        stamps.reserve( contacts.size() * 4 );
        for ( std::size_t begin = 0; begin < contacts.size(); )
        {
            std::size_t end = begin + 1;
            while ( end < contacts.size() && contacts[end].Stamp == contacts[begin].Stamp )
                ++end;
            const auto program = ProgramOf( contacts[begin].Stamp );
            if ( !program )
            {
                LOG_ERROR( "[Deformation] {} contact(s) not applied: {}", end - begin, program.GetError() );
                begin = end;
                continue;
            }
            const auto firstStamp = static_cast<uint32_t>( stamps.size() / 4 );
            for ( std::size_t c = begin; c < end; ++c )
            {
                const ::Desert::World::DeformContact&     contact = contacts[c];
                const ::Desert::World::DeformStampParams& p       = contact.Params;
                stamps.emplace_back( contact.At, p.Radius );
                stamps.emplace_back( p.Pressure, p.Speed, p.Heading, p.Mass );
                stamps.emplace_back( p.SlipVector.x, p.SlipVector.y, p.Age, p.Seed );
                stamps.emplace_back( p.ContactNormal, 0.0f );
            }
            for ( int level = 0; level < kDeformationLevels; ++level )
            {
                const DeformationLevel l  = DeformationLevelOf( level );
                const glm::ivec2       lo = frame.WindowMin[level];
                const glm::ivec2       hi = lo + glm::ivec2( l.Resolution - 1 );
                // tile -> its contacts, in submission order (std::map: a stable tile order frame to frame).
                std::map<std::pair<int, int>, std::vector<uint32_t>> binned;
                for ( std::size_t c = begin; c < end; ++c )
                {
                    const ::Desert::World::DeformContact& contact = contacts[c];
                    const float                 reach   = contact.Params.Radius * kStampReach;
                    glm::ivec2 a( static_cast<int>( std::floor( ( contact.At.x - reach ) / l.Texel ) ),
                                  static_cast<int>( std::floor( ( contact.At.z - reach ) / l.Texel ) ) );
                    glm::ivec2 b( static_cast<int>( std::floor( ( contact.At.x + reach ) / l.Texel ) ),
                                  static_cast<int>( std::floor( ( contact.At.z + reach ) / l.Texel ) ) );
                    a = glm::max( a, lo );
                    b = glm::min( b, hi );
                    if ( a.x > b.x || a.y > b.y )
                        continue; // outside this level's window: the level never sees it (DEFORM-PERSIST keeps it)
                    for ( int ty = FloorDiv( a.y, kTile ); ty <= FloorDiv( b.y, kTile ); ++ty )
                        for ( int tx = FloorDiv( a.x, kTile ); tx <= FloorDiv( b.x, kTile ); ++tx )
                            binned[{ tx, ty }].push_back( firstStamp + static_cast<uint32_t>( c - begin ) );
                }
                if ( binned.empty() )
                    continue;
                Dispatch dispatch{ .Program   = program.GetValue(),
                                   .Level     = level,
                                   .FirstTile = static_cast<int32_t>( tiles.size() ),
                                   .TileCount = static_cast<uint32_t>( binned.size() ) };
                for ( const auto& [tile, list] : binned )
                {
                    tiles.emplace_back( tile.first, tile.second, static_cast<int32_t>( tileStamps.size() ),
                                        static_cast<int32_t>( list.size() ) );
                    tileStamps.insert( tileStamps.end(), list.begin(), list.end() );
                }
                dispatches.push_back( dispatch );
            }
            begin = end;
        }
        if ( dispatches.empty() )
            return Result( std::optional<DeformationFieldFrame>{ frame } );

        if ( auto r = Upload( *m_StampBuffer, stamps, "contacts" ); !r )
            return Common::MakeError<std::optional<DeformationFieldFrame>>( r.GetError() );
        if ( auto r = Upload( *m_TileBuffer, tiles, "tiles" ); !r )
            return Common::MakeError<std::optional<DeformationFieldFrame>>( r.GetError() );
        if ( auto r = Upload( *m_TileStampBuffer, tileStamps, "tile contact lists" ); !r )
            return Common::MakeError<std::optional<DeformationFieldFrame>>( r.GetError() );

        // ONE DISPATCH PER BATCH PER LEVEL, in batch order; consecutive dispatches on one level are ordered by the
        // graph (StorageWrite after StorageWrite), and within one a texel belongs to one tile, so to one thread.
        for ( const Dispatch& dispatch : dispatches )
        {
            const DeformationLevel l = DeformationLevelOf( dispatch.Level );
            DeformApplyPush        push;
            push.Window                     = glm::ivec4( frame.WindowMin[dispatch.Level], l.Resolution,
                                                          static_cast<int32_t>( ApplyMode::Apply ) );
            push.Previous                   = glm::ivec4( 0, 0, dispatch.FirstTile, 0 );
            push.Level                      = glm::vec4( l.Texel, 0.0f, 0.0f, 0.0f );
            StampProgram* const    program  = dispatch.Program;
            ComputePipeline* const pipeline = program->Pipeline.get();
            const RDG::TextureRef  height   = frame.Height[dispatch.Level];
            const RDG::TextureRef  detail   = frame.Detail[dispatch.Level];
            const uint32_t         count    = dispatch.TileCount;
            BindBuffers( *pipeline );
            graph.AddPass(
                 std::format( "Deformation: Stamp L{} ({} tiles)", dispatch.Level, count ),
                 RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( program->Layout.Get( program->Shader ),
                                    Renderer::GetPipelineRouteFill( *pipeline ) )
                          .Storage( DeformStampApplyShaderParameters::DeformHeightTexture, height,
                                    RDG::Access::StorageWrite )
                          .Storage( DeformStampApplyShaderParameters::DeformDetailTexture, detail,
                                    RDG::Access::StorageWrite )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( DeformApplyPush ) ) );
                 },
                 [pipeline, push, count]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, kGroupsPerTileRow, kGroupsPerTileRow,
                                                       count );
                 } );
        }
        return Result( std::optional<DeformationFieldFrame>{ frame } );
    }

    Common::BoolResultStr
    DeformationFieldRenderer::AddDebugViewPass( RDG::Builder& graph, const DeformationFieldFrame& field,
                                                const RDG::TextureRef depth, const RDG::TextureRef target,
                                                const RDG::LoadOp& load, const glm::mat4& inverseViewProjection )
    {
        if ( !m_ViewShader )
        {
            m_ViewShader =
                 Runtime::ResourceRegistry::GetShaderService()->GetByName( std::string( kViewShaderName ) );
            if ( !m_ViewShader )
                return Common::MakeFormattedError<bool>( "shader {} not found", kViewShaderName );
            if ( const auto valid =
                      m_ViewLayout.Validate( m_ViewShader, DeformationFieldViewShaderParameters::kAll );
                 !valid )
                return valid;
        }
        const auto desc = graph.GetTextureDesc( target );
        if ( !desc )
            return Common::MakeError<bool>( desc.GetError() );
        const auto key = std::make_pair( static_cast<int>( desc.GetValue().Format ), desc.GetValue().Samples );
        auto       it  = m_ViewPipelines.find( key );
        if ( it == m_ViewPipelines.end() )
        {
            GraphicsPipelineSpecification spec;
            spec.DebugName         = "DeformationFieldView";
            spec.TargetLayout      = RenderTargetLayout{ .ColorFormats = { desc.GetValue().Format },
                                                         .DepthFormat  = std::nullopt,
                                                         .Samples      = desc.GetValue().Samples };
            spec.Shader            = m_ViewShader;
            spec.DepthTestEnabled  = false;
            spec.DepthWriteEnabled = false;
            const auto pipeline    = GraphicsPipeline::Create( spec );
            if ( !pipeline )
                return Common::MakeError<bool>( pipeline.GetError() );
            it = m_ViewPipelines.emplace( key, pipeline.GetValue() ).first;
        }
        DeformViewPush push;
        push.InverseViewProjection       = inverseViewProjection;
        push.Windows                     = glm::ivec4( field.WindowMin[0], field.WindowMin[1] );
        push.Levels                      = glm::vec4( DeformationLevelOf( 0 ).Texel, DeformationLevelOf( 1 ).Texel,
                                                      static_cast<float>( DeformationLevelOf( 0 ).Resolution ),
                                                      static_cast<float>( DeformationLevelOf( 1 ).Resolution ) );
        GraphicsPipeline* const pipeline = it->second.get();
        graph.AddPass(
             "Debug: Deformation Field", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( m_ViewLayout.Get( m_ViewShader ), RDG::OtherRouteFill{} )
                      .Sampler( DeformationFieldViewShaderParameters::PointClampSampler,
                                RDG::SamplerDesc::PointClamp() )
                      .Texture( DeformationFieldViewShaderParameters::DeformHeight0Texture, field.Height[0],
                                RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
                      .Texture( DeformationFieldViewShaderParameters::DeformHeight1Texture, field.Height[1],
                                RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
                      .Texture( DeformationFieldViewShaderParameters::SceneDepthTexture, depth,
                                RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
                      .PushConstantBytes( static_cast<uint32_t>( sizeof( DeformViewPush ) ) );
                 pass.ColorTarget( 0, target, load );
             },
             [pipeline, push]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                 return Renderer::GetInstance().DrawFullscreen( bindings, *pipeline, nullptr );
             } );
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::Deformation
