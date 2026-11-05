#include "MeshToPhysicsTool.hpp"

#include <Editor/Core/ColliderFit.hpp>
#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/Selection/ViewportMode.hpp>
#include <Editor/RenderSystems/Passes/EditorColliderPass.hpp>
#include <Editor/RenderSystems/Passes/EditorToolPreviewPass.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/System/SystemRules.hpp>

#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Desert::Editor::Tools
{
    namespace
    {
        using MS = Core::ModelingState;

        // UE draws a tool's not-yet-accepted result in its preview colour, apart from the committed colliders'
        // green (EditorColliderPass) and volumes' outline.
        const glm::vec4 kPreviewColour{ 1.0f, 0.55f, 0.1f, 1.0f };

        struct Session
        {
            MS::Tool     Tool = MS::Tool::None;
            Common::UUID Entity;
            std::string  Name; // "Mesh To Collision Box", for the log
            // Mesh To Collision: the fitted collider (a ConvexHull's points are cooked from the mesh at Accept,
            // so the preview carries the hull it will be).
            ECS::ColliderData Collider;
            // Mesh To Volume: the volume's type, and for both a hull shape: its corners body-local at the
            // entity's world scale, three indices per triangle.
            ECS::VolumeType             VolumeType = ECS::VolumeType::Trigger;
            std::string                 VolumeName; // "Trigger": the new entity is "<VolumeName>Volume"
            Physics::ConvexHullGeometry Hull;
        };

        std::optional<Session>& Current()
        {
            static std::optional<Session> session;
            return session;
        }

        Common::ResultStr<ECS::Entity> SingleSelected( ::Desert::Core::Scene& scene )
        {
            const auto& ids = Core::SelectionManager::GetSelection();
            if ( ids.size() != 1 )
                return Common::MakeFormattedError<ECS::Entity>(
                     "select one entity with a static mesh ({} selected)", ids.size() );
            const auto ref = scene.FindEntityByID( ids.front() );
            if ( !ref )
                return Common::MakeFormattedError<ECS::Entity>( "entity {} is not in the scene",
                                                                static_cast<uint64_t>( ids.front() ) );
            return Common::MakeSuccess( ref->get() );
        }

        // The mesh's points at the entity's WORLD scale, body-local, hulled: the shape Jolt cooks for a
        // ConvexHull collider and for a volume.
        Common::ResultStr<Physics::ConvexHullGeometry> HullOf( const ECS::Entity& entity, const glm::vec3& scale )
        {
            auto gathered =
                 ECS::PhysicsECSSystem::GatherColliderMesh( *entity.GetRegistry(), entity.GetHandle(), scale );
            if ( !gathered.IsSuccess() )
                return Common::MakeError<Physics::ConvexHullGeometry>( gathered.GetError() );
            if ( !gathered.GetValue().has_value() )
                return Common::MakeError<Physics::ConvexHullGeometry>( "its mesh is still loading" );
            return Physics::BuildConvexHull( gathered.GetValue()->Points );
        }

        void Open( Session session, ::Desert::Core::Scene& scene )
        {
            if ( Current() )
                Render::ToolPreview::Hide( scene );
            LOG_INFO(
                 "[Modeling] {}: previewing on entity {} - Accept applies it, Cancel leaves the entity as it was",
                 session.Name, static_cast<uint64_t>( session.Entity ) );
            MS& ms        = MS::Get();
            ms.ActiveTool = session.Tool;
            ms.ReqAccept  = false;
            Core::ViewportMode::Set( Core::EditorMode::Modeling );
            Current() = std::move( session );
        }

        void End( ::Desert::Core::Scene& scene )
        {
            Current().reset();
            Render::ToolPreview::Hide( scene );
        }

        // The preview: the result's wireframe at the entity's current world pose, as the collider pass will draw
        // it once accepted. Each edge's surface (for ToolPreview's depth lift) is the outward direction from the
        // shape's centre.
        void ShowPreview( ::Desert::Core::Scene& scene, const Session& session, const ECS::Entity& entity )
        {
            const auto        world = ECS::Rules::DecomposeTransform( entity.GetWorldTransform() );
            const glm::mat3   R     = glm::mat3_cast( glm::quat( world.Rotation ) );
            ECS::ColliderData shape = session.Collider;
            if ( session.Tool == MS::Tool::MeshToVolume )
                shape.Shape = Physics::ShapeType::ConvexHull;

            Render::ToolPreviewMesh preview;
            Render::AppendColliderLines( preview.Edges, shape, world.Translation, R, session.Hull.Points,
                                         session.Hull.Indices, kPreviewColour );
            const glm::vec3 centre = world.Translation + R * shape.Center;
            preview.EdgeSurfaces.reserve( preview.Edges.size() / 2 );
            for ( size_t v = 0; v + 1 < preview.Edges.size(); v += 2 )
            {
                const glm::vec3 mid =
                     ( glm::vec3( preview.Edges[v].PositionWS ) + glm::vec3( preview.Edges[v + 1].PositionWS ) ) *
                     0.5f;
                const glm::vec3 out = mid - centre;
                preview.EdgeSurfaces.push_back( glm::dot( out, out ) > 1e-8f ? glm::normalize( out )
                                                                             : glm::vec3( 0.0f, 1.0f, 0.0f ) );
            }
            Render::ToolPreview::Show( scene, std::move( preview ) );
        }

        Common::ResultStr<bool> Commit( ::Desert::Core::Scene& scene, Session& session )
        {
            const Common::UUID id = session.Entity;
            if ( session.Tool == MS::Tool::MeshToCollision )
            {
                const ECS::ColliderData collider = session.Collider;
                Commands::MutateEntityUndoable( id,
                                                [&]
                                                {
                                                    auto live = scene.FindEntityByID( id );
                                                    if ( live )
                                                        live->get().AddComponent<ECS::ColliderComponent>(
                                                             ECS::ColliderComponent{ collider } );
                                                } );
                LOG_INFO( "[Modeling] {} accepted on entity {}", session.Name, static_cast<uint64_t>( id ) );
                return Common::MakeSuccess( true );
            }

            // Mesh To Volume: the volume takes the source's world pose (it is created at the scene root, the
            // source may be a child) and carries the scale in its points, as UE's tool bakes the actor's scale
            // into the new brush.
            const auto ref = scene.FindEntityByID( id );
            if ( !ref )
                return Common::MakeFormattedError<bool>( "entity {} is not in the scene",
                                                         static_cast<uint64_t>( id ) );
            const auto        world    = ECS::Rules::DecomposeTransform( ref->get().GetWorldTransform() );
            const std::string name     = session.VolumeName + "Volume";
            const auto        replaced = Commands::ReplaceWithNewEntity(
                 id, name,
                 [&]( ECS::Entity& volume )
                 {
                     volume.SetLocalTransform(
                          ECS::LocalTransform{ world.Translation, world.Rotation, glm::vec3( 1.0f ) } );

                     ECS::VolumeComponent component;
                     component.Data.Type = session.VolumeType;
                     ECS::SetVolumeCorners( component.Data, session.Hull.Points );
                     component.Data.HullIndices = session.Hull.Indices;
                     volume.AddComponent<ECS::VolumeComponent>( std::move( component ) );

                     ECS::RigidBodyComponent body;
                     body.Data.Type             = Physics::BodyType::Static;
                     body.Data.CollisionProfile = ECS::VolumeCollisionProfile( session.VolumeType );
                     volume.AddComponent<ECS::RigidBodyComponent>( std::move( body ) );

                     ECS::ColliderComponent collider;
                     collider.Data.Shape = Physics::ShapeType::ConvexHull;
                     volume.AddComponent<ECS::ColliderComponent>( std::move( collider ) );
                 } );
            if ( !replaced.IsSuccess() )
                return Common::MakeError<bool>( replaced.GetError() );
            LOG_INFO( "[Modeling] {} accepted on entity {}: volume {}", session.Name, static_cast<uint64_t>( id ),
                      static_cast<uint64_t>( replaced.GetValue() ) );
            return Common::MakeSuccess( true );
        }
    } // namespace

    Common::ResultStr<bool> MeshToPhysicsTool::BeginCollision( ::Desert::Core::Scene& scene, const char* name,
                                                               Physics::ShapeType shape )
    {
        const auto selected = SingleSelected( scene );
        if ( !selected.IsSuccess() )
            return Common::MakeError<bool>( selected.GetError() );
        const ECS::Entity  entity = selected.GetValue();
        const Common::UUID uuid   = Core::SelectionManager::GetSelection().front();
        const auto         id     = static_cast<uint64_t>( uuid );

        const auto fit = Core::FitEntityCollider( entity, shape );
        if ( !fit.IsSuccess() )
            return Common::MakeFormattedError<bool>( "entity {}: {}", id, fit.GetError() );
        Session session;
        session.Tool     = MS::Tool::MeshToCollision;
        session.Entity   = uuid;
        session.Name     = std::string( "Mesh To Collision " ) + name;
        session.Collider = fit.GetValue();
        if ( shape == Physics::ShapeType::ConvexHull || shape == Physics::ShapeType::Mesh )
        {
            auto hull = HullOf( entity, ECS::Rules::DecomposeTransform( entity.GetWorldTransform() ).Scale );
            if ( !hull.IsSuccess() )
                return Common::MakeFormattedError<bool>( "entity {}: {}", id, hull.GetError() );
            session.Hull = std::move( hull.GetValue() );
        }
        Open( std::move( session ), scene );
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<bool> MeshToPhysicsTool::BeginVolume( ::Desert::Core::Scene& scene, const char* name,
                                                            ECS::VolumeType type )
    {
        const auto selected = SingleSelected( scene );
        if ( !selected.IsSuccess() )
            return Common::MakeError<bool>( selected.GetError() );
        const ECS::Entity  entity = selected.GetValue();
        const Common::UUID uuid   = Core::SelectionManager::GetSelection().front();
        const auto         id     = static_cast<uint64_t>( uuid );
        if ( entity.HasComponent<ECS::VolumeComponent>() )
            return Common::MakeFormattedError<bool>( "entity {} is already a volume", id );

        // UE's AVolume brush is the CONVEX HULL of the mesh (UMeshToVolumeTool, ConvexHull), not its triangles:
        // the outline drawn is the region Jolt simulates.
        auto hull = HullOf( entity, ECS::Rules::DecomposeTransform( entity.GetWorldTransform() ).Scale );
        if ( !hull.IsSuccess() )
            return Common::MakeFormattedError<bool>( "entity {}: {}", id, hull.GetError() );
        Session session;
        session.Tool       = MS::Tool::MeshToVolume;
        session.Entity     = uuid;
        session.Name       = std::string( "Mesh To Volume " ) + name;
        session.VolumeType = type;
        session.VolumeName = name;
        session.Hull       = std::move( hull.GetValue() );
        Open( std::move( session ), scene );
        return Common::MakeSuccess( true );
    }

    MS::Tool MeshToPhysicsTool::OpenTool()
    {
        return Current() ? Current()->Tool : MS::Tool::None;
    }

    void MeshToPhysicsTool::Update( ::Desert::Core::Scene& scene )
    {
        auto& current = Current();
        if ( !current )
            return;
        MS&        ms       = MS::Get();
        Session&   session  = *current;
        const auto ids      = Core::SelectionManager::GetSelection();
        const bool selected = ids.size() == 1 && ids.front() == session.Entity;
        const auto ref      = scene.FindEntityByID( session.Entity );

        // The session ends without Accept when its tool is no longer the active one (the bar's Cancel ends the
        // tool and raises ReqCancel, which was this tool's) or the selection moved to another entity.
        if ( ms.ActiveTool != session.Tool || !selected || !ref )
        {
            if ( ms.ActiveTool != session.Tool && ms.ReqCancel )
                ms.ReqCancel = false;
            LOG_INFO( "[Modeling] {}: cancelled, entity {} is unchanged", session.Name,
                      static_cast<uint64_t>( session.Entity ) );
            End( scene );
            return;
        }

        if ( ms.ReqAccept )
        {
            ms.ReqAccept         = false;
            ms.ReqAcceptEndsTool = false;
            if ( const auto done = Commit( scene, session ); !done.IsSuccess() )
            {
                // Refused: the tool stays open with its preview, and says why.
                LOG_WARN( "[Modeling] {}: {}", session.Name, done.GetError() );
                return;
            }
            // The result is on the entity (or replaced it): nothing is left to preview, so the tool ends with
            // either Accept - UE's Mesh To Collision / Volume are single-shot tools too.
            End( scene );
            ms.ActiveTool = MS::Tool::None;
            return;
        }
        ShowPreview( scene, session, ref->get() );
    }
} // namespace Desert::Editor::Tools
