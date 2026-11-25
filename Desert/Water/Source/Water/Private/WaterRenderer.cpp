#include <Water/Public/WaterRenderer.hpp>

#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ViewTargetLayouts.hpp>
#include <Engine/Graphic/Materials/Properties/StorageBufferProperty.hpp>
#include <Engine/Graphic/PipelineCache.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Runtime/Services/Texture/TextureService.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>
#include <Engine/Graphic/Materials/DrawPushFields.hpp>
#include <Engine/Graphic/Texture.hpp>
#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/Core/Frustum.hpp>
#include <Engine/Graphic/VisibilityCulling.hpp>
#include <Water/Public/GerstnerWaterWaves.hpp>
#include <Water/Public/WaterRiver.hpp>
#include <Water/Public/FFTSurfaceMirror.hpp>
#include <Water/Public/WaterBodyQuery.hpp>
#include <Engine/Graphic/Environment/SkyLook.hpp>
#include <Engine/Generated/ShaderPushConstants.gen.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Common/Core/Profiler.hpp>

#include <cmath>
#include <cstddef>
#include <limits>
#include <cstring>
#include <format>
#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <Engine/Runtime/Services/Image/ImageService.hpp>
#include <Water/Public/WaterWavesService.hpp>
#include <Common/Settings/Scalability.hpp>

#include <span>

namespace
{
    // The slot names the two water cells' passes bind, spelled once (RDGShaderParameters.hpp; UE
    // FSingleLayerWaterPassParameters). The material's own (route fill): WaterTiles, Materials, the push block.
    struct WaterShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, WaterTiles );
        DESERT_SHADER_PARAMETER( StorageBuffer, Materials ); // Core::Formats::kMaterialRowBlockName
        // Water/WaterDepthField.glslh binding 25 and WaterRiver.glslh's texture: fetched by texel, no sampler.
        DESERT_SHADER_PARAMETER( Texture, WaterShoreTexture );
        DESERT_SHADER_PARAMETER( Texture, WaterRiverTexture );
        // Pass_SingleLayerWater.glslh only: the scene behind the water and the one state its cubes, BRDF LUT,
        // scene copy and front-layer reflection are read through.
        DESERT_SHADER_PARAMETER( Texture, SceneColorTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        // Mesh/Surface/FrontLayerReflection.glslh, as the glass cells bind them (MeshRendererForward.cpp).
        DESERT_SHADER_PARAMETER( Texture, FrontLayerReflectionTexture );
        DESERT_SHADER_PARAMETER( Texture, FrontLayerDepthTexture );
        // Water/WaterSurfaceField.glslh binding 23: the waves' array (both cells, both stages) and its state.
        DESERT_SHADER_PARAMETER( Texture, WaterWavesTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearRepeatSampler );
        // Water/WaterFoamField.glslh (the water's own cell): this frame's foam.
        DESERT_SHADER_PARAMETER( Texture, WaterFoamTexture );
    };
    static_assert( WaterShaderParameters::FrontLayerReflectionTexture.Name ==
                   ::Desert::Graphic::kPassFrontLayerReflectionTextureName );
    static_assert( WaterShaderParameters::SceneColorTexture.Name ==
                   ::Desert::Graphic::kPassSceneColorTextureName );
} // namespace

namespace Desert::Graphic::System
{
    namespace
    {
        // One drawn quadtree tile, std430 (Mesh/Surface/WaterMeshTiles.glslh WaterTileRow).
        struct WaterTileRow
        {
            glm::vec4 Tile; // xy = lower (X, Z) corner relative to the zone origin, z = still level Y, w = side
            glm::vec4 Lod;  // x = LOD level, y = morphs, z = can morph twice, w = height LOD factor
            glm::vec4 Zone; // xy = the zone's world (X, Z) origin, z = LOD scale (cm), w = quads per side
            // The tile's body (UE WaterBodyIndex; TileWalk::Bodies): x = its material's row, y = 1 for a river,
            // -1 for an ocean drawn under a river's cut leaf (Water::WaterTileInstance::UnderRiver),
            // z = 1 + its wave set in WaterWavesBlock::Sets (0 = flat), w = the body's index in its zone.
            glm::vec4 Body;
            // The tile's body's medium (WaterBodyLane::Medium): x unused, y = its river's two-phase flow
            // period (s; 0 off a river), zw = a fall's speeds (PrepareTiles).
            glm::vec4 Medium;
        };
        static_assert( sizeof( WaterTileRow ) == 5 * sizeof( glm::vec4 ), "std430 rows of five vec4" );

        // One Gerstner wave, std430 (Mesh/Surface/WaterMeshTiles.glslh WaterWaveRow).
        struct WaterWaveRow
        {
            glm::vec4 Wave; // xy = direction, z = wavelength (cm), w = amplitude (cm)
            glm::vec4 Form; // x = steepness
        };
        static_assert( sizeof( WaterWaveRow ) == 2 * sizeof( glm::vec4 ), "std430 rows of two vec4" );

        // WATER_MAX_WAVES / WATER_MAX_WAVE_SETS (WaterMeshTiles.glslh): the wave sets lead the tile buffer.
        constexpr auto     kMaxWaves    = static_cast<uint32_t>( Water::kMaxDrawnWaves );
        constexpr uint32_t kMaxWaveSets = 4;
        // One body wave set, std430 (WaterMeshTiles.glslh WaterWaveSet; UE: a water body's wave buffer).
        struct WaterWaveSet
        {
            // x = count, y = 1 if it IS the frame's FFT ocean, zw = WaveOrigin
            glm::vec4                           Header{ 0.0f };
            glm::vec4                           Swell{ 0.0f }; // x = peak omega (rad/s), y = significant height (cm)
            std::array<WaterWaveRow, kMaxWaves> Rows{};
        };
        static_assert( sizeof( WaterWaveSet ) == ( 2 + 2 * kMaxWaves ) * sizeof( glm::vec4 ) );
        struct WaterWavesBlock
        {
            // Member for member WaterMeshTiles.glslh's WaterTiles head.
            glm::vec4                Header{ 0.0f };     // x = the set count, y = 1 when a set IS the FFT ocean, z = its choppiness
            glm::vec4                ShoreFrame{ 0.0f }; // xy = lower (X, Z), z = 1 / texel, w = the level its heights are over
            glm::vec4                ShoreShape{ 0.0f }; // x = open sea floor, y = beach slope, z = skirt slope
            std::array<glm::vec4, 4> FFTCascade{};       // x = 1 / patch, y = significant height, z = peak omega
            std::array<glm::vec4, 4> FFTTurn{};          // xy = cos, sin of the lattice's turn
            glm::vec4                Flow{ 0.0f };       // x = 1 when a river is in the frame, zw = the river texture's size
            glm::vec4                River{ 0.0f };      // xy = the river texture's lower (X, Z), zw = 1 / texel
            std::array<glm::vec4, 3> FoamOrigin{};       // xy = the level's lower texel, z = 1 / texel
            glm::vec4                FoamShape{ 0.0f };  // x = a level's side in texels, y = 1 when the map holds foam
            std::array<WaterWaveSet, kMaxWaveSets> Sets{};
        };
        static_assert( sizeof( WaterWavesBlock ) ==
                       ( 17 + kMaxWaveSets * ( 2 + 2 * kMaxWaves ) ) * sizeof( glm::vec4 ) );
        static_assert( sizeof( WaterWavesBlock ) == WaterRenderer::kWavesBlockBytes, "WATER-CAUSTICS reads the head" );

        std::shared_ptr<Image2D> CreateFloatImage( const char* tag, const uint32_t width, const uint32_t height,
                                                   const Core::Formats::ImageFormat format,
                                                   std::vector<unsigned char>       bytes )
        {
            Core::Formats::Image2DSpecification spec = {
                 .Tag        = tag,
                 .Width      = width,
                 .Height     = height,
                 .Format     = format,
                 .Mips       = 1,
                 .Samples    = 1,
                 .Data       = Core::Formats::EmptyPixelData{},
                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                 .Properties = Core::Formats::ImageProperties::Sample,
                 .MipLevels  = {},
            };
            spec.Data = std::move( bytes );
            return Image2D::Create( spec );
        }

        // The river texture: one river's bake grid, or for several the rectangle over all of them at the finest of
        // their texels. Size 0: no river.
        struct RiverLayout
        {
            glm::vec2  Min{ 0.0f };
            glm::vec2  Texel{ 0.0f }; // cm per texel along X and Z (a bake's texels need not be square)
            glm::uvec2 Size{ 0u };
        };

        RiverLayout RiverLayoutOf( std::span<const std::shared_ptr<const Water::RiverFlowField>> flows ) noexcept
        {
            RiverLayout layout;
            if ( flows.empty() )
                return layout;
            if ( flows.size() == 1u )
            {
                const Water::RiverFlowField& flow = *flows.front();
                layout.Min                        = flow.Min;
                layout.Texel = flow.Extent / glm::vec2( glm::max( flow.Size, glm::uvec2( 1u ) ) );
                layout.Size  = flow.Size;
                return layout;
            }
            glm::vec2 lo( std::numeric_limits<float>::max() );
            glm::vec2 hi( std::numeric_limits<float>::lowest() );
            float     texel = std::numeric_limits<float>::max();
            for ( const auto& flow : flows )
            {
                lo    = glm::min( lo, flow->Min );
                hi    = glm::max( hi, flow->Min + flow->Extent );
                texel = std::min( texel, flow->Extent.x / static_cast<float>( std::max( flow->Size.x, 1u ) ) );
            }
            const glm::vec2 span = glm::max( hi - lo, glm::vec2( 1.0f ) );
            texel = std::max( { texel, 1.0f, std::max( span.x, span.y ) / static_cast<float>( Water::kRiverFlowMaxSize ) } );
            layout.Min   = lo;
            layout.Texel = glm::vec2( texel );
            layout.Size  = glm::uvec2( glm::clamp( glm::ceil( span / texel ), glm::vec2( 1.0f ),
                                                   glm::vec2( static_cast<float>( Water::kRiverFlowMaxSize ) ) ) );
            return layout;
        }

