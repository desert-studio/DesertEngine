#include "EditorColliderPass.hpp"
#include <Engine/Graphic/ViewTargetLayouts.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/SceneRenderer.hpp> // the view's own debug/show state
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <unordered_set>

namespace Desert::Editor::Render
{
    namespace
    {
        // UE-style collider green.
        const glm::vec4 kColliderColor{ 0.27f, 0.9f, 0.35f, 1.0f };

        constexpr int kCircleSegments = 32;

        using LineVertex = Graphic::MaterialDebugLine::LineVertex;

        void AddLine( std::vector<LineVertex>& out, const glm::vec3& a, const glm::vec3& b,
                      const glm::vec4& colour )
        {
            out.push_back( { glm::vec4( a, 1.0f ), colour } );
            out.push_back( { glm::vec4( b, 1.0f ), colour } );
        }

        // Full circle around `center` in the plane spanned by axisA/axisB.
        void AddCircle( std::vector<LineVertex>& out, const glm::vec3& center, const glm::vec3& axisA,
                        const glm::vec3& axisB, float radius, const glm::vec4& colour )
        {
            constexpr float step = glm::two_pi<float>() / kCircleSegments;
            for ( int i = 0; i < kCircleSegments; ++i )
            {
                const float a0 = step * i;
                const float a1 = step * ( i + 1 );
                AddLine( out, center + ( axisA * std::cos( a0 ) + axisB * std::sin( a0 ) ) * radius,
                         center + ( axisA * std::cos( a1 ) + axisB * std::sin( a1 ) ) * radius, colour );
            }
        }

        // Half circle from +axisA over +axisB to -axisA (a capsule cap arc).
        void AddArc( std::vector<LineVertex>& out, const glm::vec3& center, const glm::vec3& axisA,
                     const glm::vec3& axisB, float radius, const glm::vec4& colour )
        {
            constexpr int   segments = kCircleSegments / 2;
            constexpr float step     = glm::pi<float>() / segments;
            for ( int i = 0; i < segments; ++i )
            {
                const float a0 = step * i;
                const float a1 = step * ( i + 1 );
                AddLine( out, center + ( axisA * std::cos( a0 ) + axisB * std::sin( a0 ) ) * radius,
                         center + ( axisA * std::cos( a1 ) + axisB * std::sin( a1 ) ) * radius, colour );
            }
        }
    } // namespace

