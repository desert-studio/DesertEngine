#include "EditorColliderPass.hpp"

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/SceneRenderer.hpp> // the view's own debug/show state
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/gtc/quaternion.hpp>

namespace Desert::Editor::Render
{
    namespace
    {
        // UE-style collider green.
        const glm::vec4 kColliderColor{ 0.27f, 0.9f, 0.35f, 1.0f };

        constexpr int kCircleSegments = 32;

        using LineVertex = Graphic::MaterialDebugLine::LineVertex;

        void AddLine( std::vector<LineVertex>& out, const glm::vec3& a, const glm::vec3& b )
        {
            out.push_back( { glm::vec4( a, 1.0f ), kColliderColor } );
            out.push_back( { glm::vec4( b, 1.0f ), kColliderColor } );
        }

        // Full circle around `center` in the plane spanned by axisA/axisB.
        void AddCircle( std::vector<LineVertex>& out, const glm::vec3& center, const glm::vec3& axisA,
                        const glm::vec3& axisB, float radius )
        {
            constexpr float step = glm::two_pi<float>() / kCircleSegments;
            for ( int i = 0; i < kCircleSegments; ++i )
            {
                const float a0 = step * i;
                const float a1 = step * ( i + 1 );
                AddLine( out, center + ( axisA * std::cos( a0 ) + axisB * std::sin( a0 ) ) * radius,
                         center + ( axisA * std::cos( a1 ) + axisB * std::sin( a1 ) ) * radius );
            }
        }

        // Half circle from +axisA over +axisB to -axisA (a capsule cap arc).
        void AddArc( std::vector<LineVertex>& out, const glm::vec3& center, const glm::vec3& axisA,
                     const glm::vec3& axisB, float radius )
        {
            constexpr int   segments = kCircleSegments / 2;
            constexpr float step     = glm::pi<float>() / segments;
            for ( int i = 0; i < segments; ++i )
            {
                const float a0 = step * i;
                const float a1 = step * ( i + 1 );
                AddLine( out, center + ( axisA * std::cos( a0 ) + axisB * std::sin( a0 ) ) * radius,
                         center + ( axisA * std::cos( a1 ) + axisB * std::sin( a1 ) ) * radius );
            }
        }
    } // namespace

    EditorColliderPass::~EditorColliderPass()
    {
        if ( const auto scene = m_Scene.lock() )
            scene->UnregisterExternalPass( "EditorColliders" );
    }

    Common::BoolResultStr EditorColliderPass::Install( const std::shared_ptr<::Desert::Core::Scene>& scene )
    {
        m_Scene = scene;

        const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "DebugLine" );
        if ( !shader )
            return Common::MakeError( "EditorColliderPass: missing shader 'DebugLine'" );

        Graphic::GraphicsPipelineSpecification spec;
        spec.DebugName         = "EditorColliderPipeline";
        spec.Shader            = shader;
        spec.Framebuffer       = scene->GetTargetFramebuffer();
        spec.Topology          = Graphic::PrimitiveTopology::Lines;
        spec.LineWidth         = 1.0f; // no wideLines feature — width stays 1.0 in SubmitLines
        spec.DepthTestEnabled  = true; // colliders occlude behind geometry (the old ImGui gizmo didn't)
        spec.DepthWriteEnabled = false;
        spec.DepthCompareOp    = Graphic::DepthCompare::CloserOrEqual;
        spec.CullMode          = Graphic::CullMode::None;
        // No vertex layout: the DebugLine shader pulls endpoints from the Lines storage buffer by index.

        const auto pipeline = Graphic::GraphicsPipeline::Create( spec );
        if ( !pipeline )
            return Common::MakeError( "EditorColliderPass: " + pipeline.GetError() );
        m_Pipeline = pipeline.GetValue();

        m_Material = std::make_unique<Graphic::MaterialDebugLine>();