        // (flow x, flow z cm/s, bank mask, still surface Y cm) per texel: the first river whose banks hold its
        // centre; zeros outside every bank. No river: one zero texel.
        std::shared_ptr<Image2D> CreateRiverImage( std::span<const std::shared_ptr<const Water::RiverFlowField>> flows )
        {
            const RiverLayout      layout = RiverLayoutOf( flows );
            const glm::uvec2       size   = glm::max( layout.Size, glm::uvec2( 1u ) );
            std::vector<glm::vec4> texels( size_t( size.x ) * size.y, glm::vec4( 0.0f ) );
            for ( uint32_t z = 0; z < layout.Size.y; ++z )
                for ( uint32_t x = 0; x < layout.Size.x; ++x )
                {
                    const glm::vec2 world =
                         layout.Min + ( glm::vec2( static_cast<float>( x ), static_cast<float>( z ) ) + 0.5f ) * layout.Texel;
                    for ( const auto& flow : flows )
                    {
                        const glm::vec2 g = ( world - flow->Min ) / flow->Extent * glm::vec2( flow->Size );
                        if ( g.x < 0.0f || g.y < 0.0f || g.x >= static_cast<float>( flow->Size.x ) ||
                             g.y >= static_cast<float>( flow->Size.y ) )
                            continue;
                        const size_t at = static_cast<size_t>( g.y ) * flow->Size.x + static_cast<size_t>( g.x );
                        if ( at >= flow->Texels.size() || flow->Texels[at].z <= 0.5f )
                            continue;
                        const glm::vec4& f = flow->Texels[at];
                        texels[size_t( z ) * size.x + x] =
                             glm::vec4( f.x, f.y, f.z,
                                        at < flow->SurfaceHeightCm.size() ? flow->SurfaceHeightCm[at] : flow->SurfaceY );
                        break;
                    }
                }
            std::vector<unsigned char> bytes( texels.size() * sizeof( glm::vec4 ) );
            std::memcpy( bytes.data(), texels.data(), bytes.size() );
            return CreateFloatImage( "WaterRiver", size.x, size.y, Core::Formats::ImageFormat::RGBA32F,
                                     std::move( bytes ) );
        }

        // The still surface's vertical extent the quadtree culls with when the body has no waves.
        constexpr float kFlatSurfaceVerticalExtentCm = 1.0f;
        // The shore wave's reach above and below the still level, in significant heights: a shoaled crest, and the
        // run-up (Hunt: at most one deep-water height on the slopes a beach has).
        constexpr float kShoreWaveReach = 1.5f;

    } // namespace

    Common::BoolResultStr WaterRenderer::Initialize()
    {
        // The engine's program must build: a material naming it is the common case and its failure is loud here.
        const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName(
             std::format( "WaterSurface/{}", Core::Preprocess::kSurfaceWaterMeshCell ) );
        if ( !shader )
            return Common::MakeError( std::format( "WaterRenderer: missing shader 'WaterSurface/{}'",
                                                   Core::Preprocess::kSurfaceWaterMeshCell ) );
        // The water's caustics (WATER-CAUSTICS): two compute passes before the deferred direct lighting, and every
        // sun receiver multiplies the sun under the water by their map (SceneRenderer::AddFrameWaterCaustics ->
        // AddCausticsPasses). Their failure leaves the water drawing without caustics, said here once.
        auto caustics = std::make_unique<WaterCaustics>( m_SceneRenderer, m_TargetFramebuffer.lock() );
        if ( const auto init = caustics->Initialize(); !init )
        {
            LOG_ERROR( "[Water] the water caustics did not initialise; the sun under water has none: {}",
                       init.GetError() );
        }
        else
        {
            caustics->SetEnabled( m_CausticsEnabled );
            m_Caustics = std::move( caustics );
        }
        return BOOLSUCCESS;
    }

    std::optional<ISingleLayerWaterPass::CausticsFrame>
    WaterRenderer::AddCausticsPasses( RDG::Builder& graph, const CausticsInputs& inputs )
    {
        if ( m_Caustics == nullptr )
            return std::nullopt;
        const WaterCaustics::FrameInputs frame{ .Waves         = GetFrameWavesBlock(),
                                                .Bodies        = GetFrameBodies(),
                                                .BodyWaveLanes = GetFrameBodyWaveLanes(),
                                                .Water         = inputs.Water,
                                                .SunTravel     = inputs.SunTravel,
                                                .Camera        = inputs.Camera,
                                                .TimeSeconds   = inputs.TimeSeconds };
        const std::optional<WaterCausticsFrame> made = m_Caustics->AddPasses( graph, frame );
        if ( !made.has_value() )
            return std::nullopt;
        return CausticsFrame{ .Map = made->Map, .Row = made->Row };
    }

    bool WaterRenderer::FirstWarning( const Assets::AssetHandle& material, const WaterWarning cause )
    {
        // One key per (material, cause): the handle spread by the golden ratio, the cause in its low bits. Asked
        // before the insert, so a cause said already costs a lookup and no node.
        const uint64_t key =
             ( static_cast<uint64_t>( material ) * 0x9E3779B97F4A7C15ull ) ^ static_cast<uint64_t>( cause );
        if ( m_Warned.contains( key ) )
            return false;
        m_Warned.insert( key );
        return true;
    }

    void WaterRenderer::WarnOnce( const Assets::AssetHandle& material, const WaterWarning cause,
                                  const std::string_view why )
    {
        if ( FirstWarning( material, cause ) )
            Warn( material, why );
    }

    void WaterRenderer::Warn( const Assets::AssetHandle& material, const std::string_view why )
    {
        LOG_WARN( "[Water] water material {}: {}", static_cast<uint64_t>( material ), why );
    }

    WaterRenderer::WaterProgram* WaterRenderer::ProgramFor( const Assets::AssetHandle& material,
                                                            const MaterialStamp&       stamp )
    {
        // The last answer while the material and every stamp a resolution reads stand: a steady frame walks no
        // instance chain and copies no template name.
        // A moved stamp drops every answer (a template may have been rebuilt or a material re-pointed).
        if ( !m_Resolved.empty() && !( m_Resolved.front().Stamp == stamp ) )
            m_Resolved.clear();
        for ( const ResolvedProgram& resolved : m_Resolved )
            if ( resolved.Material == static_cast<uint64_t>( material ) )
                return resolved.Program;
        const auto remember = [&]( WaterProgram* const answer )
        {
            m_Resolved.push_back( ResolvedProgram{ static_cast<uint64_t>( material ), stamp, answer } );
            return answer;
        };
        auto* materials = Runtime::ResourceRegistry::GetMaterialService();
        if ( materials == nullptr )
            return nullptr;
        const auto templ = materials->ShaderHandleOf( material );
        if ( static_cast<uint64_t>( templ.Shader ) == 0 || templ.CompileName.empty() )
        {
            WarnOnce( material, WaterWarning::NoTemplate,
                      "names no material with a loaded template - the body draws no water" );
            return nullptr;
        }
        if ( auto it = m_Programs.find( templ.CompileName ); it != m_Programs.end() )
        {
            if ( !it->second.Pipeline )
                return nullptr; // a refusal, said once when it was built
            return remember( &it->second );
        }

        WaterProgram&     program = m_Programs[templ.CompileName]; // an empty entry remembers a refusal
        const std::string cellName =
             std::format( "{}/{}", templ.CompileName, Core::Preprocess::kSurfaceWaterMeshCell );
        const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( cellName );
        if ( !shader )
        {
            if ( FirstWarning( material, WaterWarning::NoWaterCell ) )
                Warn( material, std::format( "its template '{}' has no {} cell - a water body draws a template of "
                                             "ShadingModel SingleLayerWater with Usage WaterMesh",
                                             templ.CompileName, Core::Preprocess::kSurfaceWaterMeshCell ) );
            return nullptr;
        }

        if ( auto built = BuildProgram( shader, "WaterSurfacePipeline:" + templ.CompileName, program ); !built )
        {
            if ( FirstWarning( material, WaterWarning::TemplateDoesNotBuild ) )
                Warn( material,
                      std::format( "its template '{}' does not build: {}", templ.CompileName, built.GetError() ) );
            return nullptr;
        }
        program.Material    = std::make_unique<DataDrivenMaterial>( cellName );
        program.CompileName = templ.CompileName;
        return remember( &program );
    }

    WaterRenderer::WaterProgram* WaterRenderer::FrontLayerProgramFor( const Assets::AssetHandle& material,
                                                                      WaterProgram&              program )
    {
        if ( program.FrontLayerTried )
            return program.FrontLayer ? program.FrontLayer.get() : nullptr;
        program.FrontLayerTried = true;
        // The template's own front layer cell (DShaderParser kSurfaceWaterMeshFrontLayerCell): the same surface
        // function over the water mesh path, its mirror normal into the front layer G-buffer
        // (Pass_FrontLayer.glslh).
        const std::string cellName =
             std::format( "{}/{}", program.CompileName, Core::Preprocess::kSurfaceWaterMeshFrontLayerCell );
        const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( cellName );
        if ( !shader )
        {
            if ( FirstWarning( material, WaterWarning::NoFrontLayerCell ) )
                Warn( material,
                      std::format( "its template '{}' has no {} cell - the water is not in the front "
                                   "layer and reflects its captures and the sky without the trace",
                                   program.CompileName, Core::Preprocess::kSurfaceWaterMeshFrontLayerCell ) );
            return nullptr;
        }
        // UE's FrontLayerTranslucencyGBuffer state over the cell's own (its cull): opaque, depth-tested and
        // depth-written against the front layer's depth, so each pixel keeps its nearest translucent surface.
        GraphicsPipelineSpecification spec;
        spec.DebugName = "WaterFrontLayerPipeline:" + program.CompileName;
        spec.Shader    = shader;
        ApplyShaderRenderState( spec, shader->GetProgramMeta().State );
        spec.TargetLayout      = FrontLayerTargetLayout();
        spec.DepthTestEnabled  = true;
        spec.DepthCompareOp    = DepthCompare::CloserOrEqual;
        spec.DepthWriteEnabled = true;
        spec.BlendEnable       = false;
        const auto pipeline    = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        if ( !pipeline )
        {
            if ( FirstWarning( material, WaterWarning::FrontLayerDoesNotBuild ) )
                Warn( material,
                      std::format( "its {} cell does not build: {} - the water reflects its captures "
                                   "and the sky without the trace",
                                   Core::Preprocess::kSurfaceWaterMeshFrontLayerCell, pipeline.GetError() ) );
            return nullptr;
        }
        auto front         = std::make_unique<WaterProgram>();
        front->Pipeline    = pipeline.GetValue();
        front->Material    = std::make_unique<DataDrivenMaterial>( cellName );
        front->CompileName = program.CompileName;
        program.FrontLayer = std::move( front );
        return program.FrontLayer.get();
    }

