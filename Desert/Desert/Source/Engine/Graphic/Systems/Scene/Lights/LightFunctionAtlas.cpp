#include "LightFunctionAtlas.hpp"

#include <Engine/Core/FrameManager.hpp>
#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Core/Formats/ShaderProgramMeta.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Materials/MaterialExecutor.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>
#include <Engine/Graphic/PipelineCache.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Shader.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Core/Profiler.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>

namespace Desert::Graphic::System
{
    namespace
    {
        // The engine's light function (UE: a light function material with no expression is the identity; ours
        // draws stripes so a light given the default shows that it works): Programs/LightFunctions/.
        constexpr const char* kDefaultLightFunctionShaderName = "DefaultLightFunction";
        constexpr uint32_t    kQuadVertexCount                = 6u;
        // LightFunctionPush { uint FirstRow; three pads }; a material's own push values may follow, up to 128.
        constexpr std::size_t kLightFunctionPushBytes = 16u;
        constexpr std::size_t kMaxPushBytes           = 128u;
        constexpr std::size_t kInitialTiles           = 64u;
        constexpr std::size_t kInitialPrograms        = 8u;
        constexpr float       kKindSpot               = 0.0f;
        constexpr float       kKindOmni               = 1.0f;

        glm::vec3 SafeNormalize( const glm::vec3& v, const glm::vec3& fallback )
        {
            const float length = glm::length( v );
            return length > 1.0e-6f ? v / length : fallback;
        }

        glm::vec4 InverseScale( const glm::vec3& scale, const float w )
        {
            const auto inverse = []( const float s ) { return s > 1.0e-3f ? 1.0f / s : 1.0e3f; };
            return glm::vec4( inverse( scale.x ), inverse( scale.y ), inverse( scale.z ), w );
        }
    } // namespace

    LightFunctionAtlas::LightFunctionAtlas()
    {
        m_Tiles.reserve( kInitialTiles );
        m_Frame.reserve( kInitialTiles );
        m_Rows.reserve( kInitialTiles );
        m_Draws.reserve( kInitialTiles );
        m_Programs.reserve( kInitialPrograms );
        m_ProgramsByMaterial.reserve( kInitialPrograms );
    }

    int32_t LightFunctionAtlas::Push( const LightFunctionProxy& proxy, const LightFunctionRowGpu& row )
    {
        if ( proxy.Material.IsNull() )
            return -1;
        m_Tiles.push_back( Tile{ .Material = proxy.Material, .Row = row } );
        return static_cast<int32_t>( m_Tiles.size() - 1u );
    }

    int32_t LightFunctionAtlas::Add( const LightFunctionProxy&                proxy,
                                     const ShaderProtocols::SpotLightPayload& light )
    {
        // The reader's spot frame (Mesh/LightFunctionAtlas.glslh DesertSpotLightFunction): forward = Direction,
        // tangent = Tangent, bitangent = cross, the tile spanning tan(outer cone).
        const glm::vec3     forward  = SafeNormalize( light.Direction, glm::vec3( 0.0f, 0.0f, -1.0f ) );
        const glm::vec3     tangent  = SafeNormalize( light.Tangent, glm::vec3( 1.0f, 0.0f, 0.0f ) );
        const float         cosOuter = std::max( light.CosOuter, 1.0e-4f );
        const float         tanOuter = std::sqrt( std::max( 1.0f - cosOuter * cosOuter, 0.0f ) ) / cosOuter;
        LightFunctionRowGpu row{};
        row.Forward   = glm::vec4( forward, kKindSpot );
        row.Tangent   = glm::vec4( tangent, tanOuter );
        row.Bitangent = glm::vec4( glm::cross( forward, tangent ), 0.0f );
        row.Position  = glm::vec4( light.Position, light.Range );
        row.InvScale  = InverseScale( proxy.Scale, 0.0f );
        return Push( proxy, row );
    }

