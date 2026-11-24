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
    // THE FFT OCEAN'S THREE COMPUTE PROGRAMS (WATER-RIVERS-FFT; Programs/Water/WaterFFT*.shader), one spelling.
    struct WaterFFTShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, WaterFFTInitialSpectrum );
        DESERT_SHADER_PARAMETER( StorageTexture, WaterFFTSpectrumA );
        DESERT_SHADER_PARAMETER( StorageTexture, WaterFFTSpectrumB );
        DESERT_SHADER_PARAMETER( StorageTexture, WaterFFTSourceA );
        DESERT_SHADER_PARAMETER( StorageTexture, WaterFFTSourceB );
        DESERT_SHADER_PARAMETER( StorageTexture, WaterFFTTargetA );
        DESERT_SHADER_PARAMETER( StorageTexture, WaterFFTTargetB );
        DESERT_SHADER_PARAMETER( StorageTexture, WaterFFTResultA );
        DESERT_SHADER_PARAMETER( StorageTexture, WaterFFTResultB );
        DESERT_SHADER_PARAMETER( StorageTexture, WaterFFTWaves );
        DESERT_SHADER_PARAMETER( StorageBuffer, WaterFFTParamsBuffer );
        DESERT_SHADER_PARAMETER( StorageBuffer, WaterFFTReadbackBuffer ); // the CPU mirror's texels (finalize)
        static constexpr std::array kSpectrum{ ::Desert::Graphic::RDG::Erase( WaterFFTInitialSpectrum ),
                                               ::Desert::Graphic::RDG::Erase( WaterFFTSpectrumA ),
                                               ::Desert::Graphic::RDG::Erase( WaterFFTSpectrumB ),
                                               ::Desert::Graphic::RDG::Erase( WaterFFTParamsBuffer ) };
        static constexpr std::array kTransform{ ::Desert::Graphic::RDG::Erase( WaterFFTSourceA ),
                                                ::Desert::Graphic::RDG::Erase( WaterFFTSourceB ),
                                                ::Desert::Graphic::RDG::Erase( WaterFFTTargetA ),
                                                ::Desert::Graphic::RDG::Erase( WaterFFTTargetB ),
                                                ::Desert::Graphic::RDG::Erase( WaterFFTParamsBuffer ) };
        static constexpr std::array kFinalize{ ::Desert::Graphic::RDG::Erase( WaterFFTResultA ),
                                               ::Desert::Graphic::RDG::Erase( WaterFFTResultB ),
                                               ::Desert::Graphic::RDG::Erase( WaterFFTWaves ),
                                               ::Desert::Graphic::RDG::Erase( WaterFFTParamsBuffer ),
                                               ::Desert::Graphic::RDG::Erase( WaterFFTReadbackBuffer ) };
    };

    // The slot names the two water cells' passes bind, spelled once (RDGShaderParameters.hpp; UE
    // FSingleLayerWaterPassParameters). The material's own (route fill): WaterTiles, Materials, the push block.
    struct WaterShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, WaterTiles );
        DESERT_SHADER_PARAMETER( StorageBuffer, Materials ); // Core::Formats::kMaterialRowBlockName
        // Mesh/Surface/WaterMeshTiles.glslh binding 25: both cells' vertex stage (the waves damp by the ground),
        // the water's fragment stage (the surf). Fetched by texel, so it brings no sampler of its own.
        DESERT_SHADER_PARAMETER( Texture, WaterInfoTexture );
        // Pass_SingleLayerWater.glslh only: the scene behind the water and the one state its cubes, BRDF LUT,
        // scene copy and front-layer reflection are read through.
        DESERT_SHADER_PARAMETER( Texture, SceneColorTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        // Mesh/Surface/FrontLayerReflection.glslh, as the glass cells bind them (MeshRendererForward.cpp).
        DESERT_SHADER_PARAMETER( Texture, FrontLayerReflectionTexture );
        DESERT_SHADER_PARAMETER( Texture, FrontLayerDepthTexture );
        // Mesh/Surface/WaterMeshTiles.glslh binding 23: the FFT ocean's two-frame atlas (both cells, both stages).
        DESERT_SHADER_PARAMETER( Texture, WaterFFTTexture );
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
            // The tile's body's medium (WaterBodyLane::Medium): x = its TargetWaveMaskDepth (cm), y = its river's
            // two-phase flow period (s; 0 off a river), zw unused.
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
        constexpr uint32_t kMaxWaves    = 32;
        constexpr uint32_t kMaxWaveSets = 4;
        // One body wave set, std430 (WaterMeshTiles.glslh WaterWaveSet; UE: a water body's wave buffer).
        struct WaterWaveSet
        {
            // x = count, y = 1 if it IS the frame's FFT ocean, zw = WaveOrigin
            glm::vec4                           Header{ 0.0f };
            std::array<WaterWaveRow, kMaxWaves> Rows{};
        };
        static_assert( sizeof( WaterWaveSet ) == ( 1 + 2 * kMaxWaves ) * sizeof( glm::vec4 ) );
        struct WaterWavesBlock
        {
            // x = the set count, y = the FFT atlas band this frame writes, z = the WaterInfo atlas's ground region
            // side in texels (= the river region's first row; 0 = no ground), w unused
            glm::vec4 Header{ 0.0f };
            // WaterInfoTexture: xy = lower (X, Z), z = 1 / side, w = 1 when the ground region is baked (each
            // body's mask depth rides its tiles: WaterTileRow::Medium.x)
            glm::vec4 Info{ 0.0f };
            // WATER-RIVERS-FFT (WaterMeshTiles.glslh WaterFFTFrame / WaterFlowFrame): xyz = 1 / cascade patch
            // side, w = 1 + the previous frame's band when a set is the FFT ocean; the river's flow lane.
            glm::vec4 FFT{ 0.0f };
            // WaterFlowFrame: x = 1 when a river is in the frame, y unused (each river's two-phase period rides
            // its tiles: WaterTileRow::Medium.y), zw = the atlas's river region in texels (columns, rows).
            glm::vec4 Flow{ 0.0f };
            // WaterRiverFrame: xy = the river region's lower world (X, Z), zw = 1 / its texel along X and Z (1/cm).
            // Its first row is the ground region's side, Header.z.
            glm::vec4                              River{ 0.0f };
            std::array<WaterWaveSet, kMaxWaveSets> Sets{};
        };
        static_assert( sizeof( WaterWavesBlock ) ==
                       ( 5 + kMaxWaveSets * ( 1 + 2 * kMaxWaves ) ) * sizeof( glm::vec4 ) );
        static_assert( sizeof( WaterWavesBlock ) == WaterRenderer::kWavesBlockBytes, "WATER-CAUSTICS reads the head" );

        // THE WATER INFO ATLAS as the GPU samples it (UE WaterInfo: ground depth + river height and velocity): one
        // RGBA32F image of two regions, each on its OWN grid so neither is resampled (WaterMeshTiles.glslh):
        //   rows [0, G)            the ground region, G x G: .r = the ground height (cm), its own WaterInfoGround
        //                          grid verbatim (WaterGroundDepth);
        //   rows [G, G + R.y)      the river region, R.x x R.y at the rivers' finest texel (50 cm): (flow x, flow z
        //                          cm/s, bank mask, still surface Y cm) - one river's region IS its bake's grid, so
        //                          the GPU's height is the very texels Water::SampleRiverSurfaceY reads (WaterFlowAt,
        //                          WaterOutsideBank, WaterRiverSurfaceY).
        std::shared_ptr<Image2D> CreateWaterInfoImage( const uint32_t width, const uint32_t height,
                                                       const std::vector<glm::vec4>& texels )
        {
            std::vector<unsigned char> bytes( texels.size() * sizeof( glm::vec4 ) );
            std::memcpy( bytes.data(), texels.data(), bytes.size() );
            Core::Formats::Image2DSpecification spec = {
                 .Tag        = "WaterInfo",
                 .Width      = width,
                 .Height     = height,
                 .Format     = Core::Formats::ImageFormat::RGBA32F,
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

        // The atlas's two regions (CreateWaterInfoImage). GroundSize 0: no ground; RiverSize 0: no river.
        struct WaterInfoLayout
        {
            glm::vec2  GroundMin{ 0.0f };
            float      GroundExtent = 0.0f;
            uint32_t   GroundSize   = 0u;
            glm::vec2  RiverMin{ 0.0f };
            glm::vec2  RiverTexel{ 0.0f }; // cm per texel along X and Z (a bake's texels need not be square)
            glm::uvec2 RiverSize{ 0u };

            [[nodiscard]] uint32_t Width() const noexcept
            {
                return std::max( { GroundSize, RiverSize.x, 1u } );
            }
            [[nodiscard]] uint32_t Height() const noexcept
            {
                return std::max( GroundSize + RiverSize.y, 1u );
            }
        };

        bool HasGroundHeights( const Water::WaterInfoGround* ground ) noexcept
        {
            return ground != nullptr && ground->Size > 0u && ground->ExtentCm > 0.0f &&
                   ground->HeightCm.size() == static_cast<size_t>( ground->Size ) * ground->Size;
        }

        // The ground region is the ground's own grid; the river region is one river's bake grid, or for several the
        // rectangle over all of them at the finest of their texels (coarsened so no side exceeds
        // Water::kRiverFlowMaxSize, as one long river's bake is).
        WaterInfoLayout
        WaterInfoLayoutOf( const Water::WaterInfoGround*                                 ground,
                           std::span<const std::shared_ptr<const Water::RiverFlowField>> flows ) noexcept
        {
            WaterInfoLayout layout;
            if ( HasGroundHeights( ground ) )
            {
                layout.GroundMin    = ground->Min;
                layout.GroundExtent = ground->ExtentCm;
                layout.GroundSize   = ground->Size;
            }
            if ( flows.empty() )
                return layout;
            if ( flows.size() == 1u )
            {
                const Water::RiverFlowField& flow = *flows.front();
                layout.RiverMin                   = flow.Min;
                layout.RiverTexel = flow.Extent / glm::vec2( glm::max( flow.Size, glm::uvec2( 1u ) ) );
                layout.RiverSize  = flow.Size;
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
            layout.RiverMin   = lo;
            layout.RiverTexel = glm::vec2( texel );
            layout.RiverSize  = glm::uvec2( glm::clamp( glm::ceil( span / texel ), glm::vec2( 1.0f ),
                                                        glm::vec2( static_cast<float>( Water::kRiverFlowMaxSize ) ) ) );
            return layout;
        }

        // The atlas's texels (row-major by Z, Width() per row): the ground's heights copied, and per river texel the
        // texel of the first river whose banks hold its centre (the nearest bake texel; a lone river's are copied
        // one to one). kWaterInfoNoGroundCm where there is no ground, zeros outside every bank.
        std::vector<glm::vec4>
        ComposeWaterInfo( const WaterInfoLayout& layout, const Water::WaterInfoGround* ground,
                          std::span<const std::shared_ptr<const Water::RiverFlowField>> flows )
        {
            const uint32_t         width = layout.Width();
            std::vector<glm::vec4> texels( static_cast<size_t>( width ) * layout.Height(),
                                           glm::vec4( Water::kWaterInfoNoGroundCm, 0.0f, 0.0f, 0.0f ) );
            for ( uint32_t z = 0; z < layout.GroundSize; ++z )
                for ( uint32_t x = 0; x < layout.GroundSize; ++x )
                    texels[static_cast<size_t>( z ) * width + x].x =
                         ground->HeightCm[static_cast<size_t>( z ) * layout.GroundSize + x];
            for ( uint32_t z = 0; z < layout.RiverSize.y; ++z )
                for ( uint32_t x = 0; x < layout.RiverSize.x; ++x )
                {
                    glm::vec4& texel = texels[static_cast<size_t>( layout.GroundSize + z ) * width + x];
                    texel            = glm::vec4( 0.0f );
                    const glm::vec2 world =
                         layout.RiverMin + ( glm::vec2( static_cast<float>( x ), static_cast<float>( z ) ) + 0.5f ) *
                                                layout.RiverTexel;
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
                        texel = glm::vec4( f.x, f.y, f.z,
                                           at < flow->SurfaceHeightCm.size() ? flow->SurfaceHeightCm[at] : flow->SurfaceY );
                        break;
                    }
                }
            return texels;
        }

        // The still surface's vertical extent the quadtree culls with when the body has no waves.
        constexpr float kFlatSurfaceVerticalExtentCm = 1.0f;

        // Water/WaterFFT.glslh WaterFFTParams, std430.
        struct WaterFFTParams
        {
            glm::vec4 Cascade{ 0.0f }; // xyz = cascade patch sides (cm), w = the view's time (s)
            glm::vec4 Shape{ 0.0f };   // x = choppiness, y = IFFT direction, z = the atlas band this frame writes
            // The foam history: x = the share of the previous half's foam kept, y = 1 + the previous half's first
            // band (0 = no history).
            glm::vec4 Foam{ 0.0f };
        };
        static_assert( sizeof( WaterFFTParams ) == 3 * sizeof( glm::vec4 ) );
        constexpr uint32_t kFFTN = Water::kFFTOceanResolution;
        // WATER_FFT_BANDS_PER_FRAME: the cascades' displacement and slopes, then the foam history band.
        constexpr uint32_t kFFTBandsPerFrame = 2 * Water::kFFTOceanCascades + 1;
        constexpr uint32_t kFFTGroups8       = kFFTN / 8;                    // LocalSize(8, 8, 1)

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
                                                .WaterInfo     = inputs.WaterInfo,
                                                .FFTWaves      = inputs.FFTWaves,
                                                .SunTravel     = inputs.SunTravel,
                                                .Camera        = inputs.Camera,
                                                .TimeSeconds   = inputs.TimeSeconds,
                                                .Black         = inputs.Black };
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
        m_FrameWavesTexture = RDG::TextureRef{};
        m_FrameWavesGraph   = nullptr;
        m_FrameBodies.clear();
        m_FrameBodyWaveLanes.clear();
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
        // draw with MaterialIndex = that row, through that program). Each body's TargetWaveMaskDepth and river
        // flow period ride its own lane (WaterBodyLane::Medium -> WaterTileRow::Medium): one value per body, never
        // the first body's.
        Assets::AssetHandle                           material = Assets::AssetHandle::Null();
        std::shared_ptr<const Water::WaterInfoGround> ground;
        for ( const WaterZoneDraw& zone : m_Queue )
            if ( ground == nullptr )
                ground = zone.Ground;
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
                // The body's own medium (UE: per-body TargetWaveMaskDepth and the river's flow): its mask depth
                // (WaterBodyData::TargetWaveMaskDepth, clamped as Water::QueryWaterBody clamps it) and, on a
                // river, its bake's two-phase period (WaterBodyData::RiverFlowPeriod ->
                // Water::RiverFlowField::FlowPeriod).
                const float maskDepth = b < bodies.WaveMaskDepths.size() ? bodies.WaveMaskDepths[b] : 0.0f;
                const std::shared_ptr<const Water::RiverFlowField>& flow =
                     b < bodies.Flows.size() ? bodies.Flows[b] : std::shared_ptr<const Water::RiverFlowField>{};
                const float flowPeriod = river && flow != nullptr ? std::max( flow->FlowPeriod, 0.05f ) : 0.0f;
                bodyLanes[z][b] =
                     WaterBodyLane{ .Body   = glm::vec4( static_cast<float>( slot - frame->Materials.begin() ),
                                                       river ? 1.0f : 0.0f, waveLane, static_cast<float>( b ) ),
                                    .Medium = glm::vec4( std::max( maskDepth, 1.0f ), flowPeriod, 0.0f, 0.0f ) };
                // Every body that draws this frame (WATER-CAUSTICS: its footprint, still level and wave set).
                m_FrameBodies.push_back( bodies.Footprints[b] );
                m_FrameBodyWaveLanes.push_back( waveLane );
            }
        }
        if ( programs.empty() )
            return; // no body's template builds: no water (each refusal said once)

        // THE BODIES' WAVES (UE: each water body's wave buffer the water vertex factory reads): each set generated
        // once per asset (Runtime::WaterWavesService), packed ahead of the tiles. Still loading = flat this frame.
        // THE FFT OCEAN replaces a set's Gerstner rows where the set is one and the tier asks for it: the first such
        // set is the frame's FFT ocean (one atlas); another FFT set draws its Gerstner generator.
        WaterWavesBlock waves;
        waves.Header.x       = static_cast<float>( waveSets.size() );
        float verticalExtent = kFlatSurfaceVerticalExtentCm;
        bool  fft            = false;
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
                verticalExtent = std::max( verticalExtent, Water::MaxWaveHeight( rows ) );
            }
            if ( !fft && PrepareFFTFrame( frameSet.Material, stamp, frameSet.Asset, waves.Header, waves.FFT,
                                          verticalExtent ) )
            {
                fft          = true;
                set.Header.y = 1.0f;
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

        // The waves block is whole here (its WaterInfo lanes name the atlas's two regions FinishPrepare composes
        // from the same ground and rivers): the upload's head, the tile task appends the rows.
        const WaterInfoLayout infoLayout = WaterInfoLayoutOf( ground.get(), rivers );
        if ( infoLayout.GroundSize > 0u )
            waves.Info = glm::vec4( infoLayout.GroundMin, 1.0f / infoLayout.GroundExtent, 1.0f );
        // THE FLOW LANES (WaterMeshTiles.glslh WaterFlowFrame / WaterRiverFrame): on while a river is in the
        // frame; each river's two-phase period is its own tiles' (WaterTileRow::Medium.y).
        if ( !rivers.empty() )
        {
            waves.Flow  = glm::vec4( 1.0f, 0.0f, static_cast<float>( infoLayout.RiverSize.x ),
                                     static_cast<float>( infoLayout.RiverSize.y ) );
            waves.River = glm::vec4( infoLayout.RiverMin, 1.0f / glm::max( infoLayout.RiverTexel, glm::vec2( 1e-3f ) ) );
        }
        waves.Header.z = static_cast<float>( infoLayout.GroundSize );
        m_Upload.resize( sizeof( WaterWavesBlock ) );
        std::memcpy( m_Upload.data(), &waves, sizeof( WaterWavesBlock ) );

        const glm::mat4 view     = camera->GetViewMatrix();
        const glm::mat4 proj     = camera->GetProjectionMatrix();
        const glm::vec3 observer = glm::vec3( glm::inverse( view )[3] );
        // The lens stretches every water LOD ring (VIEW-LOD-FOV): the one view scale meshes, foliage and terrain
        // read, so a 10-degree scope keeps the far sea as fine as it looks.
        const float lensScale = DrawDistanceScale( Geometry::LODView{ .Origin = observer, .Projection = proj } );
        m_Walk                = TileWalk{ .Programs       = std::move( programs ),
                                          .Stamp          = stamp,
                                          .Ground         = std::move( ground ),
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
        };
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
                state.TargetWaveMaskDepth = bodies.WaveMaskDepths[b];
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
                    auto spectrum = service->GetSpectrum( bodies.Waves[b] );
                    if ( spectrum && spectrum.GetValue() )
                        state.FFTSurface = service->Surfaces().Latest( spectrum.GetValue().get() );
                }
                if ( !Water::WaterBodyCovers( state, camera ) )
                    continue;
                const Water::WaterQueryResult hit   = Water::QueryWaterBody( state, camera, time, std::nullopt );
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

    void WaterRenderer::DeclareUnderwater( RDG::PassBuilder& pass, const RDG::TextureRef sceneCopy,
                                           const SceneViewInputs& view, const SceneFrameBinding& frame )
    {
        if ( !m_Underwater.Active || m_UnderwaterPipeline == nullptr || !sceneCopy.IsValid() )
            return;
        const auto sun            = m_SceneRenderer->GetFrameSun();
        m_Underwater.ToSun        = glm::vec4( -glm::vec3( sun.Direction ), 0.0f );
        m_Underwater.SunRadiance  = glm::vec4( glm::vec3( sun.ColorIntensity ) * sun.ColorIntensity.a, 0.0f );
        const SkyLookGPU look     = ToGPU( frame.EnvironmentLook );
        m_Underwater.SkyYawCosSin = look.YawCosSin;
        m_Underwater.SkyGain      = look.Gain;
        using P                   = WaterUnderwaterShaderParameters;
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
             .PushConstants<ShaderPush::WaterUnderwater>();
    }

    Common::BoolResultStr WaterRenderer::RenderUnderwaterManual( const RDG::PassContext& context )
    {
        if ( !m_Underwater.Active || m_UnderwaterPipeline == nullptr )
            return BOOLSUCCESS;
        const ShaderPush::WaterUnderwater push{
             .InvViewProjection = m_Underwater.InvViewProjection,
             .CameraSurface     = glm::vec4( m_Underwater.Camera, m_Underwater.SurfaceY ),
             .Scattering        = glm::vec4( m_Underwater.Scattering, m_Underwater.PhaseG ),
             .Absorption        = glm::vec4( m_Underwater.Absorption, m_Underwater.PreExposure ),
             .ToSun             = m_Underwater.ToSun,
             .SunRadiance       = m_Underwater.SunRadiance,
             .SkyYawCosSin      = m_Underwater.SkyYawCosSin,
             .SkyGain           = m_Underwater.SkyGain };
        RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        bindings.PushConstants( push );
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
        if ( m_Walk.Programs.empty() || m_Walk.Rows == 0u )
            return;

        // THE WATER INFO TEXTURE (UE WaterInfo: ground + river velocity): re-composed only when the collector
        // baked a new ground or the frame's rivers changed. Over no landscape and no river it is one texel of
        // kWaterInfoNoGroundCm - a bottom no wave reaches - and no flow.
        const std::shared_ptr<const Water::WaterInfoGround>& ground = m_Walk.Ground;
        if ( ground != m_GroundBaked || m_Walk.Flows != m_FlowsBaked || m_WaterInfo == nullptr )
        {
            m_GroundBaked = ground;
            m_FlowsBaked  = m_Walk.Flows;
            // The same layout PrepareFrame wrote the lanes from (one function of the same ground and rivers).
            const WaterInfoLayout layout = WaterInfoLayoutOf( ground.get(), m_Walk.Flows );
            m_WaterInfo = CreateWaterInfoImage( layout.Width(), layout.Height(),
                                                ComposeWaterInfo( layout, ground.get(), m_Walk.Flows ) );
        }
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
                                      const RDG::TextureRef waterInfo, const RDG::TextureRef waves,
                                      const RDG::TextureRef frontReflection, const RDG::TextureRef frontDepth,
                                      const SceneViewInputs& view, const SceneFrameBinding& frame )
    {
        // Exactly when RenderManual draws.
        if ( m_FrameDraws.empty() || !m_FrameReady || !sceneCopy.IsValid() || !waterInfo.IsValid() ||
             !waves.IsValid() )
            return;
        // One binding block per template, in TileWalk::Programs' order (RenderManual's block index).
        for ( const FrameProgram& program : m_Walk.Programs )
            DeclareProgramDraws( pass, *program.Program, sceneCopy, waterInfo, waves, frontReflection, frontDepth,
                                 view, frame );
    }

    void WaterRenderer::DeclareProgramDraws( RDG::PassBuilder& pass, WaterProgram& program,
                                             const RDG::TextureRef sceneCopy, const RDG::TextureRef waterInfo,
                                             const RDG::TextureRef waves, const RDG::TextureRef frontReflection,
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
        // The WaterInfo ground (UE: the water info texture a view's water pass reads): a pass input like the scene
        // copy, never a material property.
        block.Texture( WaterShaderParameters::SceneColorTexture, sceneCopy, RDG::Access::SampledGraphics,
                       RDG::SubresourceRange::All() )
             .Texture( WaterShaderParameters::WaterInfoTexture, waterInfo, RDG::Access::SampledGraphics,
                       RDG::SubresourceRange::All() )
             .Texture( WaterShaderParameters::WaterFFTTexture, waves, RDG::Access::SampledGraphics,
                       RDG::SubresourceRange::All() )
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

    void WaterRenderer::DeclareFrontLayerDraws( RDG::PassBuilder& pass, const RDG::TextureRef waterInfo,
                                                const RDG::TextureRef waves, const SceneViewInputs& view,
                                                const SceneFrameBinding& frame )
    {
        // Exactly when RenderFrontLayerManual draws.
        if ( !HasFrontLayerDraws() || !waterInfo.IsValid() || !waves.IsValid() )
            return;
        // One binding block per template with a front layer cell, in TileWalk::Programs' order.
        for ( const FrameProgram& program : m_Walk.Programs )
            if ( program.FrontLayer != nullptr )
                DeclareFrontLayerProgramDraws( pass, *program.FrontLayer, waterInfo, waves, view, frame );
    }

    void WaterRenderer::DeclareFrontLayerProgramDraws( RDG::PassBuilder& pass, WaterProgram& front,
                                                       const RDG::TextureRef waterInfo,
                                                       const RDG::TextureRef waves, const SceneViewInputs& view,
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
        //   WaterInfoTexture                                - here: the waves damp by the ground in the vertex
        //                                                     stage, as the water's own cell's do.
        // No shared sampler: Pass_FrontLayer.glslh samples nothing through one, WaterInfoTexture is fetched by
        // texel, the scene depth's PointClampSampler is the Forward cells' (SurfaceTypes.glslh) - and a shared
        // sampler no stage reads fails the shader map build, so the cell declares none and the pass binds none
        // (a bound slot the shader lacks is refused: "is not a resource of shader"). No SceneColorTexture either:
        // the front layer draws before the scene copy exists.
        BindSceneViewInputs( block, view, slotted.Slots );
        block.Texture( WaterShaderParameters::WaterInfoTexture, waterInfo, RDG::Access::SampledGraphics,
                       RDG::SubresourceRange::All() )
             .Texture( WaterShaderParameters::WaterFFTTexture, waves, RDG::Access::SampledGraphics,
                       RDG::SubresourceRange::All() );
    }

    bool WaterRenderer::EnsureFFTPipelines()
    {
        if ( m_FFTTried )
            return m_FFTSpectrum != nullptr && m_FFTTransform != nullptr && m_FFTFinalize != nullptr;
        m_FFTTried        = true;
        auto* const shaders = Runtime::ResourceRegistry::GetShaderService();
        const auto  build   = [shaders]( const char* name, ShaderBindingLayoutCache& layout,
                                      std::span<const RDG::ShaderParameter> parameters )
             -> std::shared_ptr<ComputePipeline>
        {
            const auto shader = shaders != nullptr ? shaders->GetByName( name ) : nullptr;
            if ( !shader )
            {
                LOG_ERROR( "[Water] the FFT ocean's compute shader '{}' is missing - the waves draw Gerstner", name );
                return nullptr;
            }
            auto pipeline = ComputePipeline::Create( { .Shader = shader, .DebugName = name } );
            if ( !pipeline )
            {
                LOG_ERROR( "[Water] '{}' does not build: {} - the waves draw Gerstner", name, pipeline.GetError() );
                return nullptr;
            }
            if ( const auto valid = layout.Validate( pipeline.GetValue()->GetSpecification().Shader, parameters );
                 !valid )
            {
                LOG_ERROR( "[Water] '{}': {} - the waves draw Gerstner", name, valid.GetError() );
                return nullptr;
            }
            return pipeline.GetValue();
        };
        m_FFTSpectrum  = build( "WaterFFTSpectrum", m_FFTSpectrumLayout, WaterFFTShaderParameters::kSpectrum );
        m_FFTTransform = build( "WaterFFT", m_FFTTransformLayout, WaterFFTShaderParameters::kTransform );
        m_FFTFinalize  = build( "WaterFFTFinalize", m_FFTFinalizeLayout, WaterFFTShaderParameters::kFinalize );
        return m_FFTSpectrum != nullptr && m_FFTTransform != nullptr && m_FFTFinalize != nullptr;
    }

    bool WaterRenderer::PrepareFFTFrame( const Assets::AssetHandle& material, const MaterialStamp& stamp,
                                         const Assets::AssetHandle& wavesAsset, glm::vec4& header, glm::vec4& fft,
                                         float& verticalExtent )
    {
        if ( static_cast<uint64_t>( wavesAsset ) == 0 )
            return false;
        const Common::Scalability::ResolvedQuality& quality = Common::Scalability::QualityState::Resolved();
        if ( quality.As<Common::Scalability::WaterWaveMethod>(
                  Common::Scalability::Parameter::EffectsWaterWaveMethod ) !=
             Common::Scalability::WaterWaveMethod::FFT )
            return false;
        auto* const service = Runtime::GetWaterWavesService();
        if ( service == nullptr )
            return false; // PrepareFrame said NoWavesService
        Common::ResultStr<Runtime::WaterWavesService::Spectrum> got = service->GetSpectrum( wavesAsset );
        if ( !got )
        {
            if ( FirstWarning( material, WaterWarning::SpectrumRefused ) )
                Warn( material, std::format( "its body's FFT ocean is refused: {}", got.GetError() ) );
            return false;
        }
        const Runtime::WaterWavesService::Spectrum spectrum = got.GetValue();
        if ( spectrum == nullptr )
            return false; // a Gerstner set, or still loading
        if ( !EnsureFFTPipelines() )
        {
            WarnOnce( material, WaterWarning::FFTPipelinesMissing,
                      "its body's FFT ocean has no compute programs - drawn with its Gerstner generator" );
            return false;
        }

        constexpr size_t kTexels = size_t( kFFTN ) * kFFTN;
        if ( spectrum != m_FFTUploaded )
        {
            std::vector<unsigned char> bytes( Water::kFFTOceanCascades * kTexels * sizeof( glm::vec4 ) );
            for ( uint32_t c = 0; c < Water::kFFTOceanCascades; ++c )
            {
                const std::vector<glm::vec4>& h0 = spectrum->Cascades[c].H0;
                if ( h0.size() != kTexels )
                {
                    if ( FirstWarning( material, WaterWarning::SpectrumMalformed ) )
                        Warn( material, std::format( "its FFT ocean's cascade {} has {} texels, not {} - drawn "
                                                     "with its Gerstner generator",
                                                     c, h0.size(), kTexels ) );
                    return false;
                }
                std::memcpy( bytes.data() + c * kTexels * sizeof( glm::vec4 ), h0.data(),
                             kTexels * sizeof( glm::vec4 ) );
            }
            Core::Formats::Image2DSpecification initial = {
                 .Tag        = "WaterFFTInitialSpectrum",
                 .Width      = kFFTN,
                 .Height     = kFFTN * Water::kFFTOceanCascades,
                 .Format     = Core::Formats::ImageFormat::RGBA32F,
                 .Mips       = 1,
                 .Samples    = 1,
                 .Data       = Core::Formats::EmptyPixelData{},
                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                 .Properties = Core::Formats::ImageProperties::Sample,
                 .MipLevels  = {},
            };
            initial.Data = std::move( bytes );
            m_FFTInitial = Image2D::Create( initial );
            if ( m_FFTWaves == nullptr )
                m_FFTWaves = Image2D::Create( Core::Formats::Image2DSpecification{
                     .Tag        = "WaterFFTWaves",
                     .Width      = kFFTN,
                     .Height     = kFFTN * 2 * kFFTBandsPerFrame,
                     .Format     = Core::Formats::ImageFormat::RGBA16F,
                     .Mips       = 1,
                     .Usage      = Core::Formats::Image2DUsage::Image2D,
                     .Properties = Core::Formats::Storage | Core::Formats::Sample,
                } );
            m_FFTUploaded    = spectrum;
            m_FFTWrittenHalf = -1; // the other half holds another sea
        }
        if ( m_FFTInitial == nullptr || m_FFTWaves == nullptr )
            return false;

        // The ping-pong: this frame writes the half the last recorded frame did not; with no such frame both
        // lanes name this frame's half (a zero wave velocity for one frame). It flips once per frame TIME, not per
        // PrepareFrame: a second view of the same frame (the same ViewFrame::TimeSeconds) rewrites the same half
        // with the same waves and reads the same previous half, so the velocity stays one frame's.
        const ViewFrame* const viewFrame = m_SceneRenderer->GetViewFrame();
        const double           time      = viewFrame != nullptr ? viewFrame->TimeSeconds : 0.0;
        if ( !m_FFTFlipped || time != m_FFTFlipTime || m_FFTWrittenHalf < 0 )
        {
            // THE FOAM HISTORY keeps the written half's foam faded over the time since its flip (a paused world:
            // dt 0, kept whole; a jump back in time: nothing kept rather than foam from the future).
            // The lifetime is the body MATERIAL's FoamDecaySeconds (one truth with the Gerstner path's
            // WaterFoamAmount), re-read only when the material or the stamp moved.
            if ( m_FFTFoamDecay.Material != static_cast<uint64_t>( material ) ||
                 !( m_FFTFoamDecay.Stamp == stamp ) )
            {
                m_FFTFoamDecay          = FoamDecayRead{};
                m_FFTFoamDecay.Material = static_cast<uint64_t>( material );
                m_FFTFoamDecay.Stamp    = stamp;
                constexpr std::array<std::string_view, 1> kName{ "FoamDecaySeconds" };
                std::array<glm::vec4, 1>                  value{};
                m_FFTFoamDecay.Readable = ReadMaterialValues( material, stamp, kName, value );
                m_FFTFoamDecay.Seconds  = value[0].x;
                if ( !m_FFTFoamDecay.Readable )
                    Warn( material, "its FFT ocean's foam history is off: its template has no FoamDecaySeconds" );
            }
            const double dt   = time - m_FFTFlipTime;
            m_FFTFoamHistory  = m_FFTFlipped && m_FFTWrittenHalf >= 0 && m_FFTFoamDecay.Readable;
            m_FFTFoamKeep     = m_FFTFoamHistory && dt >= 0.0
                                     ? static_cast<float>( std::exp(
                                        -dt / static_cast<double>( std::max( m_FFTFoamDecay.Seconds, 0.01f ) ) ) )
                                     : 0.0f;
            m_FFTHalf         = m_FFTWrittenHalf == 0 ? 1 : 0;
            m_FFTPreviousHalf = m_FFTWrittenHalf >= 0 ? m_FFTWrittenHalf : m_FFTHalf;
            m_FFTFlipTime     = time;
            m_FFTFlipped      = true;
        }
        const int32_t previous = m_FFTPreviousHalf;
        const auto&   sizes    = spectrum->Params.CascadeSizes;
        header.y              = static_cast<float>( m_FFTHalf * int32_t( kFFTBandsPerFrame ) );
        fft = glm::vec4( 1.0f / sizes[0], 1.0f / sizes[1], 1.0f / sizes[2],
                         1.0f + static_cast<float>( previous * int32_t( kFFTBandsPerFrame ) ) );
        // The cull extent: the vertical reach, and the choppy map moves the surface sideways by up to lambda times
        // it (the quadtree culls by a vertical extent only: the larger of the two bounds both).
        verticalExtent = std::max( { verticalExtent, spectrum->MaxWaveHeight,
                                     spectrum->MaxWaveHeight * spectrum->Params.Choppiness } );
        m_FFTFrame     = spectrum;
        return true;
    }

    void WaterRenderer::PublishFFTReadback( const uint32_t slot )
    {
        FFTReadbackSlot& read     = m_FFTReadback[slot];
        auto             spectrum = std::exchange( read.Spectrum, nullptr );
        if ( spectrum == nullptr || !read.Buffer.Physical )
            return; // nothing extracted into this slot yet, or its graph faulted before the extract
        const std::span<const std::byte> bytes = read.Buffer.Physical->ReadForHost();
        Water::FFTSurfaceRead            surface;
        surface.Time = read.Time;
        if ( bytes.size() >= Water::kFFTReadbackTexels * sizeof( glm::vec4 ) )
        {
            surface.Texels.resize( Water::kFFTReadbackTexels );
            std::memcpy( surface.Texels.data(), bytes.data(), surface.Texels.size() * sizeof( glm::vec4 ) );
        }
        else
            WarnOnce( Assets::AssetHandle::Null(), WaterWarning::FFTReadbackUnreadable,
                      std::format( "[Water] the FFT read-back is not host-readable ({} bytes): what floats rides "
                                   "the Gerstner waves",
                                   bytes.size() ) );
        read.Buffer.Physical.reset();
        if ( !surface.Texels.empty() )
            Runtime::GetWaterWavesService()->Surfaces().Publish( spectrum, std::move( surface ) );
    }

    RDG::TextureRef WaterRenderer::AddWavePasses( RDG::Builder& graph, const RDG::TextureRef initial,
                                                  const RDG::TextureRef waves, const float timeSeconds )
    {
        if ( !FrameWavesAreFFT() || !initial.IsValid() || !waves.IsValid() )
            return RDG::TextureRef{};
        if ( const RDG::TextureRef recorded = RecordedWaves( graph ); recorded.IsValid() )
            return recorded;
        const auto& sizes = m_FFTFrame->Params.CascadeSizes;
        const float band  = static_cast<float>( m_FFTHalf * int32_t( kFFTBandsPerFrame ) );
        // The foam history's lane: the previous half's first band + 1, 0 = no history (a new sea).
        const float previousFoam =
             m_FFTFoamHistory ? 1.0f + static_cast<float>( m_FFTPreviousHalf * int32_t( kFFTBandsPerFrame ) ) : 0.0f;
        const WaterFFTParams rows{ .Cascade = glm::vec4( sizes[0], sizes[1], sizes[2], timeSeconds ),
                                   .Shape   = glm::vec4( m_FFTFrame->Params.Choppiness, 0.0f, band, 0.0f ),
                                   .Foam    = glm::vec4( m_FFTFoamKeep, previousFoam, 0.0f, 0.0f ) };
        WaterFFTParams       columns = rows;
        columns.Shape.y              = 1.0f;
        const RDG::BufferRef rowsBuffer = graph.CreateBuffer( RDG::BufferDesc{ sizeof( WaterFFTParams ) },
                                                              "WaterFFT.ParamsRows" );
        const RDG::BufferRef columnsBuffer =
             graph.CreateBuffer( RDG::BufferDesc{ sizeof( WaterFFTParams ) }, "WaterFFT.ParamsColumns" );
        graph.QueueBufferUpload( rowsBuffer, std::as_bytes( std::span( &rows, 1 ) ) );
        graph.QueueBufferUpload( columnsBuffer, std::as_bytes( std::span( &columns, 1 ) ) );

        const RDG::TextureDesc spectrumDesc{ .Size   = { kFFTN, kFFTN, 1 },
                                             .Format = Core::Formats::ImageFormat::RGBA32F,
                                             .Layers = Water::kFFTOceanCascades,
                                             .Dim    = RDG::TextureDim::Tex2D };
        const RDG::TextureRef specA = graph.CreateTexture( spectrumDesc, "WaterFFT.SpectrumA" );
        const RDG::TextureRef specB = graph.CreateTexture( spectrumDesc, "WaterFFT.SpectrumB" );
        const RDG::TextureRef rowA  = graph.CreateTexture( spectrumDesc, "WaterFFT.RowsA" );
        const RDG::TextureRef rowB  = graph.CreateTexture( spectrumDesc, "WaterFFT.RowsB" );
        const RDG::TextureRef colA  = graph.CreateTexture( spectrumDesc, "WaterFFT.ColumnsA" );
        const RDG::TextureRef colB  = graph.CreateTexture( spectrumDesc, "WaterFFT.ColumnsB" );

        using P = WaterFFTShaderParameters;
        graph.AddPass(
             "Water: FFT Spectrum", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( m_FFTSpectrumLayout.Get( m_FFTSpectrum->GetSpecification().Shader ),
                                Renderer::GetPipelineRouteFill( *m_FFTSpectrum ) )
                      .Texture( P::WaterFFTInitialSpectrum, initial, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All() )
                      .Storage( P::WaterFFTSpectrumA, specA, RDG::Access::StorageWrite, 0 )
                      .Storage( P::WaterFFTSpectrumB, specB, RDG::Access::StorageWrite, 0 )
                      .Storage( P::WaterFFTParamsBuffer, rowsBuffer, RDG::Access::StorageRead );
             },
             [pipeline = m_FFTSpectrum]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, kFFTGroups8, kFFTGroups8,
                                                                 Water::kFFTOceanCascades );
             } );
        // Rows, then columns: one 256-thread group per line per cascade (WaterFFT.shader LocalSize(256, 1, 1)).
        const auto transform = [&]( const char* name, const RDG::TextureRef srcA, const RDG::TextureRef srcB,
                                    const RDG::TextureRef dstA, const RDG::TextureRef dstB,
                                    const RDG::BufferRef params )
        {
            graph.AddPass(
                 name, RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_FFTTransformLayout.Get( m_FFTTransform->GetSpecification().Shader ),
                                    Renderer::GetPipelineRouteFill( *m_FFTTransform ) )
                          .Storage( P::WaterFFTSourceA, srcA, RDG::Access::StorageRead, 0 )
                          .Storage( P::WaterFFTSourceB, srcB, RDG::Access::StorageRead, 0 )
                          .Storage( P::WaterFFTTargetA, dstA, RDG::Access::StorageWrite, 0 )
                          .Storage( P::WaterFFTTargetB, dstB, RDG::Access::StorageWrite, 0 )
                          .Storage( P::WaterFFTParamsBuffer, params, RDG::Access::StorageRead );
                 },
                 [pipeline = m_FFTTransform]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, 1u, kFFTN,
                                                                     Water::kFFTOceanCascades );
                 } );
        };
        transform( "Water: FFT Rows", specA, specB, rowA, rowB, rowsBuffer );
        transform( "Water: FFT Columns", rowA, rowB, colA, colB, columnsBuffer );
        // THE CPU MIRROR's slot: this frame in flight's buffer, read (and published) before it is written again.
        const uint32_t slot  = EngineContext::GetInstance().GetCurrentFrameIndex();
        const uint32_t slots = EngineContext::GetInstance().GetMaxFramesInFlight();
        if ( m_FFTReadback.size() != slots )
            m_FFTReadback.assign( slots, FFTReadbackSlot{} );
        const bool readback = slot < slots;
        if ( readback )
            PublishFFTReadback( slot );
        const RDG::BufferRef readbackBuffer = graph.CreateBuffer(
             RDG::BufferDesc{ Water::kFFTReadbackTexels * sizeof( glm::vec4 ) }, "WaterFFT.Readback" );
        // This frame's half of the persistent atlas; StorageWrite is read-modify-write, so the other half (the
        // previous frame) survives.
        graph.AddPass(
             "Water: FFT Finalize", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( m_FFTFinalizeLayout.Get( m_FFTFinalize->GetSpecification().Shader ),
                                Renderer::GetPipelineRouteFill( *m_FFTFinalize ) )
                      .Storage( P::WaterFFTResultA, colA, RDG::Access::StorageRead, 0 )
                      .Storage( P::WaterFFTResultB, colB, RDG::Access::StorageRead, 0 )
                      .Storage( P::WaterFFTWaves, waves, RDG::Access::StorageWrite, 0 )
                      .Storage( P::WaterFFTParamsBuffer, rowsBuffer, RDG::Access::StorageRead )
                      .Storage( P::WaterFFTReadbackBuffer, readbackBuffer, RDG::Access::StorageWrite );
             },
             [pipeline = m_FFTFinalize]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, kFFTGroups8, kFFTGroups8,
                                                                 Water::kFFTOceanCascades );
             } );
        if ( readback )
        {
            graph.Extract( readbackBuffer, m_FFTReadback[slot].Buffer, RDG::Access::HostRead );
            m_FFTReadback[slot].Spectrum = m_FFTFrame;
            m_FFTReadback[slot].Time     = static_cast<double>( timeSeconds );
        }
        m_FFTWrittenHalf    = m_FFTHalf;
        m_FrameWavesTexture = waves;
        m_FrameWavesGraph   = &graph;
        return waves;
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
        std::vector<std::byte>&       push     = m_Push;
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
