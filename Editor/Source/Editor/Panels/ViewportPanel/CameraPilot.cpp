#include "CameraPilot.hpp"

#include <Editor/Core/Commands/SceneCommands.hpp>

#include <Engine/Core/CameraEntityView.hpp>
#include <Engine/Core/Scene.hpp>

#include <glm/gtc/quaternion.hpp>
#include <Engine/ECS/CameraComponent.hpp>
#include <Engine/ECS/TagComponent.hpp>
#include <Engine/ECS/TransformComponent.hpp>

namespace Desert::Editor
{
    namespace
    {
        // What counts as the user having moved the viewport since the pose was last written. The fly
        // camera's damping decays geometrically and never reaches exactly zero, so an exact comparison
        // would call every frame a flight and pile up undo steps forever; a thousandth of a centimetre
        // and ~0.03 degrees are both far below anything a person does on purpose.
        constexpr float kMovedDistance = 1.0e-3f; // cm
        constexpr float kMovedCosine   = 1.0e-7f; // 1 - cos(angle)

        bool Moved( const glm::vec3& position, const ::Desert::Core::ViewBasis& basis,
                    const glm::vec3& lastPosition, const ::Desert::Core::ViewBasis& lastBasis )
        {
            return glm::length( position - lastPosition ) > kMovedDistance ||
                   1.0f - glm::dot( glm::normalize( basis.Forward ), lastBasis.Forward ) > kMovedCosine ||
                   1.0f - glm::dot( glm::normalize( basis.Up ), lastBasis.Up ) > kMovedCosine;
        }

        std::optional<ECS::Entity> CameraEntity( const ::Desert::Core::Scene& scene, const Common::UUID& uuid )
        {
            const auto found = scene.FindEntityByID( uuid );
            if ( !found )
                return std::nullopt;
            ECS::Entity entity = found->get();
            if ( !entity.HasComponent<ECS::CameraComponent>() || !entity.HasComponent<ECS::TransformComponent>() )
                return std::nullopt;
            return entity;
        }
    } // namespace

    Common::BoolResultStr CameraPilot::Begin( const ::Desert::Core::Scene& scene, const Common::UUID& uuid )
    {
        if ( !CameraEntity( scene, uuid ) )
            return Common::MakeError<bool>( "the entity is not a camera in this viewport's scene." );
        // Only the request is recorded: Update places the camera (and saves the pose to go back to the
        // first time any layer is set), so a pilot under a Camera Cut waits until the cut goes.
        m_User = uuid;
        return Common::MakeSuccess<bool>( true );
    }

    void CameraPilot::Eject( ::Desert::Core::EditorCamera* camera )
    {
        if ( !m_User )
            return;
        CloseSegment();
        m_User.reset();
        if ( m_Cinematic )
            return; // the cut stays in force; the saved pose is restored when it goes
        if ( camera != nullptr && m_HasSaved )
        {
            camera->RestorePose( m_Saved );
            camera->FollowEntity( ::Desert::Core::Camera::kNoSourceEntity );
        }
        m_HasSaved = false;
        m_Shown.reset();
        m_ShownName.clear();
    }

    void CameraPilot::SetCinematic( const std::optional<Common::UUID>& uuid )
    {
        if ( uuid == m_Cinematic )
            return;
        const std::optional<Common::UUID> activeAfter = uuid ? uuid : m_User;
        if ( activeAfter != ActiveEntity() )
            CloseSegment();
        m_Cinematic = uuid;
    }

    void CameraPilot::CloseSegment()
    {
        if ( !m_InSegment || !m_Shown )
            return;
        m_InSegment = false;
        // One undo step for the whole stretch; RecordTransformEdit reads the CURRENT transform as "after"
        // and no-ops when nothing changed.
        Commands::RecordTransformEdit( *m_Shown, m_SegmentTranslation, m_SegmentRotation, m_SegmentScale );
    }