    Common::BoolResultStr WaterRenderer::BuildProgram( const std::shared_ptr<Shader>& shader,
                                                       const std::string& debugName, WaterProgram& program )
    {
        GraphicsPipelineSpecification spec;
        spec.DebugName    = debugName;
        spec.Shader       = shader;
        spec.TargetLayout = SceneTargetLayout();
        spec.BlendEnable  = false;
        ApplyShaderRenderState( spec, shader->GetProgramMeta().State );
        const auto pipeline = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        if ( !pipeline )
            return Common::MakeError( std::string( pipeline.GetError() ) );
        program.Pipeline = pipeline.GetValue();
        return BOOLSUCCESS;
    }

    WaterRenderer::WaterProgram* WaterRenderer::VirtualLocalProgramFor( const Assets::AssetHandle& material,
                                                                        WaterProgram&              program )
    {
        if ( program.VirtualLocalTried )
            return program.VirtualLocal ? program.VirtualLocal.get() : nullptr;
        program.VirtualLocalTried = true;
        // The forward cells' one permutation rule, with no screen mask: the water is not in the opaque depth the
        // masks were traced against. Listed for the cook by MeshRenderer::MaterialProgramVariants.
        const std::shared_ptr<Shader>& shader = program.Pipeline->GetSpecification().Shader;
        const Core::ShaderVariant      permutation =
             MeshRenderer::ForwardShadowMaskVariant( *shader, /*rayTracedSun*/ false, /*localShadowMasks*/ false,
                                                     /*virtualLocal*/ true );
        if ( permutation.IsDefault() )
            return &program; // reads no local shadow: the cell itself serves VirtualMaps
        auto acquired =
             Runtime::ResourceRegistry::GetShaderService()->AcquireVariant( shader->GetName(), permutation );
        if ( !acquired )
        {
            if ( FirstWarning( material, WaterWarning::NoVirtualLocalPermutation ) )
                Warn( material,
                      std::format( "its cell has no DESERT_LOCAL_SHADOW_VSM permutation ({}) - the water "
                                   "is not drawn under Shadows.Method VirtualMaps",
                                   acquired.GetError() ) );
            return nullptr;
        }
        auto twin = std::make_unique<WaterProgram>();
        if ( auto built =
                  BuildProgram( acquired.GetValue(), "WaterSurfacePipelineVsm:" + shader->GetName(), *twin );
             !built )
        {
            if ( FirstWarning( material, WaterWarning::VirtualLocalDoesNotBuild ) )
                Warn( material,
                      std::format( "its DESERT_LOCAL_SHADOW_VSM permutation does not build: {} - the water "
                                   "is not drawn under Shadows.Method VirtualMaps",
                                   built.GetError() ) );
            return nullptr;
        }
        // Over the permutation itself: its layout is its own (the virtual map's tables, no atlas).
        twin->Material       = std::make_unique<DataDrivenMaterial>( acquired.GetValue() );
        program.VirtualLocal = std::move( twin );
        return program.VirtualLocal.get();
    }

    bool WaterRenderer::FrameSamples( const std::string_view textureName ) const
    {
        // Any of the frame's templates: the shadow gates must serve every program that draws.
        if ( m_FrameDraws.empty() || !m_FrameReady )
            return false;
        return std::any_of( m_Walk.Programs.begin(), m_Walk.Programs.end(),
                            [&]( const FrameProgram& frame )
                            {
                                return frame.Program != nullptr && frame.Program->Pipeline &&
                                       frame.Program->Pipeline->GetSpecification().Shader->SamplesTexture(
                                            textureName );
                            } );
    }

    void WaterRenderer::ApplyMaterials( WaterProgram& program, const std::vector<Assets::AssetHandle>& materials )
    {
        // One row per body material of the template, in FrameProgram::Materials' order (the tiles'
        // MaterialIndex), written in one upload. A fold no material of this frame reads any more is dropped.
        m_Rows.clear();
        for ( const Assets::AssetHandle& material : materials )
            ApplyMaterial( material, m_Walk.Stamp, program, m_Rows );
        std::erase_if( program.Folded,
                       [&]( const FoldedValues& folded )
                       {
                           return std::none_of( materials.begin(), materials.end(),
                                                [&]( const Assets::AssetHandle& m )
                                                { return static_cast<uint64_t>( m ) == folded.Material; } );
                       } );
        if ( auto* rows = BlocksOf( program ).Rows )
            rows->SetRawData( m_Rows.data(), static_cast<uint32_t>( m_Rows.size() * sizeof( glm::vec4 ) ) );
    }

    void WaterRenderer::ApplyMaterial( const Assets::AssetHandle& material, const MaterialStamp& stamp,
                                       WaterProgram& program, std::vector<glm::vec4>& rows )
    {
        // The body's `.demat` folded over the template's schema into the cell's material row (UE: the material's
        // uniform expressions, cached by the render proxy until the material changes), its textures into the
        // bindless slots the row names. Each material keeps its own fold (WaterProgram::Folded), re-resolved only
        // when a stamp moved; the cell takes a fold's values only when another material was applied to it last.
        // The row is appended every frame (each frame and view has its own copy of the block).
        DataDrivenMaterial& cell = *program.Material;
        auto it = std::find_if( program.Folded.begin(), program.Folded.end(), [&]( const FoldedValues& f )
                                { return f.Material == static_cast<uint64_t>( material ); } );
        if ( it == program.Folded.end() )
            it = program.Folded.emplace( program.Folded.end() );
        FoldedValues& folded = *it;
        if ( folded.Material != static_cast<uint64_t>( material ) || folded.Stamp != stamp )
        {
            folded.Material = static_cast<uint64_t>( material );
            folded.Stamp    = stamp;
            folded.Overrides.Params.clear();
            folded.Overrides.Textures.clear();
            // The refilled Params may name other parameters in the same places: the handles start over.
            folded.Overrides.Resolved.Reset();
            if ( auto* materials = Runtime::ResourceRegistry::GetMaterialService() )
                (void)materials->ResolveOverrides( material, folded.Overrides );
            program.Applied = 0; // the cell must take the refolded values
        }
        if ( program.Applied != static_cast<uint64_t>( material ) )
        {
            // FPS-MAT-HANDLES3: the defaults, then each param by its handle (resolved once per layout stamp).
            cell.ApplyOverrides( folded.Overrides );
            program.Applied = static_cast<uint64_t>( material );
        }
        const auto& textures = folded.Overrides.Textures;
        for ( size_t index = 0; index < textures.size(); ++index )
        {
            const auto& [name, handle] = textures[index];
            if ( handle == 0 )
                continue;
            auto* texture = Runtime::ResourceRegistry::GetTextureService()->Get( Common::UUID( handle ) );
            if ( texture == nullptr )
                continue; // still loading: picked up the frame it lands
            const auto* image = dynamic_cast<Image2D*>(
                 Runtime::ResourceRegistry::GetImageService()->Resolve( texture->GetImageHandle() ) );
            // FPS-MAT-HANDLES3: through the cell's handle for this override's place (re-resolved by name only
            // when the layout, the executor or the name there moved).
            if ( image != nullptr )
                (void)cell.SetTexture( cell.OverrideTextureHandle( index ), name, image );
        }
        const Core::Formats::MaterialParamRow& row = cell.GetParamRow();
        rows.insert( rows.end(), row.begin(), row.end() );
    }

    const WaterRenderer::WaterProgram::CellBlocks& WaterRenderer::BlocksOf( WaterProgram& program )
    {
        WaterProgram::CellBlocks& blocks   = program.Blocks;
        const DataDrivenMaterial& cell     = *program.Material;
        const MaterialLayoutStamp stamp    = cell.GetLayoutStamp();
        const MaterialExecutor*   executor = cell.GetMaterialExecutor();
        if ( blocks.Resolved && blocks.Stamp == stamp && blocks.Executor == executor )
            return blocks;
        blocks.Stamp    = stamp;
        blocks.Executor = executor;
        blocks.Rows     = cell.Get<StorageBufferProperty>( WaterShaderParameters::Materials );
        blocks.Tiles    = cell.Get<StorageBufferProperty>( WaterShaderParameters::WaterTiles );
        blocks.Resolved = true;
        return blocks;
    }