    void AppendColliderLines( std::vector<Graphic::MaterialDebugLine::LineVertex>& out,
                              const ECS::ColliderData& collider, const glm::vec3& translation, const glm::mat3& R,
                              std::span<const glm::vec3> hullPoints, std::span<const uint32_t> hullIndices,
                              const glm::vec4& colour )
    {
        const glm::vec3 axisX  = R[0];
        const glm::vec3 axisY  = R[1];
        const glm::vec3 axisZ  = R[2];
        const glm::vec3 center = translation + R * collider.Center;

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
                    c[i] = center + axisX * ( sx * he.x ) + axisY * ( sy * he.y ) + axisZ * ( sz * he.z );
                }
                static const int kEdges[12][2] = { { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 }, { 4, 5 }, { 5, 7 },
                                                   { 7, 6 }, { 6, 4 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
                for ( const auto& e : kEdges )
                    AddLine( out, c[e[0]], c[e[1]], colour );
                break;
            }
            case Physics::ShapeType::Sphere:
            {
                const float r = collider.Radius;
                AddCircle( out, center, axisX, axisY, r, colour );
                AddCircle( out, center, axisX, axisZ, r, colour );
                AddCircle( out, center, axisY, axisZ, r, colour );
                break;
            }
            case Physics::ShapeType::Mesh:
            case Physics::ShapeType::ConvexHull:
            {
                // The hull's edges, each once.
                const size_t                 corners = hullPoints.size();
                const auto                   world = [&]( uint32_t i ) { return translation + R * hullPoints[i]; };
                std::unordered_set<uint64_t> drawn;
                const auto                   edge = [&]( uint32_t a, uint32_t b )
                {
                    if ( a >= corners || b >= corners )
                        return;
                    const uint64_t key = ( uint64_t( std::min( a, b ) ) << 32u ) | std::max( a, b );
                    if ( drawn.insert( key ).second )
                        AddLine( out, world( a ), world( b ), colour );
                };
                for ( size_t t = 0; t + 2 < hullIndices.size(); t += 3 )
                {
                    edge( hullIndices[t], hullIndices[t + 1] );
                    edge( hullIndices[t + 1], hullIndices[t + 2] );
                    edge( hullIndices[t + 2], hullIndices[t] );
                }
                break;
            }
            case Physics::ShapeType::Capsule:
            {
                // Jolt capsule: axis along local Y; HalfHeight is the CYLINDER half-length,
                // hemispherical caps of Radius extend beyond it.
                // Axis picks which body axis plays Jolt's Y; the other two span the rings.
                glm::vec3 up = axisY;
                if ( collider.Axis == Physics::CapsuleAxis::X )
                    up = axisX;
                else if ( collider.Axis == Physics::CapsuleAxis::Z )
                    up = axisZ;
                const glm::vec3 u   = collider.Axis == Physics::CapsuleAxis::X ? axisY : axisX;
                const glm::vec3 w   = collider.Axis == Physics::CapsuleAxis::Z ? axisY : axisZ;
                const float     r   = collider.Radius;
                const glm::vec3 top = center + up * collider.HalfHeight;
                const glm::vec3 bot = center - up * collider.HalfHeight;

                AddCircle( out, top, u, w, r, colour );
                AddCircle( out, bot, u, w, r, colour );

                // Four cylinder side lines.
                AddLine( out, top + u * r, bot + u * r, colour );
                AddLine( out, top - u * r, bot - u * r, colour );
                AddLine( out, top + w * r, bot + w * r, colour );
                AddLine( out, top - w * r, bot - w * r, colour );

                // Cap arcs: two orthogonal half-circles per cap.
                AddArc( out, top, u, up, r, colour );
                AddArc( out, top, w, up, r, colour );
                AddArc( out, bot, u, -up, r, colour );
                AddArc( out, bot, w, -up, r, colour );
                break;
            }
        }
    }

    EditorColliderPass::~EditorColliderPass()
    {
        if ( const auto scene = m_Scene.lock() )
            scene->UnregisterExtensionPass( "EditorColliders" );
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
        spec.TargetLayout      = Desert::Graphic::SceneTargetLayout();
        spec.Topology          = Graphic::PrimitiveTopology::Lines;
        spec.LineWidth         = 1.0f; // no wideLines feature — width stays 1.0 in SubmitPulled
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

        Graphic::ExtensionPass pass;
        pass.Name                  = "EditorColliders";
        pass.Point                 = Graphic::RDG::ExtensionPoint::Overlay;
        // The lines Declare uploaded for this view; the exec only draws them.
        pass.Execute = [this]( const Graphic::ExtensionPassContext&,
                               Graphic::RDG::PassContext& ) -> Common::BoolResultStr
        {
            if ( m_LineVertices == 0 )
                return BOOLSUCCESS;
            Graphic::Renderer::GetInstance().SubmitPulled( m_Pipeline.get(), m_LineVertices, 1.0f,
                                                           m_Material->GetMaterialExecutor() );
            return BOOLSUCCESS;
        };
        // RENDER-THREADS M1 (UE: a pass's buffers are filled before the graph records): the lines are built and
        // written into the material in the node's setup - the write may grow its storage buffer, which drops the
        // view copies - and the material is prepared, so the exec creates nothing. A view's setup and exec run
        // back to back (the pass runs inside its view's OnUpdate), so one pass object still serves every view.
        //
        // THE LINES ARE BUILT FROM THE REGISTRY ON THE GAME THREAD (ExtensionPass::Gather, when the view opens its
        // frame): the setup and exec run on the render task while the interface edits the registry.
        pass.Gather = [this]( const Graphic::ExtensionPassGatherContext& ctx )
        {
            m_GatheredLines.clear();
            // Asked of the RENDERER, not the scene — see EditorGridPass for why. This flag in particular:
            // it defaulted to `true` in SceneSettings and 55 of 80 committed scenes carried it on, so the
            // green wireframes travelled through git into everybody's viewport.
            if ( ctx.ScenePlaying || !ctx.Camera || !ctx.Renderer )
                return;
            BuildLines( m_GatheredLines, ctx.Renderer->GetDebugView().ShowColliders );
        };
        pass.Declare = [this]( Graphic::RenderPassDeclaration& declared, const Graphic::ExtensionPassContext& ctx )
        {
            m_LineVertices = 0;
            if ( m_GatheredLines.empty() || ctx.Renderer == nullptr )
                return;
            // The lines are drawn over the scene view: its frame's camera block (the pass runs inside OnUpdate).
            const Graphic::ViewFrame* view = ctx.Renderer->GetViewFrame();
            if ( view == nullptr )
                return;

            m_Material->Update( *view, m_GatheredLines );
            declared.Prepares( *m_Material->GetMaterialExecutor() );
            m_LineVertices = static_cast<uint32_t>( m_GatheredLines.size() );
        };

        scene->RegisterExtensionPass( std::move( pass ) );
        return BOOLSUCCESS;
    }

    void EditorColliderPass::BuildLines( std::vector<LineVertex>& outLines, bool showColliders ) const
    {
        const auto scene = m_Scene.lock();
        if ( !scene )
            return;

        // Colliders are authored in WORLD units at the entity's translation+rotation — NOT scaled by
        // TransformComponent.Scale — to exactly match the shape PhysicsECSSystem feeds Jolt.
        const auto             view = scene->GetRegistry().view<ECS::ColliderComponent, ECS::TransformComponent>();
        std::vector<glm::vec3> hull;
        for ( auto entity : view )
        {
            const auto* volume = scene->GetRegistry().try_get<ECS::VolumeComponent>( entity );
            if ( !showColliders && volume == nullptr )
                continue;
            const auto& collider  = view.get<ECS::ColliderComponent>( entity ).Data;
            const auto& transform = view.get<ECS::TransformComponent>( entity );
            const glm::mat3 R         = glm::mat3_cast( glm::quat( transform.GetRotation() ) );

            // A Mesh / ConvexHull collider is built from the entity's own mesh, which is already drawn: an
            // outline would repeat it. A VOLUME draws nothing else, so its hull IS drawn (UE: a volume shows as
            // its brush outline), with the entity's scale, as PhysicsECSSystem cooks it.
            hull.clear();
            std::span<const uint32_t> indices;
            if ( volume != nullptr )
            {
                const size_t corners = ECS::VolumeCornerCount( volume->Data );
                hull.reserve( corners );
                for ( size_t i = 0; i < corners; ++i )
                    hull.push_back( ECS::VolumeCorner( volume->Data, i ) * transform.GetScale() );
                indices = volume->Data.HullIndices;
            }
            AppendColliderLines( outLines, collider, transform.GetTranslation(), R, hull, indices,
                                 kColliderColor );
        }
    }
} // namespace Desert::Editor::Render
