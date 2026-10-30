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
#include <Engine/Runtime/Services/Texture/TextureService.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>
#include <Engine/Graphic/Materials/Properties/Texture2DProperty.hpp>
#include <Engine/Graphic/Texture.hpp>
#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/Core/Frustum.hpp>
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

namespace
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct WaterShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, WaterTiles );
        DESERT_SHADER_PARAMETER( StorageBuffer, Materials ); // Core::Formats::kMaterialRowBlockName
    };
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

    void WaterRenderer::WarnOnce( const Assets::AssetHandle& material, const std::string& why )
    {
        if ( m_Warned.insert( static_cast<uint64_t>( material ) ).second )
            LOG_WARN( "[Water] water material {}: {}", static_cast<uint64_t>( material ), why );
    }

    WaterRenderer::WaterProgram* WaterRenderer::ProgramFor( const Assets::AssetHandle& material )
    {
        auto* materials = Runtime::ResourceRegistry::GetMaterialService();
        if ( materials == nullptr )
            return nullptr;
        const auto templ = materials->ShaderHandleOf( material );
        if ( static_cast<uint64_t>( templ.Shader ) == 0 || templ.CompileName.empty() )
        {
            WarnOnce( material, "names no material with a loaded template - the body draws no water" );
            return nullptr;
        }
        if ( auto it = m_Programs.find( templ.CompileName ); it != m_Programs.end() )
            return it->second.Pipeline ? &it->second : nullptr;

        WaterProgram&     program  = m_Programs[templ.CompileName]; // an empty entry remembers a refusal
        const std::string cellName =
             std::format( "{}/{}", templ.CompileName, Core::Preprocess::kSurfaceWaterMeshCell );
        const auto        shader   = Runtime::ResourceRegistry::GetShaderService()->GetByName( cellName );
        if ( !shader )
        {
            WarnOnce( material, std::format( "its template '{}' has no {} cell - a water body draws a template of "
                                             "ShadingModel SingleLayerWater with Usage WaterMesh",
                                             templ.CompileName, Core::Preprocess::kSurfaceWaterMeshCell ) );
            return nullptr;
        }

        if ( auto built = BuildProgram( shader, "WaterSurfacePipeline:" + templ.CompileName, program ); !built )
        {
            WarnOnce( material,
                      std::format( "its template '{}' does not build: {}", templ.CompileName, built.GetError() ) );
            return nullptr;
        }
        program.Material = std::make_unique<DataDrivenMaterial>( cellName );
        return &program;
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
            WarnOnce( material,
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
            WarnOnce( material,
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

    void WaterRenderer::ApplyMaterial( const Assets::AssetHandle& material, WaterProgram& program )
    {
        // The body's `.demat` folded over the template's schema into the cell's material row (UE: the material's
        // uniform expressions), its textures into the bindless slots the row names.
        DataDrivenMaterial& cell = *program.Material;
        cell.ApplyDefaults();
        MaterialOverrides overrides;
        if ( auto* materials = Runtime::ResourceRegistry::GetMaterialService() )
            (void)materials->ResolveOverrides( material, overrides );
        for ( const auto& [name, value] : overrides.Params )
            (void)cell.SetParam( name, value );
        for ( const auto& [name, handle] : overrides.Textures )
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
            for ( size_t b = 0; b < zone.BodyMaterials.size(); ++b )
            {
                const Assets::AssetHandle& body = zone.BodyMaterials[b];
                if ( static_cast<uint64_t>( body ) == 0 )
                    continue;
                if ( static_cast<uint64_t>( material ) == 0 )
                {
                    material   = body;
                    wavesAsset = zone.BodyWaves[b];
                    waveOrigin = zone.BodyWaveOrigins[b];
                    waveMaskDepth = zone.BodyWaveMaskDepths[b];
                }
                else if ( body != material )
                    WarnOnce( body, "differs from another body's in the same frame - drawn with the first one" );
            }
        if ( static_cast<uint64_t>( material ) == 0 )
            return; // no body names a look: no water (WaterBodyData::WaterMaterial)
        WaterProgram* program = ProgramFor( material );
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
                WarnOnce( material, "its body's waves cannot be read: no WaterWavesService" );
            else if ( !got )
                WarnOnce( material, std::format( "its body's waves are refused: {}", got.GetError() ) );
            else if ( got.GetValue() )
            {
                const std::vector<Water::GerstnerWave>& set = *got.GetValue();
                if ( set.size() > kMaxWaves )
                    WarnOnce( material, std::format( "its body's wave set has {} waves - the water mesh draws the "
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

        // Every zone's tiles in its density order: one instanced draw per run of one grid density (a row carries
        // its own zone, so a run may span two zones).
        std::vector<WaterTileRow> rows;
        for ( const WaterZoneDraw& zone : m_Queue )
        {
            const Water::WaterQuadTree                  tree( zone.Zone, zone.Bodies );
            const std::vector<Water::WaterTileInstance> tiles =
                 tree.Traverse( observer, &frustum, verticalExtent );
            const glm::vec2 origin = tree.GetRootMin();
            for ( const Water::WaterTileInstance& tile : tiles )
            {
                const uint32_t quads = tree.QuadsPerSide( tile.DensityIndex );
                if ( m_FrameDraws.empty() || m_FrameDraws.back().QuadsPerSide != quads )
                {
                    m_FrameDraws.push_back( { .FirstRow     = static_cast<uint32_t>( rows.size() ),
                                              .RowCount     = 0,
                                              .QuadsPerSide = quads } );
                }
                ++m_FrameDraws.back().RowCount;

                WaterTileRow row;
                row.Tile = glm::vec4( tile.Corner.x, tile.Corner.y, tile.SurfaceY, tile.Size );
                row.Lod  = glm::vec4( static_cast<float>( tile.LODLevel ), tile.ShouldMorph ? 1.0f : 0.0f,
                                     tile.CanMorphTwice ? 1.0f : 0.0f, tile.HeightMorph );
                row.Zone = glm::vec4( origin.x, origin.y, tree.GetLODScale(), static_cast<float>( quads ) );
                rows.push_back( row );
            }
        }
        if ( rows.empty() )
            return;

        ApplyMaterial( material, *program );

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
        if ( auto* tiles = program->Material->Get<StorageBufferProperty>( WaterShaderParameters::WaterTiles ) )
        {
            std::vector<std::byte> bytes( sizeof( WaterWavesBlock ) + rows.size() * sizeof( WaterTileRow ) );
            std::memcpy( bytes.data(), &waves, sizeof( WaterWavesBlock ) );
            std::memcpy( bytes.data() + sizeof( WaterWavesBlock ), rows.data(),
                         rows.size() * sizeof( WaterTileRow ) );
            tiles->SetRawData( bytes.data(), static_cast<uint32_t>( bytes.size() ) );
        }
        m_Frame = program;
    }

    void WaterRenderer::DeclareDraws( RDG::PassBuilder& pass, const RDG::TextureRef sceneCopy,
                                      const RDG::TextureRef waterInfo, const SceneViewInputs& view,
                                      const SceneFrameBinding& frame )
    {
        // Exactly when RenderManual draws.
        if ( m_FrameDraws.empty() || m_Frame == nullptr || !sceneCopy.IsValid() || !waterInfo.IsValid() )
            return;
        // The view's camera, sun, sky cubes and sky look, by block name - as every surface cell gets them.
        frame.ApplyTo( m_Frame->Material.get() );
        const auto layout = m_Layout.Get( m_Frame->Pipeline->GetSpecification().Shader );
        auto       block  = pass.Bindings( layout, m_Frame->Material->GetMaterialExecutor()->GetRouteFill() );
        BindSceneViewInputs( block, view, *layout );
        constexpr RDG::TextureParameter kSceneColor{ "SceneColorTexture" };
        constexpr RDG::SamplerParameter kClamp{ "LinearClampSampler" };
        // The WaterInfo ground (UE: the water info texture a view's water pass reads): a pass input like the scene
        // copy, never a material property.
        constexpr RDG::TextureParameter kWaterInfo{ "WaterInfoTexture" };
        block.Texture( kSceneColor, sceneCopy, RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
             .Texture( kWaterInfo, waterInfo, RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
             .Sampler( kClamp, RDG::SamplerDesc::LinearClamp() );
    }

    Common::BoolResultStr WaterRenderer::RenderManual( const RDG::PassContext& context )
    {
        const auto* camera = m_SceneRenderer->GetMainCamera();
        if ( m_FrameDraws.empty() || m_Frame == nullptr || camera == nullptr )
            return BOOLSUCCESS;
        DataDrivenMaterial& material = *m_Frame->Material;
        DESERT_PROFILE_PASS( "Water: Surface" );

        // The row base and the matrix are push constants, snapshotted per draw at record.
        // Push constants, snapshotted per draw at record: the material row (the cell's one row) and the draw's
        // first tile row (PrimitiveIndex: the water has no GpuScene primitive; Vertex_WaterMesh.glslh).
        const RDG::PassBindings block( context, context.GetBindingBlock( 0 ) );
        material.SetPushMatrix( glm::mat4( 1.0f ) );
        material.SetMaterialIndex( 0u );
        for ( const FrameDraw& draw : m_FrameDraws )
        {
            material.SetPrimitiveIndex( draw.FirstRow );
            if ( auto drawn = Renderer::GetInstance().DrawProcedural(
                      block, *m_Frame->Pipeline, material.GetMaterialExecutor(),
                      draw.QuadsPerSide * draw.QuadsPerSide * 6u, draw.RowCount );
                 !drawn.IsSuccess() )
                return drawn;
        }
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::System