    void WaterRenderer::PrepareFrame()
    {
        m_FrameDraws.clear();
        const auto* camera = m_SceneRenderer->GetMainCamera();
        m_FrameReady       = false;
        m_Walk             = TileWalk{};
        m_FrameFrontLayers = 0u;
        m_FFTFrame.reset();
        m_FrameWaterRefs  = WaterFrameRefs{};
        m_FrameWaterGraph = nullptr;
        m_FoamFrame.reset();
        m_FrameBodies.clear();
        m_FrameBodyWaveLanes.clear();
        m_FrameBodyLooks.clear();
        m_Underwater = UnderwaterFrame{};
        if ( m_Queue.empty() )
            m_ZoneTrees.clear(); // no water this frame: the retained trees (and their body sets) go
        if ( camera == nullptr || m_Queue.empty() )
            return;

        // THE FRAME'S BODIES (UE: the water mesh tile's WaterBodyIndex picks its body's material and waves). Each
        // distinct (wave set, WaveOrigin) the bodies name is one of the frame's wave sets (WaterWavesBlock::Sets,
        // at most kMaxWaveSets; a body past them draws flat, said once); a tile reads its body's set (Body.z).
        // Each template the bodies use is one FrameProgram (its own cell, binding block and Materials block);
        // every body material gets a row in its template's Materials block (FrameProgram::Materials; its tiles
        // draw with MaterialIndex = that row, through that program). Each river body's
        // flow period rides its own lane (WaterBodyLane::Medium -> WaterTileRow::Medium): one value per body, never
        // the first body's.
        Assets::AssetHandle                           material = Assets::AssetHandle::Null();
        std::shared_ptr<const Water::WaterShoreField> shore; // the world's one field: every zone carries it
        for ( const WaterZoneDraw& zone : m_Queue )
            if ( shore == nullptr )
                shore = zone.Shore;
        for ( const WaterZoneDraw& zone : m_Queue )
            for ( size_t b = 0; b < zone.Bodies->Materials.size() && static_cast<uint64_t>( material ) == 0; ++b )
                material = zone.Bodies->Materials[b];
        if ( static_cast<uint64_t>( material ) == 0 )
            return; // no body names a look: no water (WaterBodyData::WaterMaterial)
        MaterialStamp stamp;
        if ( auto* const materials = Runtime::ResourceRegistry::GetMaterialService() )
        {
            stamp.Service             = materials;
            stamp.InvalidationVersion = materials->GetInvalidationVersion();
            stamp.ValuesEditSerial    = materials->GetValuesEditSerial();
        }
        stamp.ContentSerial = Assets::ContentRegistry::WriteSerial();
        // The body the camera is under (its tiles may all be out of view: the volume is drawn regardless).
        PrepareUnderwater( glm::vec3( glm::inverse( camera->GetViewMatrix() )[3] ), stamp );
        // Under Shadows.Method VirtualMaps no atlas is drawn: each cell's virtual-map permutation draws, as every
        // forward local-shadow reader's does (SceneRenderer::LocalShadowsAreVirtual, settled by BeginScene).
        const bool                          virtualLocal = m_SceneRenderer->LocalShadowsAreVirtual();
        std::vector<FrameProgram>           programs;
        // The frame's wave sets: the asset, its WaveOrigin and the first body material naming it (its warnings).
        struct FrameWaveSet
        {
            Assets::AssetHandle Asset;
            glm::vec2           Origin{ 0.0f };
            Assets::AssetHandle Material;
        };
        std::vector<FrameWaveSet>           waveSets;
        std::vector<std::vector<WaterBodyLane>> bodyLanes( m_Queue.size() );
        std::vector<std::vector<uint32_t>>      bodyPrograms( m_Queue.size() );
        for ( size_t z = 0; z < m_Queue.size(); ++z )
        {
            const WaterZoneBodies& bodies = *m_Queue[z].get().Bodies;
            bodyLanes[z].assign( bodies.Materials.size(), WaterBodyLane{} );
            bodyPrograms[z].assign( bodies.Materials.size(), 0u );
            for ( size_t b = 0; b < bodies.Materials.size(); ++b )
            {
                const Assets::AssetHandle& body = bodies.Materials[b];
                if ( static_cast<uint64_t>( body ) == 0 )
                    continue; // no look: its tiles are not drawn
                // The body's template (ProgramFor says a refusal once; its tiles are then not drawn).
                WaterProgram* const base = ProgramFor( body, stamp );
                if ( base == nullptr )
                    continue;
                auto frame = std::find_if( programs.begin(), programs.end(),
                                           [base]( const FrameProgram& p ) { return p.Base == base; } );
                if ( frame == programs.end() )
                {
                    // The template's front layer cell, of the base program (it reads no local shadow: no twin).
                    WaterProgram* const front = FrontLayerProgramFor( body, *base );
                    WaterProgram* const drawn = virtualLocal ? VirtualLocalProgramFor( body, *base ) : base;
                    if ( drawn == nullptr )
                        continue; // no VSM twin: not drawn rather than drawn unshadowed (said once)
                    frame = programs.insert(
                         programs.end(),
                         FrameProgram{ .Base = base, .Program = drawn, .FrontLayer = front, .Materials = {} } );
                }
                auto slot = std::find( frame->Materials.begin(), frame->Materials.end(), body );
                if ( slot == frame->Materials.end() )
                    slot = frame->Materials.insert( frame->Materials.end(), body );
                bodyPrograms[z][b] = static_cast<uint32_t>( frame - programs.begin() );
                const bool river = b < bodies.Types.size() && bodies.Types[b] == Water::WaterBodyType::River;
                // The body's wave set: 1 + its index in waveSets, 0 = flat (none named, or past the last set).
                const Assets::AssetHandle& bodyWaves = bodies.Waves[b];
                float                      waveLane  = 0.0f;
                if ( static_cast<uint64_t>( bodyWaves ) != 0 )
                {
                    const glm::vec2 origin = b < bodies.WaveOrigins.size() ? bodies.WaveOrigins[b] : glm::vec2( 0.0f );
                    auto set = std::find_if( waveSets.begin(), waveSets.end(), [&]( const FrameWaveSet& w )
                                             { return w.Asset == bodyWaves && w.Origin == origin; } );
                    if ( set == waveSets.end() && waveSets.size() < kMaxWaveSets )
                        set = waveSets.insert( waveSets.end(),
                                               FrameWaveSet{ .Asset = bodyWaves, .Origin = origin, .Material = body } );
                    if ( set != waveSets.end() )
                        waveLane = 1.0f + static_cast<float>( set - waveSets.begin() );
                    else
                        WarnOnce( body, WaterWarning::TooManyWaveSets,
                                  std::format( "its body's wave set is past the frame's {} (WATER_MAX_WAVE_SETS) - "
                                               "drawn flat",
                                               kMaxWaveSets ) );
                }
                // The body's own medium (UE: the river's flow): on a river its bake's two-phase period (WaterBodyData::RiverFlowPeriod ->
                // Water::RiverFlowField::FlowPeriod).
                const std::shared_ptr<const Water::RiverFlowField>& flow =
                     b < bodies.Flows.size() ? bodies.Flows[b] : std::shared_ptr<const Water::RiverFlowField>{};
                const float flowPeriod = river && flow != nullptr ? std::max( flow->FlowPeriod, 0.05f ) : 0.0f;
                bodyLanes[z][b] =
                     WaterBodyLane{ .Body   = glm::vec4( static_cast<float>( slot - frame->Materials.begin() ),
                                                       river ? 1.0f : 0.0f, waveLane, static_cast<float>( b ) ),
                                    .Medium = glm::vec4( 0.0f, flowPeriod, 0.0f, 0.0f ) };
                // Every body that draws this frame (WATER-CAUSTICS: its footprint, still level and wave set).
                m_FrameBodies.push_back( bodies.Footprints[b] );
                m_FrameBodyWaveLanes.push_back( waveLane );
                m_FrameBodyLooks.push_back( FrameBodyLook{ .Material = body, .River = river } );
            }
        }
        if ( programs.empty() )
            return; // no body's template builds: no water (each refusal said once)

        // THE BODIES' WAVES (UE: each water body's wave buffer the water vertex factory reads): each set generated
        // once per asset (Runtime::WaterWavesService), packed ahead of the tiles. Still loading = flat this frame.
        // THE FFT OCEAN replaces a set's Gerstner rows where the set is one and the tier asks for it: the world's one
        // ocean (ECS::WaterOceanRule), under every WaveOrigin its bodies have.
        WaterWavesBlock waves;
        const glm::vec3 observer = glm::vec3( glm::inverse( camera->GetViewMatrix() )[3] );
        // The floor and its shore (Water/WaterDepthField.glslh), before the waves: the swell's shore form reads its slope.
        waves.ShoreFrame = shore != nullptr ? glm::vec4( shore->MinCm, 1.0f / shore->TexelCm, shore->ReferenceLevelCm )
                                            : glm::vec4( 0.0f, 0.0f, 0.01f, m_FrameBodies.front().SurfaceY );
        waves.ShoreShape = shore != nullptr
                                ? glm::vec4( shore->OpenSeaFloorCm, shore->BeachSlope, Water::kShoreSkirtSlope, 0.0f )
                                : glm::vec4( m_FrameBodies.front().SurfaceY - Water::kOpenSeaDepthCm, 0.0f,
                                             Water::kShoreSkirtSlope, 0.0f );
        waves.Header.x       = static_cast<float>( waveSets.size() );
        float verticalExtent = kFlatSurfaceVerticalExtentCm;
        bool  fft            = false;
        Assets::AssetHandle ocean = Assets::AssetHandle::Null(); // the FFT set, once fft
        auto* const service  = Runtime::GetWaterWavesService();
        for ( size_t index = 0; index < waveSets.size(); ++index )
        {
            const FrameWaveSet& frameSet = waveSets[index];
            WaterWaveSet&       set      = waves.Sets[index];
            set.Header.z                 = frameSet.Origin.x;
            set.Header.w                 = frameSet.Origin.y;
            if ( service == nullptr )
            {
                WarnOnce( frameSet.Material, WaterWarning::NoWavesService,
                          "its body's waves cannot be read: no WaterWavesService" );
                continue;
            }
            Common::ResultStr<Runtime::WaterWavesService::Waves> got = service->Get( frameSet.Asset );
            if ( !got )
            {
                if ( FirstWarning( frameSet.Material, WaterWarning::WavesRefused ) )
                    Warn( frameSet.Material, std::format( "its body's waves are refused: {}", got.GetError() ) );
            }
            else if ( got.GetValue() )
            {
                const std::vector<Water::GerstnerWave>& rows = *got.GetValue();
                if ( rows.size() > kMaxWaves && FirstWarning( frameSet.Material, WaterWarning::TooManyWaves ) )
                    Warn( frameSet.Material, std::format( "its body's wave set has {} waves - the water mesh draws "
                                                          "the first {} (WATER_MAX_WAVES)",
                                                          rows.size(), kMaxWaves ) );
                const size_t count = std::min<size_t>( rows.size(), kMaxWaves );
                for ( size_t k = 0; k < count; ++k )
                {
                    const Water::GerstnerWave& wave = rows[k];
                    set.Rows[k].Wave                = glm::vec4( wave.Direction, wave.WaveLength, wave.Amplitude );
                    set.Rows[k].Form                = glm::vec4( wave.Steepness, 0.0f, 0.0f, 0.0f );
                }
                set.Header.x   = static_cast<float>( count );
                const Water::ShoreWaveParams swell =
                     Water::GerstnerSwell( std::span( rows ).first( count ), waves.ShoreShape.y );
                set.Swell      = glm::vec4( swell.Omega, swell.SignificantHeight, 0.0f, 0.0f );
                verticalExtent = std::max( { verticalExtent, Water::MaxWaveHeight( rows ),
                                             kShoreWaveReach * swell.SignificantHeight } );
            }
            if ( fft ? frameSet.Asset == ocean : PrepareFFTFrame( frameSet.Material, frameSet.Asset ) )
            {
                fft                                   = true;
                ocean                                 = frameSet.Asset;
                service->SetDrawnFFT( frameSet.Asset );
                const Water::FFTOceanSpectrum& spectrum = *m_FFTFrame;
                waves.Header.y                        = 1.0f;
                waves.Header.z                        = spectrum.Params.Choppiness;
                set.Header.y                          = 1.0f;
                for ( uint32_t c = 0; c < Water::kFFTOceanCascades; ++c )
                {
                    const float angle   = glm::radians( Water::kFFTOceanLatticeDegrees[c] );
                    waves.FFTCascade[c] = glm::vec4( 1.0f / spectrum.Params.CascadeSizes[c],
                                                     spectrum.Cascades[c].SignificantHeight,
                                                     spectrum.Cascades[c].PeakOmega, 0.0f );
                    waves.FFTTurn[c]    = glm::vec4( std::cos( angle ), std::sin( angle ), 0.0f, 0.0f );
                }
                const Water::ShoreWaveParams swell = Water::FFTOceanSwell( spectrum, waves.ShoreShape.y );
                set.Swell = glm::vec4( swell.Omega, swell.SignificantHeight, 0.0f, 0.0f );
                // The choppy map moves the surface sideways by up to lambda times its reach, and the quadtree culls
                // by a vertical extent only: the larger of the two bounds both.
                verticalExtent = std::max( { verticalExtent, spectrum.MaxWaveHeight,
                                             spectrum.MaxWaveHeight * spectrum.Params.Choppiness,
                                             kShoreWaveReach * swell.SignificantHeight } );
            }
        }
        // The frame's rivers: every distinct baked ribbon of the zones (a body shared by two zones counts once).
        // Kept as shared_ptrs: the WaterInfo texture composed from them is rebuilt only when this list changes
        // (FinishPrepare, m_FlowsBaked).
        std::vector<std::shared_ptr<const Water::RiverFlowField>> rivers;
        for ( const WaterZoneDraw& zone : m_Queue )
            for ( const std::shared_ptr<const Water::RiverFlowField>& flow : zone.Bodies->Flows )
                if ( flow != nullptr && flow->Size.x != 0u && flow->Size.y != 0u &&
                     std::find( rivers.begin(), rivers.end(), flow ) == rivers.end() )
                    rivers.push_back( flow );
        const auto riverCount = static_cast<int32_t>( rivers.size() );
        if ( const int32_t source = ( fft ? 1 : 0 ) + 2 * riverCount; source != m_LoggedWaveSource )
        {
            m_LoggedWaveSource = source;
            if ( fft )
            {
                LOG_INFO( "[Water] waves=FFT cascades={} rivers={} readback=on", Water::kFFTOceanCascades,
                          riverCount );
            }
            else
            {
                LOG_INFO( "[Water] waves=Gerstner rivers={}", riverCount );
            }
        }

        // The waves block is whole here: the upload's head, the tile task appends the rows.
        const RiverLayout river = RiverLayoutOf( rivers );
        // THE FLOW LANES (WaterMeshTiles.glslh WaterFlowFrame / WaterRiverFrame): on while a river is in the
        // frame; each river's two-phase period is its own tiles' (WaterTileRow::Medium.y).
        if ( !rivers.empty() )
        {
            waves.Flow  = glm::vec4( 1.0f, 0.0f, static_cast<float>( river.Size.x ),
                                     static_cast<float>( river.Size.y ) );
            waves.River = glm::vec4( river.Min, 1.0f / glm::max( river.Texel, glm::vec2( 1e-3f ) ) );
        }
        int32_t fftSet = -1;
        for ( size_t index = 0; index < waveSets.size() && fft && fftSet < 0; ++index )
            if ( waves.Sets[index].Header.y > 0.5f )
                fftSet = static_cast<int32_t>( index );
        PrepareFoam( stamp, observer, fftSet, shore, waves.FoamOrigin, waves.FoamShape );
        m_Upload.resize( sizeof( WaterWavesBlock ) );
        std::memcpy( m_Upload.data(), &waves, sizeof( WaterWavesBlock ) );

        const glm::mat4 view     = camera->GetViewMatrix();
        const glm::mat4 proj     = camera->GetProjectionMatrix();
        // The lens stretches every water LOD ring (VIEW-LOD-FOV): the one view scale meshes, foliage and terrain
        // read, so a 10-degree scope keeps the far sea as fine as it looks.
        const float lensScale = DrawDistanceScale( Geometry::LODView{ .Origin = observer, .Projection = proj } );
        m_Walk                = TileWalk{ .Programs       = std::move( programs ),
                                          .Stamp          = stamp,
                                          .Shore          = std::move( shore ),
                                          .Flows          = std::move( rivers ),
                                          .Bodies         = std::move( bodyLanes ),
                                          .BodyPrograms   = std::move( bodyPrograms ),
                                          .View           = view,
                                          .Projection     = proj,
                                          .Observer       = observer,
                                          .LensScale      = lensScale,
                                          .VerticalExtent = verticalExtent,
                                          .Rows           = 0 };
    }

