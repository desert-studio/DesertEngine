#include "WaterRenderer.hpp"

#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ViewTargetLayouts.hpp>
#include <Engine/Graphic/Materials/Properties/StorageBufferProperty.hpp>
#include <Engine/Graphic/Materials/Properties/UniformBufferProperty.hpp>
#include <Engine/Graphic/PipelineCache.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Core/Frustum.hpp>
#include <Common/Core/Profiler.hpp>

#include <cstddef>
#include <string>

namespace Desert::Graphic::System
{
    namespace
    {
        // Matches WaterUB (binding 0) in Programs/Water/WaterSurfaceData.glslh: frame data only.
        struct WaterUB
        {
            glm::mat4 ViewProjection;
            glm::mat4 PrevViewProjection;
            glm::vec4 CameraPos;
            glm::vec4 SunDir;
            glm::vec4 SunColor;
        };

        // Matches WaterTileRow (WaterTiles, binding 1).
        struct WaterTileRow
        {
            glm::vec4 Tile;
            glm::vec4 Lod;
            glm::vec4 Zone;
        };
        static_assert( sizeof( WaterTileRow ) == 3 * sizeof( glm::vec4 ), "std430 rows of three vec4" );

        // The culling box's padding above and below the still plane. The surface is flat (WATER-W6); WATER-W7's
        // waves widen it by the bodies' MaxWaveHeight.
        constexpr float kFlatSurfaceVerticalExtentCm = 1.0f;
    } // namespace

    Common::BoolResultStr WaterRenderer::Initialize()
    {
        const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "WaterSurface" );
        if ( !shader )
            return Common::MakeError( "WaterRenderer: missing shader 'WaterSurface'" );

        GraphicsPipelineSpecification spec;
        spec.DebugName    = "WaterSurfacePipeline";
        spec.Shader       = shader;
        spec.TargetLayout = SceneTargetLayout();
        spec.BlendEnable  = false;
        ApplyShaderRenderState( spec, shader->GetProgramMeta().State );

        const auto pipeline = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        if ( !pipeline )
            return Common::MakeError( "WaterRenderer: " + pipeline.GetError() );
        m_Pipeline = pipeline.GetValue();
        m_Material = std::make_unique<DataDrivenMaterial>( "WaterSurface" );
        return BOOLSUCCESS;
    }

    void WaterRenderer::PrepareFrame()
    {
        m_FrameDraws.clear();
        const auto* camera = m_SceneRenderer->GetMainCamera();
        if ( camera == nullptr || m_Queue.empty() || !m_Pipeline )
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

        WaterUB     ub{};
        const auto* viewFrame = m_SceneRenderer->GetViewFrame();
        ub.ViewProjection     = viewFrame->ViewProjection;
        ub.PrevViewProjection = viewFrame->PrevViewProjection;
        ub.CameraPos          = glm::vec4( observer, 1.0f );
        ub.SunDir             = glm::vec4( glm::normalize( glm::vec3( -0.4f, -0.85f, -0.35f ) ), 0.0f );
        ub.SunColor           = glm::vec4( 1.0f, 0.98f, 0.92f, 3.0f );
        if ( const auto& lights = m_SceneRenderer->GetDirectionLights().DirectionLights; !lights.empty() )
        {
            ub.SunDir   = lights[0].Direction;
            ub.SunColor = lights[0].ColorIntensity;
        }

        // Uploaded up front at final size, before any draw records (the terrain's reason: growing a storage buffer
        // reallocates it under a draw already recorded against the old one).
        if ( auto* block = m_Material->Get<UniformBufferProperty>( "WaterUB" ) )
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): a uniform block uploaded as bytes
            block->SetRawData( reinterpret_cast<const std::byte*>( &ub ), sizeof( ub ) );
        if ( auto* tiles = m_Material->Get<StorageBufferProperty>( "WaterTiles" ) )
            tiles->SetRawData( rows.data(), static_cast<uint32_t>( rows.size() * sizeof( WaterTileRow ) ) );
    }

    void WaterRenderer::DeclareDraws( RDG::PassBuilder& pass, const SceneViewInputs& view )
    {
        // Exactly when RenderManual draws.
        if ( m_FrameDraws.empty() || !m_Pipeline )
            return;
        const auto& layout = m_Layout.Get( m_Pipeline->GetSpecification().Shader );
        auto        block  = pass.Bindings( layout, m_Material->GetMaterialExecutor()->GetRouteFill() );
        // The surface fogs itself at its own depth (Mesh/Surface/SceneFog.glslh): the frame's fog block and
        // images.
        BindSceneFogInputs( block, view, *layout );
    }

    Common::BoolResultStr WaterRenderer::RenderManual( const RDG::PassContext& context )
    {
        const auto* camera = m_SceneRenderer->GetMainCamera();
        if ( m_FrameDraws.empty() || !m_Pipeline || camera == nullptr )
            return BOOLSUCCESS;
        DESERT_PROFILE_PASS( "Water: Surface" );

        // The row base and the matrix are push constants, snapshotted per draw at record.
        const RDG::PassBindings block( context, context.GetBindingBlock( 0 ) );
        m_Material->SetPushMatrix( camera->GetProjectionMatrix() * camera->GetViewMatrix() );
        for ( const FrameDraw& draw : m_FrameDraws )
        {
            m_Material->SetMaterialIndex( draw.FirstRow );
            if ( auto drawn = Renderer::GetInstance().DrawProcedural(
                      block, *m_Pipeline, m_Material->GetMaterialExecutor(),
                      draw.QuadsPerSide * draw.QuadsPerSide * 6u, draw.RowCount );
                 !drawn.IsSuccess() )
                return drawn;
        }
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::System