        Graphic::ExternalPassSpecification pass;
        pass.Name                  = "EditorColliders";
        pass.Phase                 = Graphic::RenderPhase::Debug;
        pass.Dependencies          = { Graphic::RenderPassDependency( Graphic::RenderPhase::Geometry ) };
        pass.PipelineSpecification = m_Pipeline->GetSpecification();
        pass.Execute               = [this]( const Graphic::ExternalPassContext& ctx )
        {
            // Asked of the RENDERER, not the scene — see EditorGridPass for why. This flag in particular:
            // it defaulted to `true` in SceneSettings and 55 of 80 committed scenes carried it on, so the
            // green wireframes travelled through git into everybody's viewport.
            const auto scene = m_Scene.lock();
            if ( !scene || ctx.ScenePlaying || !ctx.Camera )
                return;
            const auto* renderer = scene->GetSceneRenderer();
            if ( !renderer || !renderer->GetDebugView().ShowColliders )
                return;

            std::vector<LineVertex> lines;
            BuildLines( lines );
            if ( lines.empty() )
                return;

            m_Material->Update( ctx.Camera, lines );
            Graphic::Renderer::GetInstance().SubmitLines( m_Pipeline.get(),
                                                          static_cast<uint32_t>( lines.size() ), 1.0f,
                                                          m_Material->GetMaterialExecutor() );
        };

        scene->RegisterExternalPass( std::move( pass ) );
        return BOOLSUCCESS;
    }

    void EditorColliderPass::BuildLines( std::vector<LineVertex>& outLines ) const
    {
        const auto scene = m_Scene.lock();
        if ( !scene )
            return;

        // Colliders are authored in WORLD units at the entity's translation+rotation — NOT scaled by
        // TransformComponent.Scale — to exactly match the shape PhysicsECSSystem feeds Jolt.
        const auto view =
             scene->GetRegistry().view<ECS::ColliderComponent, ECS::TransformComponent>();
        for ( auto entity : view )
        {
            const auto& collider  = view.get<ECS::ColliderComponent>( entity ).Data;
            const auto& transform = view.get<ECS::TransformComponent>( entity );

            const glm::mat3 R      = glm::mat3_cast( glm::quat( transform.Rotation ) );
            const glm::vec3 axisX  = R[0];
            const glm::vec3 axisY  = R[1];
            const glm::vec3 axisZ  = R[2];
            const glm::vec3 center = transform.Translation;

            switch ( collider.Shape )
            {
                case Physics::ShapeType::Box:
                {
                    const glm::vec3 he = collider.HalfExtents;
                    glm::vec3       c[8];
                    for ( int i = 0; i < 8; ++i )
                    {
                        const float sx = ( i & 1 ) ? 1.0f : -1.0f;
                        const float sy = ( i & 2 ) ? 1.0f : -1.0f;
                        const float sz = ( i & 4 ) ? 1.0f : -1.0f;
                        c[i] = center + axisX * ( sx * he.x ) + axisY * ( sy * he.y ) +
                               axisZ * ( sz * he.z );
                    }
                    static const int kEdges[12][2] = { { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 },
                                                       { 4, 5 }, { 5, 7 }, { 7, 6 }, { 6, 4 },
                                                       { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
                    for ( const auto& e : kEdges )
                        AddLine( outLines, c[e[0]], c[e[1]] );
                    break;
                }
                case Physics::ShapeType::Sphere:
                {
                    const float r = collider.Radius;
                    AddCircle( outLines, center, axisX, axisY, r );
                    AddCircle( outLines, center, axisX, axisZ, r );
                    AddCircle( outLines, center, axisY, axisZ, r );
                    break;
                }
                case Physics::ShapeType::Capsule:
                {
                    // Jolt capsule: axis along local Y; HalfHeight is the CYLINDER half-length,
                    // hemispherical caps of Radius extend beyond it.
                    const float     r    = collider.Radius;
                    const glm::vec3 top  = center + axisY * collider.HalfHeight;
                    const glm::vec3 bot  = center - axisY * collider.HalfHeight;

                    AddCircle( outLines, top, axisX, axisZ, r );
                    AddCircle( outLines, bot, axisX, axisZ, r );

                    // Four cylinder side lines.
                    AddLine( outLines, top + axisX * r, bot + axisX * r );
                    AddLine( outLines, top - axisX * r, bot - axisX * r );
                    AddLine( outLines, top + axisZ * r, bot + axisZ * r );
                    AddLine( outLines, top - axisZ * r, bot - axisZ * r );

                    // Cap arcs: two orthogonal half-circles per cap.
                    AddArc( outLines, top, axisX, axisY, r );
                    AddArc( outLines, top, axisZ, axisY, r );
                    AddArc( outLines, bot, axisX, -axisY, r );
                    AddArc( outLines, bot, axisZ, -axisY, r );
                    break;
                }
            }
        }
    }
} // namespace Desert::Editor::Render