    namespace
    {
        // The slot names of Programs/Water/WaterUnderwater.shader, spelled once.
        struct WaterUnderwaterShaderParameters
        {
            DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
            DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
            DESERT_SHADER_PARAMETER( Texture, SceneColorTexture );
            DESERT_SHADER_PARAMETER( Texture, SceneDepthTexture );
            DESERT_SHADER_PARAMETER( Texture, EnvironmentIrradianceTexture );
            DESERT_SHADER_PARAMETER( UniformBuffer, UnderwaterUB );
        };
        // WaterUnderwater.shader's UnderwaterUB (std140: every member 16-byte aligned).
        struct UnderwaterUBData
        {
            glm::mat4 InvViewProjection;
            glm::vec4 CameraSurface;
            glm::vec4 Scattering;
            glm::vec4 Absorption;
            glm::vec4 ToSun;
            glm::vec4 SunRadiance;
            glm::vec4 SkyYawCosSin;
            glm::vec4 SkyGain;
        };
        static_assert( sizeof( UnderwaterUBData ) == 176u, "UnderwaterUB: std140 size" );

        // RecordDraws' per-draw push block, a thread's scratch (CPU-FRAME-ARENA 3, as TerrainRenderer's
        // t_TerrainRecordPush): "Water: Surface" and "Water: Front Layer" are recorded on two graph chunks at
        // once (RDG ChunkTasks), so one renderer-owned block was rewritten by the other chunk between a draw's
        // WriteInto and its DrawProcedural - the draw took the other program's placement and lost its tiles.
        thread_local std::vector<std::byte> t_WaterRecordPush;
    } // namespace

    bool WaterRenderer::ReadUnderwaterMedium( const Assets::AssetHandle& material, const MaterialStamp& stamp )
    {
        if ( m_UnderwaterMedium.Material == static_cast<uint64_t>( material ) &&
             m_UnderwaterMedium.Stamp == stamp )
            return m_UnderwaterMedium.Readable;
        m_UnderwaterMedium          = UnderwaterMedium{};
        m_UnderwaterMedium.Material = static_cast<uint64_t>( material );
        m_UnderwaterMedium.Stamp    = stamp;
        auto* const materials       = Runtime::ResourceRegistry::GetMaterialService();
        if ( materials == nullptr )
            return false;
        // The template's defaults under the file's values: a `.demat` that states no PhaseG still has the
        // template's (ResolveOverrides alone gives only what the files state).
        constexpr std::array<std::string_view, 3> kNames{ "ScatteringCoefficients", "AbsorptionCoefficients",
                                                          "PhaseG" };
        std::array<glm::vec4, 3>                  values{};
        if ( !ReadMaterialValues( material, stamp, kNames, values ) )
            return false;
        m_UnderwaterMedium.Scattering = glm::max( glm::vec3( values[0] ), glm::vec3( 0.0f ) );
        m_UnderwaterMedium.Absorption = glm::max( glm::vec3( values[1] ), glm::vec3( 0.0f ) );
        m_UnderwaterMedium.PhaseG     = glm::clamp( values[2].x, -0.9f, 0.9f );
        m_UnderwaterMedium.Readable   = true;
        return true;
    }