    int32_t LightFunctionAtlas::Add( const LightFunctionProxy&                 proxy,
                                     const ShaderProtocols::PointLightPayload& light )
    {
        // The reader's omni frame (LightSources.glslh): forward = cross(AxisX, AxisY), tangent = AxisX, as the
        // payload holds them (unnormalised exactly as the reader takes them).
        const glm::vec3     forward = glm::cross( light.AxisX, light.AxisY );
        LightFunctionRowGpu row{};
        row.Forward   = glm::vec4( forward, kKindOmni );
        row.Tangent   = glm::vec4( light.AxisX, 0.0f );
        row.Bitangent = glm::vec4( glm::cross( forward, light.AxisX ), 0.0f );
        row.Position  = glm::vec4( light.Position, light.Radius );
        row.InvScale  = InverseScale( proxy.Scale, 0.0f );
        return Push( proxy, row );
    }

    void LightFunctionAtlas::ClearQueue()
    {
        m_Tiles.clear();
    }

    Common::BoolResultStr LightFunctionAtlas::EnsureAtlas( const uint32_t tilesPerSide )
    {
        if ( m_Atlas && tilesPerSide <= m_TilesPerSide )
            return BOOLSUCCESS;
        const uint32_t                            side  = tilesPerSide * kLightFunctionTileTexels;
        const Core::Formats::Image2DSpecification spec  = { .Tag        = "LightFunctionAtlas",
                                                            .Width      = side,
                                                            .Height     = side,
                                                            .Format     = Core::Formats::ImageFormat::RGBA8F,
                                                            .Usage      = Core::Formats::Image2DUsage::Attachment,
                                                            .Properties = Core::Formats::Sample |
                                                                          Core::Formats::GraphDeclaredBindless };
        std::shared_ptr<Image2D>                  atlas = Image2D::Create( spec );
        if ( !atlas || !atlas->GetBindlessSlot().IsValid() )
            return Common::MakeFormattedError(
                 "the {}x{} light function atlas was not created with a bindless slot", side, side );
        // The old atlas may still be sampled by a frame in flight.
        if ( m_Atlas )
            m_RetiredAtlases.Park( std::move( m_Atlas ),
                                   ::Desert::Engine::FrameManager::GetInstance().GetAbsoluteFrameCount() );
        m_Atlas        = std::move( atlas );
        m_TilesPerSide = tilesPerSide;
        return BOOLSUCCESS;
    }

    int32_t LightFunctionAtlas::PrepareFrame( const float timeSeconds, PipelineCache& pipelines )
    {
        m_Frame.clear();
        m_Rows.clear();
        m_Draws.clear();
        m_Programs.clear();
        const auto& frames = ::Desert::Engine::FrameManager::GetInstance();
        m_Retired.Collect( frames.GetAbsoluteFrameCount(), frames.GetMaxFramesInFlight() );
        m_RetiredAtlases.Collect( frames.GetAbsoluteFrameCount(), frames.GetMaxFramesInFlight() );
        if ( m_Tiles.empty() )
            return 0;

        uint32_t tilesPerSide = 1u;
        while ( static_cast<std::size_t>( tilesPerSide ) * tilesPerSide < m_Tiles.size() )
            tilesPerSide *= 2u;
        if ( const auto ensured = EnsureAtlas( tilesPerSide ); !ensured )
        {
            if ( !m_AtlasRefusedLogged )
                LOG_ERROR( "[LightFunctions] {} - lights with a light function shade without it",
                           ensured.GetError() );
            m_AtlasRefusedLogged = true;
            return 0;
        }
        m_AtlasRefusedLogged = false;

        // Tiles in the atlas's own grid (the atlas may be larger than this frame needs: it only grows).
        const float tileUV = 1.0f / static_cast<float>( m_TilesPerSide );
        for ( std::size_t index = 0; index < m_Tiles.size(); ++index )
        {
            const Program* program = ProgramFor( m_Tiles[index].Material, pipelines );
            if ( program == nullptr )
                continue; // cleared to 1: the light shades as if it had none
            const auto          tile = static_cast<uint32_t>( index );
            const float         x0   = static_cast<float>( tile % m_TilesPerSide ) * tileUV;
            const float         y0   = static_cast<float>( tile / m_TilesPerSide ) * tileUV;
            LightFunctionRowGpu row  = m_Tiles[index].Row;
            row.InvScale.w           = timeSeconds;
            row.TileUV               = glm::vec4( x0, y0, x0 + tileUV, y0 + tileUV );
            m_Frame.push_back( FrameTile{ .Program = program, .Row = row } );
        }

        // Blocks in first-seen order, then the tiles grouped by block (stable: the frame is the same every time).
        for ( const FrameTile& frame : m_Frame )
            if ( std::find( m_Programs.begin(), m_Programs.end(), frame.Program ) == m_Programs.end() )
            {
                frame.Program->Block = static_cast<uint32_t>( m_Programs.size() );
                m_Programs.push_back( frame.Program );
            }
        std::stable_sort( m_Frame.begin(), m_Frame.end(), []( const FrameTile& a, const FrameTile& b )
                          { return a.Program->Block < b.Program->Block; } );
        for ( const FrameTile& frame : m_Frame )
        {
            const auto row = static_cast<uint32_t>( m_Rows.size() );
            m_Rows.push_back( frame.Row );
            if ( !m_Draws.empty() && m_Draws.back().Program == frame.Program )
            {
                ++m_Draws.back().Count;
                continue;
            }
            m_Draws.push_back( Draw{ .Program = frame.Program, .FirstRow = row, .Count = 1u } );
        }
        return static_cast<int32_t>( m_Atlas->GetBindlessSlot().Index );
    }

