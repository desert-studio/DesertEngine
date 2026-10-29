#include "WaterRenderer.hpp"

#include <Engine/Graphic/SceneRenderer.hpp>
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
#include <Engine/Core/Frustum.hpp>
#include <Common/Core/Profiler.hpp>

#include <cmath>
#include <cstddef>
#include <format>
#include <algorithm>
#include <array>
#include <string>
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

        // The still surface's vertical extent the quadtree culls with (no GPU waves yet: REMAINDER).
        constexpr float kFlatSurfaceVerticalExtentCm = 1.0f;

        // The cell a water material draws with (DShaderParser kSurfaceWaterMeshPath x
        // kSurfaceSingleLayerWaterPass).
        constexpr const char* kWaterCell = "WaterMesh.SingleLayerWater";
    } // namespace

    Common::BoolResultStr WaterRenderer::Initialize()
    {
        // The engine's program must build: a material naming it is the common case and its failure is loud here.
        const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName(
             std::format( "WaterSurface/{}", kWaterCell ) );
        if ( !shader )
            return Common::MakeError(
                 std::format( "WaterRenderer: missing shader 'WaterSurface/{}'", kWaterCell ) );
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
        const std::string cellName = std::format( "{}/{}", templ.CompileName, kWaterCell );
        const auto        shader   = Runtime::ResourceRegistry::GetShaderService()->GetByName( cellName );
        if ( !shader )
        {
            WarnOnce( material, std::format( "its template '{}' has no {} cell - a water body draws a template of "
                                             "ShadingModel SingleLayerWater with Usage WaterMesh",
                                             templ.CompileName, kWaterCell ) );
            return nullptr;
        }

        GraphicsPipelineSpecification spec;
        spec.DebugName    = "WaterSurfacePipeline:" + templ.CompileName;
        spec.Shader       = shader;
        spec.TargetLayout = SceneTargetLayout();
        spec.BlendEnable  = false;
        ApplyShaderRenderState( spec, shader->GetProgramMeta().State );
        const auto pipeline = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        if ( !pipeline )
        {
            WarnOnce( material, std::format( "its template '{}' does not build: {}", templ.CompileName,
                                             pipeline.GetError() ) );
            return nullptr;
        }
        program.Pipeline = pipeline.GetValue();
        program.Material = std::make_unique<DataDrivenMaterial>( cellName );
        return &program;
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
        Assets::AssetHandle material = Assets::AssetHandle::Null();
        for ( const WaterZoneDraw& zone : m_Queue )
            for ( const Assets::AssetHandle& body : zone.BodyMaterials )
            {
                if ( static_cast<uint64_t>( body ) == 0 )
                    continue;
                if ( static_cast<uint64_t>( material ) == 0 )
                    material = body;
                else if ( body != material )
                    WarnOnce( body, "differs from another body's in the same frame - drawn with the first one" );
            }
        if ( static_cast<uint64_t>( material ) == 0 )
            return; // no body names a look: no water (WaterBodyData::WaterMaterial)
        WaterProgram* program = ProgramFor( material );
        if ( program == nullptr )
            return;

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
                 tree.Traverse( observer, &frustum, kFlatSurfaceVerticalExtentCm );
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
        if ( auto* tiles = program->Material->Get<StorageBufferProperty>( WaterShaderParameters::WaterTiles ) )
            tiles->SetRawData( rows.data(), static_cast<uint32_t>( rows.size() * sizeof( WaterTileRow ) ) );
        m_Frame = program;
    }

    void WaterRenderer::DeclareDraws( RDG::PassBuilder& pass, const RDG::TextureRef sceneCopy,
                                      const SceneViewInputs& view, const SceneFrameBinding& frame )
    {
        // Exactly when RenderManual draws.
        if ( m_FrameDraws.empty() || m_Frame == nullptr || !sceneCopy.IsValid() )
            return;
        // The view's camera, sun, sky cubes and sky look, by block name - as every surface cell gets them.
        frame.ApplyTo( m_Frame->Material.get() );
        const auto layout = m_Layout.Get( m_Frame->Pipeline->GetSpecification().Shader );
        auto       block  = pass.Bindings( layout, m_Frame->Material->GetMaterialExecutor()->GetRouteFill() );
        BindSceneViewInputs( block, view, *layout );
        constexpr RDG::TextureParameter kSceneColor{ "SceneColorTexture" };
        constexpr RDG::SamplerParameter kClamp{ "LinearClampSampler" };
        block.Texture( kSceneColor, sceneCopy, RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
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