    bool WaterRenderer::ReadMaterialValues( const Assets::AssetHandle& material, const MaterialStamp& stamp,
                                            std::span<const std::string_view> names, std::span<glm::vec4> out )
    {
        if ( names.size() != out.size() )
            return false;
        // The defaults: the schema of the template this material is drawn by (the program ProgramFor built).
        WaterProgram* const program = ProgramFor( material, stamp );
        if ( program == nullptr || program->Material == nullptr )
            return false;
        const auto& params = program->Material->GetSchema().Params;
        for ( size_t i = 0; i < names.size(); ++i )
        {
            const auto param =
                 std::find_if( params.begin(), params.end(), [&]( const Core::Formats::ShaderParam& p )
                               { return !p.IsTexture && p.Name == names[i]; } );
            if ( param == params.end() )
                return false; // not a value of this template
            out[i] = param->Default;
        }
        // The `.demat` chain over them, base first, the childmost statement last (it wins).
        auto* const materials = Runtime::ResourceRegistry::GetMaterialService();
        if ( materials == nullptr )
            return false;
        MaterialOverrides overrides;
        (void)materials->ResolveOverrides( material, overrides );
        for ( const auto& [name, value] : overrides.Params )
            for ( size_t i = 0; i < names.size(); ++i )
                if ( name == names[i] )
                    out[i] = value;
        return true;
    }