    const LightFunctionAtlas::Program* LightFunctionAtlas::ProgramFor( const Assets::AssetHandle& material,
                                                                       PipelineCache&             pipelines )
    {
        const auto key = static_cast<uint64_t>( material );
        if ( const auto found = m_ProgramsByMaterial.find( key ); found != m_ProgramsByMaterial.end() )
        {
            Program& program = found->second;
            if ( program.Refused )
                return RetryRefused( program, material, pipelines );
            FollowShaderReload( program, material, pipelines );
            return &program;
        }
        // A material still loading is asked again next frame rather than remembered as refused.
        const auto* materials = Runtime::ResourceRegistry::GetMaterialService();
        if ( materials != nullptr && materials->IsPending( material ) )
            return nullptr;

        Program& program = m_ProgramsByMaterial[key];
        if ( const auto built = Build( program, material, pipelines ); !built )
        {
            LOG_ERROR( "[LightFunctions] material {}: {} - the light shades without its light function", key,
                       built.GetError() );
            program.Pipeline.reset();
            program.Material.reset();
            program.Refused = true;
            return nullptr;
        }
        return &program;
    }

    const LightFunctionAtlas::Program* LightFunctionAtlas::RetryRefused( Program&                   program,
                                                                         const Assets::AssetHandle& material,
                                                                         PipelineCache&             pipelines )
    {
        // A refusal stands until its shader recompiles (DecalRenderer::RetryRefused's rule).
        if ( !program.BuiltShader || program.BuiltShader->GetCodeGeneration() == program.Generation )
            return nullptr;
        program.Generation = program.BuiltShader->GetCodeGeneration();
        if ( const auto built = Build( program, material, pipelines ); !built )
        {
            LOG_ERROR( "[LightFunctions] material {}: still refused after its shader reloaded: {}",
                       static_cast<uint64_t>( material ), built.GetError() );
            program.Pipeline.reset();
            program.Material.reset();
            return nullptr;
        }
        program.Refused = false;
        return &program;
    }

