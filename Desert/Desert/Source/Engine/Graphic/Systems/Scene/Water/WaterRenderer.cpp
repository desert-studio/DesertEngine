#include "WaterRenderer.hpp"

#include <Engine/Graphic/SceneRenderer.hpp>
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
#include <Engine/Graphic/Materials/Properties/Texture2DProperty.hpp>
#include <Engine/Graphic/Texture.hpp>
#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/Core/Frustum.hpp>
#include <Engine/Graphic/VisibilityCulling.hpp>
#include <Engine/Water/GerstnerWaterWaves.hpp>
#include <Common/Core/Profiler.hpp>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{
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
        };
        static_assert( sizeof( WaterTileRow ) == 3 * sizeof( glm::vec4 ), "std430 rows of three vec4" );

        // One Gerstner wave, std430 (Mesh/Surface/WaterMeshTiles.glslh WaterWaveRow).
        struct WaterWaveRow
        {
            glm::vec4 Wave; // xy = direction, z = wavelength (cm), w = amplitude (cm)
            glm::vec4 Form; // x = steepness
        };
        static_assert( sizeof( WaterWaveRow ) == 2 * sizeof( glm::vec4 ), "std430 rows of two vec4" );

        // WATER_MAX_WAVES (WaterMeshTiles.glslh): the waves lead the tile buffer.
        constexpr uint32_t kMaxWaves = 32;
        struct WaterWavesBlock
        {
            glm::vec4                           Header{ 0.0f }; // x = count, y unused, zw = WaveOrigin
            glm::vec4 Info{ 0.0f }; // WaterInfoTexture: xy = lower (X, Z), z = 1 / side, w = mask depth
            std::array<WaterWaveRow, kMaxWaves> Rows{};
        };
        static_assert( sizeof( WaterWavesBlock ) == ( 2 + 2 * kMaxWaves ) * sizeof( glm::vec4 ) );

        // The WaterInfo ground as the GPU samples it: one R32F texel per baked height (Water::WaterInfoGround).
        std::shared_ptr<Image2D> CreateWaterInfoImage( const uint32_t size, const std::vector<float>& heights )
        {
            std::vector<unsigned char> bytes( heights.size() * sizeof( float ) );
            std::memcpy( bytes.data(), heights.data(), bytes.size() );
            Core::Formats::Image2DSpecification spec = {
                 .Tag        = "WaterInfoGround",
                 .Width      = size,
                 .Height     = size,
                 .Format     = Core::Formats::ImageFormat::R32F,
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

        // The still surface's vertical extent the quadtree culls with when the body has no waves.
        constexpr float kFlatSurfaceVerticalExtentCm = 1.0f;

    } // namespace

    Common::BoolResultStr WaterRenderer::Initialize()
    {
        // The engine's program must build: a material naming it is the common case and its failure is loud here.
        const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName(
             std::format( "WaterSurface/{}", Core::Preprocess::kSurfaceWaterMeshCell ) );
        if ( !shader )
            return Common::MakeError( std::format( "WaterRenderer: missing shader 'WaterSurface/{}'",
                                                   Core::Preprocess::kSurfaceWaterMeshCell ) );
        return BOOLSUCCESS;
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
        if ( m_Resolved.Program != nullptr && m_Resolved.Material == static_cast<uint64_t>( material ) &&
             m_Resolved.Stamp == stamp )
            return m_Resolved.Program;
        m_Resolved      = ResolvedProgram{};
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
            m_Resolved = ResolvedProgram{ static_cast<uint64_t>( material ), stamp, &it->second };
            return &it->second;
        }

        WaterProgram&     program  = m_Programs[templ.CompileName]; // an empty entry remembers a refusal
        const std::string cellName =
             std::format( "{}/{}", templ.CompileName, Core::Preprocess::kSurfaceWaterMeshCell );
        const auto        shader   = Runtime::ResourceRegistry::GetShaderService()->GetByName( cellName );
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
        m_Resolved          = ResolvedProgram{ static_cast<uint64_t>( material ), stamp, &program };
        return &program;
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
        return !m_FrameDraws.empty() && m_Frame != nullptr && m_Frame->Pipeline &&
               m_Frame->Pipeline->GetSpecification().Shader->SamplesTexture( textureName );
    }

    void WaterRenderer::ApplyMaterial( const Assets::AssetHandle& material, const MaterialStamp& stamp,
                                       WaterProgram& program )
    {
        // The body's `.demat` folded over the template's schema into the cell's material row (UE: the material's
        // uniform expressions, cached by the render proxy until the material changes), its textures into the
        // bindless slots the row names. Re-resolved only when another material is drawn with this program or a
        // stamp moved; the row is written every frame (each frame and view has its own copy of the block).
        DataDrivenMaterial& cell   = *program.Material;
        FoldedValues&       folded = program.Folded;
        if ( folded.Material != static_cast<uint64_t>( material ) || folded.Stamp != stamp )
        {
            folded.Material = static_cast<uint64_t>( material );
            folded.Stamp    = stamp;
            folded.Overrides.Params.clear();
            folded.Overrides.Textures.clear();
            if ( auto* materials = Runtime::ResourceRegistry::GetMaterialService() )
                (void)materials->ResolveOverrides( material, folded.Overrides );
            cell.ApplyDefaults();
            for ( const auto& [name, value] : folded.Overrides.Params )
                (void)cell.SetParam( name, value );
        }
        for ( const auto& [name, handle] : folded.Overrides.Textures )
        {
            if ( handle == 0 )
                continue;
            auto* texture = Runtime::ResourceRegistry::GetTextureService()->Get( Common::UUID( handle ) );
            if ( texture == nullptr )
                continue; // still loading: picked up the frame it lands
            const auto* image = dynamic_cast<Image2D*>(
                 Runtime::ResourceRegistry::GetImageService()->Resolve( texture->GetImageHandle() ) );
            if ( image != nullptr )
                (void)cell.SetTexture( name, image );
        }
        const Core::Formats::MaterialParamRow& row = cell.GetParamRow();
        if ( auto* rows = cell.Get<StorageBufferProperty>( WaterShaderParameters::Materials ) )
            rows->SetRawData( row.data(), static_cast<uint32_t>( row.size() * sizeof( glm::vec4 ) ) );
    }

    void WaterRenderer::PrepareFrame()
    {
        m_FrameDraws.clear();
        const auto* camera = m_SceneRenderer->GetMainCamera();
        m_Frame            = nullptr;
        m_FrameFrontLayer  = nullptr;
        if ( m_Queue.empty() )
            m_ZoneTrees.clear(); // no water this frame: the retained trees (and their body sets) go
        if ( camera == nullptr || m_Queue.empty() )
            return;

        // THE FRAME'S WATER MATERIAL: the first body's that names one. Bodies of one frame naming different
        // materials draw with the first (REMAINDER WATER: a per-tile body index, UE's WaterBodyIndex).
        // Its waves are that body's (the same per-tile body index in REMAINDER).
        Assets::AssetHandle material   = Assets::AssetHandle::Null();
        Assets::AssetHandle wavesAsset = Assets::AssetHandle::Null();
        glm::vec2           waveOrigin( 0.0f );
        float                                         waveMaskDepth = 0.0f;
        std::shared_ptr<const Water::WaterInfoGround> ground;
        for ( const WaterZoneDraw& zone : m_Queue )
            if ( ground == nullptr )
                ground = zone.Ground;
        for ( const WaterZoneDraw& zone : m_Queue )
            for ( size_t b = 0; b < zone.Bodies->Materials.size(); ++b )
            {
                const Assets::AssetHandle& body = zone.Bodies->Materials[b];
                if ( static_cast<uint64_t>( body ) == 0 )
                    continue;
                if ( static_cast<uint64_t>( material ) == 0 )
                {
                    material   = body;
                    wavesAsset = zone.Bodies->Waves[b];
                    waveOrigin = zone.Bodies->WaveOrigins[b];
                    waveMaskDepth = zone.Bodies->WaveMaskDepths[b];
                }
                else if ( body != material )
                    WarnOnce( body, WaterWarning::MixedMaterials,
                              "differs from another body's in the same frame - drawn with the first one" );
            }
        if ( static_cast<uint64_t>( material ) == 0 )
            return; // no body names a look: no water (WaterBodyData::WaterMaterial)
        MaterialStamp stamp;
        if ( auto* const materials = Runtime::ResourceRegistry::GetMaterialService() )
        {
            stamp.Service             = materials;
            stamp.InvalidationVersion = materials->GetInvalidationVersion();
            stamp.ValuesEditSerial    = materials->GetValuesEditSerial();
        }
        stamp.ContentSerial   = Assets::ContentRegistry::WriteSerial();
        WaterProgram* program = ProgramFor( material, stamp );
        // The template's front layer cell, of the base program (it reads no local shadow: no VSM twin).
        WaterProgram* const frontLayer = program != nullptr ? FrontLayerProgramFor( material, *program ) : nullptr;
        // Under Shadows.Method VirtualMaps no atlas is drawn: the cell's virtual-map permutation draws, as every
        // forward local-shadow reader's does (SceneRenderer::LocalShadowsAreVirtual, settled by BeginScene).
        if ( program != nullptr && m_SceneRenderer->LocalShadowsAreVirtual() )
            program = VirtualLocalProgramFor( material, *program );
        if ( program == nullptr )
            return;

        // THE BODY'S WAVES (UE: the water body's wave buffer the water vertex factory reads): the generated set,
        // once per asset (Runtime::WaterWavesService), packed ahead of the tiles. Still loading = flat this frame.
        WaterWavesBlock waves;
        waves.Header.z       = waveOrigin.x;
        waves.Header.w       = waveOrigin.y;
        float verticalExtent = kFlatSurfaceVerticalExtentCm;
        if ( static_cast<uint64_t>( wavesAsset ) != 0 )
        {
            auto* service = Runtime::ResourceRegistry::GetWaterWavesService();
            Common::ResultStr<Runtime::WaterWavesService::Waves> got =
                 Common::MakeSuccess( Runtime::WaterWavesService::Waves{} );
            if ( service != nullptr )
                got = service->Get( wavesAsset );
            if ( service == nullptr )
                WarnOnce( material, WaterWarning::NoWavesService,
                          "its body's waves cannot be read: no WaterWavesService" );
            else if ( !got )
            {
                if ( FirstWarning( material, WaterWarning::WavesRefused ) )
                    Warn( material, std::format( "its body's waves are refused: {}", got.GetError() ) );
            }
            else if ( got.GetValue() )
            {
                const std::vector<Water::GerstnerWave>& set = *got.GetValue();
                if ( set.size() > kMaxWaves && FirstWarning( material, WaterWarning::TooManyWaves ) )
                    Warn( material, std::format( "its body's wave set has {} waves - the water mesh draws the "
                                                 "first {} (WATER_MAX_WAVES)",
                                                 set.size(), kMaxWaves ) );
                const size_t count = std::min<size_t>( set.size(), kMaxWaves );
                for ( size_t k = 0; k < count; ++k )
                {
                    const Water::GerstnerWave& wave = set[k];
                    waves.Rows[k].Wave              = glm::vec4( wave.Direction, wave.WaveLength, wave.Amplitude );
                    waves.Rows[k].Form              = glm::vec4( wave.Steepness, 0.0f, 0.0f, 0.0f );
                }
                waves.Header.x = static_cast<float>( count );
                verticalExtent = std::max( verticalExtent, Water::MaxWaveHeight( set ) );
            }
        }

        const glm::mat4     view     = camera->GetViewMatrix();
        const glm::mat4     proj     = camera->GetProjectionMatrix();
        const glm::vec3     observer = glm::vec3( glm::inverse( view )[3] );
        const Core::Frustum frustum( proj, view );
        // The lens stretches every water LOD ring (VIEW-LOD-FOV): the one view scale meshes, foliage and terrain
        // read, so a 10-degree scope keeps the far sea as fine as it looks.
        const float lensScale = DrawDistanceScale( Geometry::LODView{ .Origin = observer, .Projection = proj } );

        // Every zone's tiles in its density order: one instanced draw per run of one grid density (a row carries
        // its own zone, so a run may span two zones). The rows go straight into the upload, after its waves block
        // (written last: its WaterInfo lane needs the ground).
        m_Upload.resize( sizeof( WaterWavesBlock ) );
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
            for ( const Water::WaterTileInstance& tile : tiles )
            {
                const uint32_t quads = tree.QuadsPerSide( tile.DensityIndex );
                if ( m_FrameDraws.empty() || m_FrameDraws.back().QuadsPerSide != quads )
                {
                    m_FrameDraws.push_back( { .FirstRow = rowCount, .RowCount = 0, .QuadsPerSide = quads } );
                }
                ++m_FrameDraws.back().RowCount;

                WaterTileRow row;
                row.Tile = glm::vec4( tile.Corner.x, tile.Corner.y, tile.SurfaceY, tile.Size );
                row.Lod  = glm::vec4( static_cast<float>( tile.LODLevel ), tile.ShouldMorph ? 1.0f : 0.0f,
                                     tile.CanMorphTwice ? 1.0f : 0.0f, tile.HeightMorph );
                row.Zone = glm::vec4( origin.x, origin.y, viewLODScale, static_cast<float>( quads ) );
                const size_t at = m_Upload.size();
                m_Upload.resize( at + sizeof( WaterTileRow ) );
                std::memcpy( m_Upload.data() + at, &row, sizeof( WaterTileRow ) );
                ++rowCount;
            }
        }
        if ( rowCount == 0u )
            return;

        ApplyMaterial( material, stamp, *program );

        // THE WATER INFO GROUND (UE WaterInfo): re-uploaded only when the collector baked a new one. Over no
        // landscape the texture is one texel of kWaterInfoNoGroundCm - a bottom no wave reaches.
        if ( ground != m_GroundBaked || m_WaterInfo == nullptr )
        {
            m_GroundBaked = ground;
            m_WaterInfo   = ground != nullptr && ground->Size > 0u
                                 ? CreateWaterInfoImage( ground->Size, ground->HeightCm )
                                 : CreateWaterInfoImage( 1u, { Water::kWaterInfoNoGroundCm } );
        }
        if ( ground != nullptr && ground->ExtentCm > 0.0f )
            waves.Info = glm::vec4( ground->Min, 1.0f / ground->ExtentCm, std::max( waveMaskDepth, 1.0f ) );
        std::memcpy( m_Upload.data(), &waves, sizeof( WaterWavesBlock ) );
        if ( auto* tiles = program->Material->Get<StorageBufferProperty>( WaterShaderParameters::WaterTiles ) )
            tiles->SetRawData( m_Upload.data(), static_cast<uint32_t>( m_Upload.size() ) );
        m_Frame = program;

        // The front layer draws the same surface: the same `.demat` folded into its own row and the same upload.
        if ( frontLayer != nullptr )
        {
            ApplyMaterial( material, stamp, *frontLayer );
            if ( auto* tiles =
                      frontLayer->Material->Get<StorageBufferProperty>( WaterShaderParameters::WaterTiles ) )
                tiles->SetRawData( m_Upload.data(), static_cast<uint32_t>( m_Upload.size() ) );
            m_FrameFrontLayer = frontLayer;
        }
    }

    void WaterRenderer::DeclareDraws( RDG::PassBuilder& pass, const RDG::TextureRef sceneCopy,
                                      const RDG::TextureRef waterInfo, const RDG::TextureRef frontReflection,
                                      const RDG::TextureRef frontDepth, const SceneViewInputs& view,
                                      const SceneFrameBinding& frame )
    {
        // Exactly when RenderManual draws.
        if ( m_FrameDraws.empty() || m_Frame == nullptr || !sceneCopy.IsValid() || !waterInfo.IsValid() )
            return;
        // The view's camera, sun, sky cubes and sky look, by block name - as every surface cell gets them.
        frame.ApplyTo( m_Frame->Material.get() );
        // RENDER-THREADS M1: the one material RenderManual applies - its view sets and block copies exist before
        // the graph records (after the frame binding above, which writes its blocks).
        pass.Prepares( *m_Frame->Material->GetMaterialExecutor() );
        const SlottedLayout& slotted = m_Layout.GetSlotted( m_Frame->Pipeline->GetSpecification().Shader );
        const auto&          layout  = slotted.Layout;
        auto block = pass.Bindings( layout, m_Frame->Material->GetMaterialExecutor()->GetRouteFill() );
        BindSceneViewInputs( block, view, slotted.Slots );
        // The WaterInfo ground (UE: the water info texture a view's water pass reads): a pass input like the scene
        // copy, never a material property.
        block.Texture( WaterShaderParameters::SceneColorTexture, sceneCopy, RDG::Access::SampledGraphics,
                       RDG::SubresourceRange::All() )
             .Texture( WaterShaderParameters::WaterInfoTexture, waterInfo, RDG::Access::SampledGraphics,
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
                                                const SceneViewInputs& view, const SceneFrameBinding& frame )
    {
        // Exactly when RenderFrontLayerManual draws.
        if ( !HasFrontLayerDraws() || !waterInfo.IsValid() )
            return;
        WaterProgram& front = *m_FrameFrontLayer;
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
                       RDG::SubresourceRange::All() );
    }

    Common::BoolResultStr WaterRenderer::RenderManual( const RDG::PassContext& context )
    {
        if ( m_FrameDraws.empty() || m_Frame == nullptr )
            return BOOLSUCCESS;
        DESERT_PROFILE_PASS( "Water: Surface" );
        return RecordDraws( context, *m_Frame );
    }

    Common::BoolResultStr WaterRenderer::RenderFrontLayerManual( const RDG::PassContext& context )
    {
        // The graph opened the render pass on the front layer's targets (after the mesh front layer, or cleared).
        if ( !HasFrontLayerDraws() )
            return BOOLSUCCESS;
        DESERT_PROFILE_PASS( "Water: Front Layer" );
        return RecordDraws( context, *m_FrameFrontLayer );
    }

    Common::BoolResultStr WaterRenderer::RecordDraws( const RDG::PassContext& context,
                                                      const WaterProgram&     program )
    {
        const auto* camera = m_SceneRenderer->GetMainCamera();
        if ( camera == nullptr )
            return BOOLSUCCESS;
        const DataDrivenMaterial& material = *program.Material;

        // RENDER-THREADS M3: the push values travel BY VALUE, the material is only read (DrawPushFields, as
        // TerrainRenderer::RecordDraws): each draw's block is a copy of the material's with the identity matrix,
        // the material row (the cell's one row) and the draw's first tile row over it (PrimitiveIndex: the water
        // has no GpuScene primitive; Vertex_WaterMesh.glslh).
        const RDG::PassBindings       block( context, context.GetBindingBlock( 0 ) );
        const MaterialExecutor*       executor = material.GetMaterialExecutor();
        const Common::Memory::Buffer& pushed   = executor->GetPushConstantBuffer();
        std::vector<std::byte>&       push     = m_Push;
        for ( const FrameDraw& draw : m_FrameDraws )
        {
            push.assign( pushed.Size, std::byte{ 0 } );
            if ( pushed.Data != nullptr && pushed.Size != 0u )
                std::memcpy( push.data(), pushed.Data, pushed.Size );
            DrawPushFields fields;
            fields.Transform( glm::mat4( 1.0f ) ).MaterialIndex( 0u ).PrimitiveIndex( draw.FirstRow );
            if ( auto written = fields.WriteInto( push, material.GetMaterialLayout() ); !written )
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