    void WaterRenderer::PrepareUnderwater( const glm::vec3& camera, const MaterialStamp& stamp )
    {
        const ViewFrame* const viewFrame = m_SceneRenderer->GetViewFrame();
        auto* const            service   = Runtime::GetWaterWavesService();
        if ( viewFrame == nullptr )
            return;
        // The near plane's lowest corner (reversed-Z: device depth 1): a camera just above the water whose near
        // plane dips under it is half under (the waterline crosses the screen).
        const glm::mat4 inv     = viewFrame->InvJitteredViewProjection;
        float           nearLow = camera.y;
        for ( const glm::vec2 ndc : { glm::vec2( -1.0f, -1.0f ), glm::vec2( 1.0f, -1.0f ),
                                      glm::vec2( -1.0f, 1.0f ), glm::vec2( 1.0f, 1.0f ) } )
        {
            const glm::vec4 w = inv * glm::vec4( ndc, 1.0f, 1.0f );
            nearLow           = std::min( nearLow, w.y / w.w );
        }
        const float         time     = static_cast<float>( viewFrame->TimeSeconds );
        float               best     = 0.0f; // how far the near plane reaches under the chosen body's surface, cm
        Assets::AssetHandle chosen   = Assets::AssetHandle::Null();
        float               surfaceY = 0.0f;
        for ( const WaterZoneDraw& zone : m_Queue )
        {
            const WaterZoneBodies& bodies = *zone.Bodies;
            for ( size_t b = 0; b < bodies.Materials.size(); ++b )
            {
                const Water::WaterQuadTreeBody& footprint = bodies.Footprints[b];
                if ( static_cast<uint64_t>( bodies.Materials[b] ) == 0 || camera.x < footprint.FootprintMin.x ||
                     camera.x > footprint.FootprintMax.x || camera.z < footprint.FootprintMin.y ||
                     camera.z > footprint.FootprintMax.y )
                    continue;
                // The body as the physics step sees it (ECS::WaterBodyGather), around the camera.
                Water::WaterBodyState state;
                state.Location = glm::vec3( bodies.WaveOrigins[b].x, footprint.SurfaceY, bodies.WaveOrigins[b].y );
                state.Extents  = 2.0f * glm::max( glm::abs( footprint.FootprintMax - bodies.WaveOrigins[b] ),
                                                  glm::abs( bodies.WaveOrigins[b] - footprint.FootprintMin ) ) +
                                glm::vec2( 1.0f );
                state.Type                = bodies.Types[b];
                state.Flow                = bodies.Flows[b];
                if ( state.Type == Water::WaterBodyType::River && state.Flow != nullptr )
                    state.Location =
                         glm::vec3( state.Flow->Min.x + 0.5f * state.Flow->Extent.x, state.Flow->SurfaceY,
                                    state.Flow->Min.y + 0.5f * state.Flow->Extent.y );
                if ( service != nullptr && static_cast<uint64_t>( bodies.Waves[b] ) != 0 )
                {
                    auto waves = service->Get( bodies.Waves[b] );
                    if ( waves && waves.GetValue() )
                        state.Waves = waves.GetValue();
                    Water::WaterSurfaceSource surface = service->SurfaceOf( bodies.Waves[b] );
                    state.FFT                         = surface.FFT;
                    state.FFTSurface                  = std::move( surface.Read );
                }
                if ( !Water::WaterBodyCovers( state, camera ) )
                    continue;
                const Water::WaterQueryResult hit   = Water::QueryWaterBody( state, camera, time, zone.Shore.get() );
                const float                   reach = hit.SurfaceLocation.y - nearLow;
                if ( reach > best )
                {
                    best     = reach;
                    chosen   = bodies.Materials[b];
                    surfaceY = hit.SurfaceLocation.y;
                }
            }
        }
        if ( best <= 0.0f )
            return; // the near plane is in the air: no pixel's ray starts under the water
        if ( !ReadUnderwaterMedium( chosen, stamp ) )
        {
            WarnOnce( chosen, WaterWarning::UnderwaterMediumMissing,
                      "its .demat states no ScatteringCoefficients / AbsorptionCoefficients / PhaseG: the "
                      "underwater volume is not drawn under it" );
            return;
        }
        if ( !m_UnderwaterPipelineTried )
        {
            m_UnderwaterPipelineTried = true;
            if ( const auto shader =
                      Runtime::ResourceRegistry::GetShaderService()->GetByName( "WaterUnderwater" ) )
            {
                GraphicsPipelineSpecification spec;
                spec.DebugName         = "WaterUnderwater";
                spec.Shader            = shader;
                spec.TargetLayout      = SceneTargetLayout();
                spec.DepthTestEnabled  = false;
                spec.DepthWriteEnabled = false;
                spec.CullMode          = CullMode::None;
                spec.Topology          = PrimitiveTopology::Triangles;
                spec.BlendEnable       = false;
                if ( const auto pipeline = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec ) )
                    m_UnderwaterPipeline = pipeline.GetValue();
            }
        }
        if ( m_UnderwaterPipeline == nullptr )
        {
            WarnOnce( chosen, WaterWarning::UnderwaterPipelineMissing,
                      "the WaterUnderwater program did not build: the underwater volume is not drawn" );
            return;
        }
        m_Underwater.Active            = true;
        m_Underwater.SurfaceY          = surfaceY;
        m_Underwater.Camera            = camera;
        m_Underwater.Scattering        = m_UnderwaterMedium.Scattering;
        m_Underwater.Absorption        = m_UnderwaterMedium.Absorption;
        m_Underwater.PhaseG            = m_UnderwaterMedium.PhaseG;
        m_Underwater.PreExposure       = viewFrame->PreExposure;
        m_Underwater.InvViewProjection = inv;
    }

    RDG::BufferRef WaterRenderer::UploadUnderwater( RDG::Builder& graph, const SceneFrameBinding& frame )
    {
        if ( !m_Underwater.Active || m_UnderwaterPipeline == nullptr )
            return {};
        const auto sun            = m_SceneRenderer->GetFrameSun();
        m_Underwater.ToSun        = glm::vec4( -glm::vec3( sun.Direction ), 0.0f );
        m_Underwater.SunRadiance  = glm::vec4( glm::vec3( sun.ColorIntensity ) * sun.ColorIntensity.a, 0.0f );
        const SkyLookGPU look     = ToGPU( frame.EnvironmentLook );
        m_Underwater.SkyYawCosSin = look.YawCosSin;
        m_Underwater.SkyGain      = look.Gain;
        const UnderwaterUBData data{ .InvViewProjection = m_Underwater.InvViewProjection,
                                     .CameraSurface     = glm::vec4( m_Underwater.Camera, m_Underwater.SurfaceY ),
                                     .Scattering  = glm::vec4( m_Underwater.Scattering, m_Underwater.PhaseG ),
                                     .Absorption  = glm::vec4( m_Underwater.Absorption, m_Underwater.PreExposure ),
                                     .ToSun       = m_Underwater.ToSun,
                                     .SunRadiance = m_Underwater.SunRadiance,
                                     .SkyYawCosSin = m_Underwater.SkyYawCosSin,
                                     .SkyGain      = m_Underwater.SkyGain };
        const RDG::BufferRef   buffer =
             graph.CreateBuffer( RDG::BufferDesc{ sizeof( data ) }, "Water.UnderwaterUB" );
        graph.QueueBufferUpload( buffer, std::as_bytes( std::span<const UnderwaterUBData>( &data, 1 ) ) );
        return buffer;
    }

    void WaterRenderer::DeclareUnderwater( RDG::PassBuilder& pass, const RDG::TextureRef sceneCopy,
                                           const SceneViewInputs& view, const RDG::BufferRef uniforms )
    {
        if ( !m_Underwater.Active || m_UnderwaterPipeline == nullptr || !sceneCopy.IsValid() ||
             !uniforms.IsValid() )
            return;
        using P = WaterUnderwaterShaderParameters;
        pass.Bindings( m_Layout.GetSlotted( m_UnderwaterPipeline->GetSpecification().Shader ).Layout,
                       RDG::NoOtherRouteFill() )
             .Texture( P::SceneColorTexture, sceneCopy, RDG::Access::SampledGraphics,
                       RDG::SubresourceRange::All() )
             .Texture( P::SceneDepthTexture, view.SceneDepth, RDG::Access::SampledGraphics,
                       RDG::SubresourceRange::All() )
             .Texture( P::EnvironmentIrradianceTexture, view.EnvIrradiance, RDG::Access::SampledGraphics,
                       RDG::SubresourceRange::All() )
             .Sampler( P::PointClampSampler, RDG::SamplerDesc::PointClamp() )
             .Sampler( P::LinearClampSampler, RDG::SamplerDesc::LinearClamp() )
             .Uniform( P::UnderwaterUB, uniforms );
    }

    Common::BoolResultStr WaterRenderer::RenderUnderwaterManual( const RDG::PassContext& context )
    {
        if ( !m_Underwater.Active || m_UnderwaterPipeline == nullptr )
            return BOOLSUCCESS;
        const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        return Renderer::GetInstance().DrawFullscreen( bindings, *m_UnderwaterPipeline, nullptr );
    }

    void WaterRenderer::PrepareTiles() noexcept
    {
        if ( m_Walk.Programs.empty() )
            return;
        const Core::Frustum frustum( m_Walk.Projection, m_Walk.View );
        const glm::vec3     observer       = m_Walk.Observer;
        const float         lensScale      = m_Walk.LensScale;
        const float         verticalExtent = m_Walk.VerticalExtent;

        // Every zone's tiles in its density order: one instanced draw per run of one grid density (a row carries
        // its own zone, so a run may span two zones). The rows go straight into the upload, after its waves block
        // (PrepareFrame wrote it).
        uint32_t rowCount = 0;
        m_ZoneTrees.resize( m_Queue.size() );
        for ( size_t z = 0; z < m_Queue.size(); ++z )
        {
            const WaterZoneDraw& zone   = m_Queue[z].get();
            ZoneTree&            cached = m_ZoneTrees[z];
            if ( !cached.Tree.has_value() || cached.Bodies != zone.Bodies || cached.Zone != zone.Zone )
            {
                cached.Tree.emplace( zone.Zone, zone.Bodies->Footprints );
                cached.Zone   = zone.Zone;
                cached.Bodies = zone.Bodies;
            }
            const Water::WaterQuadTree&                     tree = *cached.Tree;
            const std::span<const Water::WaterTileInstance> tiles =
                 tree.Traverse( observer, lensScale, &frustum, verticalExtent, m_Traversal );
            const float     viewLODScale = tree.GetViewLODScale( lensScale );
            const glm::vec2 origin = tree.GetRootMin();
            const std::vector<WaterBodyLane>& bodies       = m_Walk.Bodies[z];
            const std::vector<uint32_t>&  bodyPrograms = m_Walk.BodyPrograms[z];
            for ( const Water::WaterTileInstance& tile : tiles )
            {
                // The tile's body (PrepareFrame's lane): its material row; a body with no drawn look is skipped.
                if ( tile.BodyIndex >= bodies.size() || bodies[tile.BodyIndex].Body.x < 0.0f )
                    continue;
                const WaterBodyLane& body    = bodies[tile.BodyIndex];
                const auto           slot    = static_cast<uint32_t>( body.Body.x );
                const uint32_t  program = bodyPrograms[tile.BodyIndex];
                const uint32_t  quads   = tree.QuadsPerSide( tile.DensityIndex );
                // One instanced draw per run of one grid density, one template AND one material row (the draw's
                // program and MaterialIndex).
                if ( m_FrameDraws.empty() || m_FrameDraws.back().QuadsPerSide != quads ||
                     m_FrameDraws.back().MaterialSlot != slot || m_FrameDraws.back().ProgramSlot != program )
                {
                    m_FrameDraws.push_back( { .FirstRow     = rowCount,
                                              .RowCount     = 0,
                                              .QuadsPerSide = quads,
                                              .MaterialSlot = slot,
                                              .ProgramSlot  = program } );
                }
                ++m_FrameDraws.back().RowCount;

                WaterTileRow row;
                row.Tile        = glm::vec4( tile.Corner.x, tile.Corner.y, tile.SurfaceY, tile.Size );
                row.Lod         = glm::vec4( static_cast<float>( tile.LODLevel ), tile.ShouldMorph ? 1.0f : 0.0f,
                                     tile.CanMorphTwice ? 1.0f : 0.0f, tile.HeightMorph );
                row.Zone = glm::vec4( origin.x, origin.y, viewLODScale, static_cast<float>( quads ) );
                row.Body   = body.Body;
                row.Medium = body.Medium;
                // An ocean drawn under a river's cut leaf: -1, clipped to the bank's far side (WaterBankClipped).
                if ( tile.UnderRiver )
                    row.Body.y = -1.0f;
                const size_t at = m_Upload.size();
                m_Upload.resize( at + sizeof( WaterTileRow ) );
                std::memcpy( m_Upload.data() + at, &row, sizeof( WaterTileRow ) );
                ++rowCount;
            }

            // A RIVER'S FALLS (Water::RiverFall, its bake's steep reaches): one row per stretch of each fall's sheet
            // (Body.y = 2; Vertex_WaterMesh.glslh WaterFallVertex), drawn by the river's own template and material,
            // its water's speed from the slope (Water::RiverFallSpeed under the scene's gravity, as the CPU query).
            constexpr uint32_t kFallQuadsPerSide = 4u;
            for ( size_t b = 0; b < bodies.size() && b < zone.Bodies->Flows.size(); ++b )
            {
                const std::shared_ptr<const Water::RiverFlowField>& flow = zone.Bodies->Flows[b];
                if ( flow == nullptr || flow->Falls.empty() || bodies[b].Body.x < 0.0f )
                    continue;
                const WaterBodyLane& body    = bodies[b];
                const auto           slot    = static_cast<uint32_t>( body.Body.x );
                const uint32_t       program = bodyPrograms[b];
                for ( const Water::RiverFall& fall : flow->Falls )
                    for ( size_t k = 0; k + 1 < fall.Samples.size(); ++k )
                    {
                        const Water::RiverFallSample& a = fall.Samples[k];
                        const Water::RiverFallSample& c = fall.Samples[k + 1];
                        // The view cull (the tiles' frustum, as the quadtree's walk): the stretch's bounding sphere -
                        // half its span, half the sheet's width and the surface's vertical reach around its middle.
                        const glm::vec3 middle = 0.5f * ( a.Position + c.Position );
                        const float     reach  = 0.5f * glm::length( c.Position - a.Position ) + 0.5f * fall.Width +
                                            verticalExtent;
                        if ( !frustum.IntersectsSphere( middle, reach ) )
                            continue;
                        if ( m_FrameDraws.empty() || m_FrameDraws.back().QuadsPerSide != kFallQuadsPerSide ||
                             m_FrameDraws.back().MaterialSlot != slot || m_FrameDraws.back().ProgramSlot != program )
                        {
                            m_FrameDraws.push_back( { .FirstRow     = rowCount,
                                                      .RowCount     = 0,
                                                      .QuadsPerSide = kFallQuadsPerSide,
                                                      .MaterialSlot = slot,
                                                      .ProgramSlot  = program } );
                        }
                        ++m_FrameDraws.back().RowCount;
                        WaterTileRow row;
                        row.Tile   = glm::vec4( a.Position, fall.Width );
                        row.Lod    = glm::vec4( c.Position, fall.LipFlowDistance + a.Distance );
                        row.Zone   = glm::vec4( fall.Across.x, fall.Across.z, c.Distance - a.Distance,
                                                static_cast<float>( kFallQuadsPerSide ) );
                        row.Body   = body.Body;
                        row.Body.y = 2.0f;
                        row.Medium = glm::vec4( body.Medium.x, body.Medium.y,
                                                Water::RiverFallSpeed( fall.FlowSpeed, a.Drop, zone.Gravity ),
                                                Water::RiverFallSpeed( fall.FlowSpeed, c.Drop, zone.Gravity ) );
                        const size_t at = m_Upload.size();
                        m_Upload.resize( at + sizeof( WaterTileRow ) );
                        std::memcpy( m_Upload.data() + at, &row, sizeof( WaterTileRow ) );
                        ++rowCount;
                    }
            }
        }
        m_Walk.Rows = rowCount;
    }

    void WaterRenderer::FinishPrepare()
    {
        if ( m_Walk.Programs.empty() )
            return;
        // The caustics read it whether or not a tile is in view.
        if ( m_Walk.Flows != m_FlowsBaked || m_RiverImage == nullptr )
        {
            m_FlowsBaked = m_Walk.Flows;
            RetireImage( std::move( m_RiverImage ) );
            m_RiverImage = CreateRiverImage( m_FlowsBaked );
        }
        if ( m_Walk.Rows == 0u )
            return;
        // Each template's cell takes its own bodies' rows and the whole tile upload (its draws index the rows of
        // their own runs; FrameDraw::FirstRow is global).
        for ( const FrameProgram& frame : m_Walk.Programs )
        {
            ApplyMaterials( *frame.Program, frame.Materials );
            if ( auto* tiles = BlocksOf( *frame.Program ).Tiles )
                tiles->SetRawData( m_Upload.data(), static_cast<uint32_t>( m_Upload.size() ) );
            // The front layer draws the same surface: the same `.demat`s folded into its own rows, the same
            // upload.
            if ( frame.FrontLayer != nullptr )
            {
                ApplyMaterials( *frame.FrontLayer, frame.Materials );
                if ( auto* tiles = BlocksOf( *frame.FrontLayer ).Tiles )
                    tiles->SetRawData( m_Upload.data(), static_cast<uint32_t>( m_Upload.size() ) );
                ++m_FrameFrontLayers;
            }
        }
        m_FrameReady = true;
    }

    void WaterRenderer::DeclareDraws( RDG::PassBuilder& pass, const RDG::TextureRef sceneCopy,
                                      const WaterFrameRefs& water, const RDG::TextureRef frontReflection,
                                      const RDG::TextureRef frontDepth, const SceneViewInputs& view, const SceneFrameBinding& frame )
    {
        // Exactly when RenderManual draws.
        if ( m_FrameDraws.empty() || !m_FrameReady || !sceneCopy.IsValid() || !water.IsValid() )
            return;
        // One binding block per template, in TileWalk::Programs' order (RenderManual's block index).
        for ( const FrameProgram& program : m_Walk.Programs )
            DeclareProgramDraws( pass, *program.Program, sceneCopy, water, frontReflection, frontDepth, view,
                                 frame );
    }

    void WaterRenderer::DeclareProgramDraws( RDG::PassBuilder& pass, WaterProgram& program,
                                             const RDG::TextureRef sceneCopy, const WaterFrameRefs& water,
                                             const RDG::TextureRef frontReflection,
                                             const RDG::TextureRef frontDepth, const SceneViewInputs& view,
                                             const SceneFrameBinding& frame )
    {
        // The view's camera, sun, sky cubes and sky look, by block name - as every surface cell gets them.
        frame.ApplyTo( program.Material.get() );
        // RENDER-THREADS M1: the material RenderManual applies - its view sets and block copies exist before
        // the graph records (after the frame binding above, which writes its blocks).
        pass.Prepares( *program.Material->GetMaterialExecutor() );
        const SlottedLayout& slotted = m_Layout.GetSlotted( program.Pipeline->GetSpecification().Shader );
        const auto&          layout  = slotted.Layout;
        auto block = pass.Bindings( layout, program.Material->GetMaterialExecutor()->GetRouteFill() );
        BindSceneViewInputs( block, view, slotted.Slots );
        // The frame's water (shore, river, waves, foam): pass inputs like the scene copy, never material properties.
        block.Texture( WaterShaderParameters::SceneColorTexture, sceneCopy, RDG::Access::SampledGraphics,
                       RDG::SubresourceRange::All() )
             .Texture( WaterShaderParameters::WaterShoreTexture, water.Shore, RDG::Access::SampledGraphics,
                       RDG::SubresourceRange::All() )
             .Texture( WaterShaderParameters::WaterRiverTexture, water.River, RDG::Access::SampledGraphics,
                       RDG::SubresourceRange::All() )
             .Texture( WaterShaderParameters::WaterWavesTexture, water.Waves, RDG::Access::SampledGraphics,
                       RDG::SubresourceRange::All() )
             .Texture( WaterShaderParameters::WaterFoamTexture, water.Foam, RDG::Access::SampledGraphics,
                       RDG::SubresourceRange::All() )
             .Sampler( WaterShaderParameters::LinearRepeatSampler, RDG::SamplerDesc::LinearRepeat() )
             .Sampler( WaterShaderParameters::LinearClampSampler, RDG::SamplerDesc::LinearClamp() );
        // The front layer's traced reflection where this fragment is the front layer (FrontLayerReflection.glslh),
        // fetched by texel through the clamp state above; the caller hands the graph's black for both without one,
        // which the water reads as "no trace" (its captures and sky alone).
        if ( slotted.Slots.Has( SceneViewSlot::FrontLayerReflectionTexture ) )
            block.Texture( WaterShaderParameters::FrontLayerReflectionTexture, frontReflection,
                           RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
                 .Texture( WaterShaderParameters::FrontLayerDepthTexture, frontDepth, RDG::Access::SampledGraphics,
                           RDG::SubresourceRange::All() );
    }

    void WaterRenderer::DeclareFrontLayerDraws( RDG::PassBuilder& pass, const WaterFrameRefs& water,
                                                const SceneViewInputs& view,
                                                const SceneFrameBinding& frame )
    {
        // Exactly when RenderFrontLayerManual draws.
        if ( !HasFrontLayerDraws() || !water.IsValid() )
            return;
        // One binding block per template with a front layer cell, in TileWalk::Programs' order.
        for ( const FrameProgram& program : m_Walk.Programs )
            if ( program.FrontLayer != nullptr )
                DeclareFrontLayerProgramDraws( pass, *program.FrontLayer, water, view, frame );
    }

    void WaterRenderer::DeclareFrontLayerProgramDraws( RDG::PassBuilder& pass, WaterProgram& front,
                                                       const WaterFrameRefs& water, const SceneViewInputs& view,
                                                       const SceneFrameBinding& frame )
    {
        frame.ApplyTo( front.Material.get() );
        pass.Prepares( *front.Material->GetMaterialExecutor() );
        const SlottedLayout& slotted = m_Layout.GetSlotted( front.Pipeline->GetSpecification().Shader );
        auto block = pass.Bindings( slotted.Layout, front.Material->GetMaterialExecutor()->GetRouteFill() );
        // THE CELL'S RESOURCES (census of WaterMesh.FrontLayer = Vertex_WaterMesh.glslh + Pass_FrontLayer.glslh
        // over SurfaceTypes.glslh), every one filled here or by the material - ValidatePassBindings refuses the
        // node otherwise:
        //   the view block and the frame's blocks           - frame.ApplyTo above;
        //   WaterTiles, Materials, the push block           - the material (PrepareFrame, ApplyMaterial);
        //   the bindless textures and samplers              - the material table's bindless set, as every cell;
        //   any SceneViewSlot the layout declares           - BindSceneViewInputs (none today: the front layer
        //                                                     pass reads no scene depth, no shading word);
        //   WaterShoreTexture / River / Waves, the repeat   - here: both stages draw the one water surface.
        // No SceneColorTexture: the front layer draws before the scene copy exists.
        BindSceneViewInputs( block, view, slotted.Slots );
        block.Texture( WaterShaderParameters::WaterShoreTexture, water.Shore, RDG::Access::SampledGraphics,
                       RDG::SubresourceRange::All() )
             .Texture( WaterShaderParameters::WaterRiverTexture, water.River, RDG::Access::SampledGraphics,
                       RDG::SubresourceRange::All() )
             .Texture( WaterShaderParameters::WaterWavesTexture, water.Waves, RDG::Access::SampledGraphics,
                       RDG::SubresourceRange::All() )
             .Sampler( WaterShaderParameters::LinearRepeatSampler, RDG::SamplerDesc::LinearRepeat() );
    }

    Common::BoolResultStr WaterRenderer::RenderManual( const RDG::PassContext& context )
    {
        if ( m_FrameDraws.empty() || !m_FrameReady )
            return BOOLSUCCESS;
        DESERT_PROFILE_PASS( "Water: Surface" );
        // Each template through its own block (DeclareDraws declared them in TileWalk::Programs' order).
        for ( uint32_t slot = 0; slot < m_Walk.Programs.size(); ++slot )
            if ( auto drawn = RecordDraws( context, *m_Walk.Programs[slot].Program, slot, slot ); !drawn )
                return drawn;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr WaterRenderer::RenderFrontLayerManual( const RDG::PassContext& context )
    {
        // The graph opened the render pass on the front layer's targets (after the mesh front layer, or cleared).
        if ( !HasFrontLayerDraws() )
            return BOOLSUCCESS;
        DESERT_PROFILE_PASS( "Water: Front Layer" );
        // The templates with a front layer cell, each through its own block (DeclareFrontLayerDraws' order).
        uint32_t block = 0;
        for ( uint32_t slot = 0; slot < m_Walk.Programs.size(); ++slot )
        {
            const WaterProgram* const front = m_Walk.Programs[slot].FrontLayer;
            if ( front == nullptr )
                continue;
            if ( auto drawn = RecordDraws( context, *front, slot, block++ ); !drawn )
                return drawn;
        }
        return BOOLSUCCESS;
    }

    Common::BoolResultStr WaterRenderer::RecordDraws( const RDG::PassContext& context, const WaterProgram& program,
                                                      const uint32_t programSlot, const uint32_t blockIndex )
    {
        const auto* camera = m_SceneRenderer->GetMainCamera();
        if ( camera == nullptr )
            return BOOLSUCCESS;
        const DataDrivenMaterial& material = *program.Material;

        // RENDER-THREADS M3: the push values travel BY VALUE, the material is only read (DrawPushFields, as
        // TerrainRenderer::RecordDraws): each draw's block is a copy of the material's with the identity matrix,
        // the material row (the cell's one row) and the draw's first tile row over it (PrimitiveIndex: the water
        // has no GpuScene primitive; Vertex_WaterMesh.glslh).
        const RDG::PassBindings       block( context, context.GetBindingBlock( blockIndex ) );
        const MaterialExecutor*       executor = material.GetMaterialExecutor();
        const Common::Memory::Buffer& pushed   = executor->GetPushConstantBuffer();
        std::vector<std::byte>&       push     = t_WaterRecordPush;
        // FPS-MAT-HANDLES3: the cell's push fields placed once per shader code (DrawPushFields::ShaderPlacements,
        // as TerrainRenderer::RecordDraws), copied here so a later Of on this thread cannot move it under the
        // loop; no field is found by name per draw.
        const DrawPushFields::Placement placement =
             DrawPushFields::ThreadShaderPlacements().Of( executor->GetShader().get() );
        for ( const FrameDraw& draw : m_FrameDraws )
        {
            if ( draw.ProgramSlot != programSlot )
                continue; // another template's run: drawn through its own block
            push.assign( pushed.Size, std::byte{ 0 } );
            if ( pushed.Data != nullptr && pushed.Size != 0u )
                std::memcpy( push.data(), pushed.Data, pushed.Size );
            DrawPushFields fields;
            fields.Transform( glm::mat4( 1.0f ) )
                 .MaterialIndex( draw.MaterialSlot )
                 .PrimitiveIndex( draw.FirstRow );
            if ( auto written = fields.WriteInto( push, placement ); !written )
                return Common::MakeFormattedError( "{}: water draw at tile row {}: {}", context.GetPassName(),
                                                   draw.FirstRow, written.GetError() );
            if ( auto drawn = Renderer::GetInstance().DrawProcedural( block, *program.Pipeline, executor,
                                                                      draw.QuadsPerSide * draw.QuadsPerSide * 6u,
                                                                      draw.RowCount, push );
                 !drawn.IsSuccess() )
                return drawn;
        }
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::System