    void LightFunctionAtlas::FollowShaderReload( Program& program, const Assets::AssetHandle& material,
                                                 PipelineCache& pipelines )
    {
        // DecalRenderer::FollowShaderReload's rule: rebuild against the reloaded code, park the replaced build.
        if ( !program.Pipeline || !program.Pipeline->GetSpecification().Shader )
            return;
        const uint64_t generation = program.Pipeline->GetSpecification().Shader->GetCodeGeneration();
        if ( generation == program.Generation )
            return;
        std::shared_ptr<GraphicsPipeline>   previousPipeline = program.Pipeline;
        std::unique_ptr<DataDrivenMaterial> previousMaterial = std::move( program.Material );
        if ( const auto rebuilt = Build( program, material, pipelines ); !rebuilt )
        {
            LOG_ERROR(
                 "[LightFunctions] material {}: did not rebuild after its shader reloaded ({}); it keeps the "
                 "previous build",
                 static_cast<uint64_t>( material ), rebuilt.GetError() );
            program.Pipeline   = std::move( previousPipeline );
            program.Material   = std::move( previousMaterial );
            program.Generation = generation;
            return;
        }
        m_Retired.Park( RetiredProgram{ std::move( previousPipeline ), std::move( previousMaterial ) },
                        ::Desert::Engine::FrameManager::GetInstance().GetAbsoluteFrameCount() );
    }

    Common::BoolResultStr LightFunctionAtlas::Build( Program& program, const Assets::AssetHandle& material,
                                                     PipelineCache& pipelines ) const
    {
        auto* shaders   = Runtime::ResourceRegistry::GetShaderService();
        auto* materials = Runtime::ResourceRegistry::GetMaterialService();
        if ( shaders == nullptr || materials == nullptr )
            return Common::MakeError( "no shader or material service" );
        const auto materialTemplate = materials->ShaderHandleOf( material );
        if ( materialTemplate.Shader.IsNull() || materialTemplate.CompileName.empty() )
            return Common::MakeError( "the handle names no material asset with a loaded template" );
        const std::string shaderName = materialTemplate.CompileName;
        const auto        shader     = shaders->GetByName( shaderName );
        if ( !shader )
            return Common::MakeFormattedError( "no shader named '{}' is registered (the engine's is "
                                               "Editor/Resources/Shaders/Programs/LightFunctions/{}.shader)",
                                               shaderName, kDefaultLightFunctionShaderName );
        program.BuiltShader = shader;
        program.Generation  = shader->GetCodeGeneration();
        const auto& meta    = shader->GetProgramMeta();
        if ( !Core::Formats::DrawnByLightFunctionPath( meta.Domain ) )
            return Common::MakeFormattedError(
                 "shader '{}' has Domain {}, and a light function draws only a Domain {} shader", shaderName,
                 Core::Formats::ShaderDomainName( meta.Domain ),
                 Core::Formats::ShaderDomainName( Core::Formats::kLightFunctionPathDomain ) );
        if ( const auto valid = program.Layout.Validate( shader, LightFunctionShaderParameters::kAll ); !valid )
            return valid;

        // One opaque quad per tile, no depth, no blend: the tile is the function's value.
        GraphicsPipelineSpecification spec;
        spec.DebugName         = "LightFunction:" + shaderName;
        spec.Shader            = shader;
        spec.TargetLayout      = RenderTargetLayout{ { Core::Formats::ImageFormat::RGBA8F }, {}, 1 };
        spec.Topology          = PrimitiveTopology::Triangles;
        spec.CullMode          = CullMode::None;
        spec.DepthTestEnabled  = false;
        spec.DepthWriteEnabled = false;
        spec.BlendEnable       = false;
        const auto pipeline    = pipelines.GetOrCreate( spec );
        if ( !pipeline )
            return Common::MakeError( std::string( pipeline.GetError() ) );
        program.Pipeline   = pipeline.GetValue();
        program.Material   = std::make_unique<DataDrivenMaterial>( shader );
        program.Generation = shader->GetCodeGeneration();

        // The asset's values by name onto the program's runtime material (DecalRenderer::Build's route).
        MaterialOverrides overrides;
        if ( !materials->ResolveOverrides( material, overrides ) )
            return BOOLSUCCESS;
        for ( const auto& [name, value] : overrides.Params )
            if ( !program.Material->SetParam( name, value ) )
                LOG_WARN( "[LightFunctions] '{}' has no parameter '{}' - the value in the .demat is ignored",
                          shaderName, name );
        return BOOLSUCCESS;
    }