    void CameraPilot::Update( const ::Desert::Core::Scene& scene, ::Desert::Core::EditorCamera& camera )
    {
        const std::optional<Common::UUID> active = ActiveEntity();
        if ( !active )
        {
            // Both layers are empty: back to where the editor camera was before the first one.
            if ( m_Shown )
            {
                m_InSegment = false;
                if ( m_HasSaved )
                    camera.RestorePose( m_Saved );
                camera.FollowEntity( ::Desert::Core::Camera::kNoSourceEntity );
                m_Shown.reset();
                m_ShownName.clear();
            }
            m_HasSaved = false;
            return;
        }

        auto entity = CameraEntity( scene, *active );
        if ( !entity )
        {
            // Deleted, or its camera component removed, while looked through: that layer goes, and the
            // next frame falls through to the layer underneath (or back to the saved pose).
            m_InSegment = false;
            if ( m_Cinematic )
            {
                LOG_WARN( "[Viewport] the Camera Cut's entity is no longer a camera; dropping the cut." );
                m_Cinematic.reset();
            }
            else
            {
                LOG_WARN( "[Viewport] '{}' is no longer a camera; ejecting.", m_ShownName );
                m_User.reset();
            }
            return;
        }

        const auto& data = entity->GetComponent<ECS::CameraComponent>().Data;

        if ( active != m_Shown )
        {
            // A new entity to look through (Begin, a cut switch, an Eject under a cut): place the camera
            // without reading the jump as the user's flight, and give the view a new camera identity so
            // its temporal history is dropped (SceneViewState's camera-cut reset).
            CloseSegment();
            if ( !m_HasSaved )
            {
                m_Saved    = camera.CapturePose();
                m_HasSaved = true;
            }
            // A camera component has no ortho.
            camera.SetProjectionType( ::Desert::Core::ProjectionType::Perspective );
            camera.SetExactLens( true );
            const auto view =
                 ::Desert::Core::CameraEntityViewOf( entity->GetWorldTransform(), data.FOV, data.Near, data.Far );
            camera.PlaceAt( view.Position, view.Basis );
            camera.SetFOV( view.FovYDegrees );
            camera.SetNear( view.Near );
            camera.SetFar( view.Far );
            camera.FollowEntity( static_cast<uint32_t>( entity->GetHandle() ) );

            m_Shown     = active;
            m_ShownName = entity->HasComponent<ECS::TagComponent>() ? entity->GetComponent<ECS::TagComponent>().Tag
                                                                    : std::string( "Camera" );
            m_LastPosition = camera.GetPosition();
            m_LastBasis    = camera.GetBasis();
            return;
        }

        const auto& tc = entity->GetComponent<ECS::TransformComponent>();

        const glm::vec3                 position = camera.GetPosition();
        const ::Desert::Core::ViewBasis basis    = camera.GetBasis();
        if ( Moved( position, basis, m_LastPosition, m_LastBasis ) )
        {
            if ( !m_InSegment )
            {
                m_InSegment          = true;
                m_SegmentTranslation = tc.GetTranslation();
                m_SegmentRotation    = tc.GetRotation();
                m_SegmentScale       = tc.GetScale();
            }

            // The viewport's pose becomes the entity's WORLD pose; the parent chain is peeled off so the
            // stored LOCAL transform produces it. Scale is the entity's own and is not touched.
            const glm::mat4 world       = entity->GetWorldTransform();
            const glm::mat4 parentWorld = world * glm::inverse( tc.GetTransform() );
            const glm::vec3 worldScale( glm::length( glm::vec3( world[0] ) ), glm::length( glm::vec3( world[1] ) ),
                                        glm::length( glm::vec3( world[2] ) ) );
            const glm::mat4 local = glm::inverse( parentWorld ) *
                                    ::Desert::Core::CameraWorldTransformFor( position, basis, worldScale );

            const glm::mat3 rotation( glm::normalize( glm::vec3( local[0] ) ),
                                      glm::normalize( glm::vec3( local[1] ) ),
                                      glm::normalize( glm::vec3( local[2] ) ) );
            ECS::LocalTransform posed = tc.GetLocal();
            posed.Translation         = glm::vec3( local[3] );
            posed.Rotation            = glm::eulerAngles( glm::quat_cast( rotation ) );
            entity->SetLocalTransform( posed );
        }
        else
        {
            CloseSegment();
        }

        // The entity is the source of truth: whatever it says now — after the flight above, or after an
        // edit in the Details panel, or an undo — is what the viewport shows.
        const auto view =
             ::Desert::Core::CameraEntityViewOf( entity->GetWorldTransform(), data.FOV, data.Near, data.Far );
        camera.PlaceAt( view.Position, view.Basis );
        if ( camera.GetFOV() != view.FovYDegrees )
            camera.SetFOV( view.FovYDegrees );
        if ( camera.GetNear() != view.Near )
            camera.SetNear( view.Near );
        if ( camera.GetFar() != view.Far )
            camera.SetFar( view.Far );

        m_LastPosition = camera.GetPosition();
        m_LastBasis    = camera.GetBasis();
    }
} // namespace Desert::Editor