    RDG::BufferRef LightFunctionAtlas::UploadRows( RDG::Builder& graph ) const
    {
        const RDG::BufferRef rows = graph.CreateBuffer(
             RDG::BufferDesc{ m_Rows.size() * sizeof( LightFunctionRowGpu ) }, "LightFunctions.Rows" );
        graph.QueueBufferUpload( rows, std::as_bytes( std::span( m_Rows ) ) );
        return rows;
    }

    void LightFunctionAtlas::Declare( RDG::PassBuilder& pass, const RDG::BufferRef rows ) const
    {
        for ( const Program* program : m_Programs )
        {
            const MaterialExecutor* executor = program->Material->GetMaterialExecutor();
            pass.Prepares( *executor );
            const auto layout = program->Layout.Get( program->Pipeline->GetSpecification().Shader );
            pass.Bindings( layout, executor->GetRouteFill() )
                 .Storage( LightFunctionShaderParameters::LightFunctionRowsBuffer, rows,
                           RDG::Access::StorageRead );
        }
    }

    Common::BoolResultStr LightFunctionAtlas::Render( const RDG::PassContext& context ) const
    {
        DESERT_PROFILE_PASS( "Lights: Light Function Atlas" );
        for ( const Draw& draw : m_Draws )
        {
            const Program&                program = *draw.Program;
            const RDG::PassBindings       bindings( context, context.GetBindingBlock( program.Block ) );
            const MaterialExecutor*       executor = program.Material->GetMaterialExecutor();
            const Common::Memory::Buffer& pushed   = executor->GetPushConstantBuffer();
            if ( pushed.Size > kMaxPushBytes )
                return Common::MakeFormattedError( "{}: light function shader '{}' has a {}-byte push block (at "
                                                   "most {})",
                                                   context.GetPassName(), program.Material->GetShaderName(),
                                                   pushed.Size, kMaxPushBytes );
            std::array<std::byte, kMaxPushBytes> push{};
            if ( pushed.Data != nullptr && pushed.Size != 0u )
                std::memcpy( push.data(), pushed.Data, pushed.Size );
            std::memcpy( push.data(), &draw.FirstRow, sizeof( draw.FirstRow ) );
            const std::size_t                bytes = std::max<std::size_t>( pushed.Size, kLightFunctionPushBytes );
            const std::span<const std::byte> block( push.data(), bytes );
            if ( auto drawn = Renderer::DrawProcedural( bindings, *program.Pipeline, executor, kQuadVertexCount,
                                                        draw.Count, block );
                 !drawn.IsSuccess() )
                return drawn;
        }
        return BOOLSUCCESS;
    }

    void LightFunctionAtlas::AddPasses( RDG::Builder& graph, const RDG::TextureRef atlas ) const
    {
        if ( m_Draws.empty() || !atlas.IsValid() )
            return;
        const RDG::BufferRef rows = UploadRows( graph );
        // Cleared to 1, the identity: a tile whose program is still loading leaves its light unfiltered.
        graph.AddPass(
             "Lights: Light Function Atlas", RDG::PassFlags::Raster | RDG::PassFlags::NeverCull,
             [this, atlas, rows]( RDG::PassBuilder& pass )
             {
                 pass.ColorTarget( 0, atlas, RDG::LoadOp::ClearColor( 1.0f, 1.0f, 1.0f, 1.0f ) );
                 Declare( pass, rows );
             },
             [this]( RDG::PassContext& context ) -> Common::BoolResultStr { return Render( context ); } );
        // Every reader samples the atlas bindless (Mesh/LightFunctionAtlas.glslh) and declares it in its own pass
        // (ViewLightBuffers::LightFunctionAtlas -> BindlessRead: the lit raster passes, MegaLights, fog, radiance
        // cache, surface cache, screen probes, SSR, path tracer), so the graph orders each after this writer.
    }
} // namespace Desert::Graphic::System
