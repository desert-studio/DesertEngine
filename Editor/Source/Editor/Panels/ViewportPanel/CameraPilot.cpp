#include "CameraPilot.hpp"

#include <Editor/Core/Commands/SceneCommands.hpp>

#include <Engine/Core/CameraEntityView.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>

#include <glm/gtc/quaternion.hpp>

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

    Common::BoolResultStr CameraPilot::Begin( const ::Desert::Core::Scene& scene, const Common::UUID& uuid,
                                              ::Desert::Core::EditorCamera& camera )
    {
        const auto entity = CameraEntity( scene, uuid );
        if ( !entity )
            return Common::MakeError<bool>( "the entity is not a camera in this viewport's scene." );

        // Re-targeting keeps the pose captured when piloting began: Eject returns to where the user was
        // before ANY piloting, not to the previous camera.
        if ( !m_Entity )
            m_Saved = camera.CapturePose();
        CloseSegment();

        m_Entity     = uuid;
        m_EntityName = entity->HasComponent<ECS::TagComponent>() ? entity->GetComponent<ECS::TagComponent>().Tag
                                                                 : std::string( "Camera" );

        camera.SetProjectionType( ::Desert::Core::ProjectionType::Perspective ); // a camera component has no ortho
        camera.SetExactLens( true );

        // Place first, so the very first Update sees no motion and does not write the entity.
        const auto& data = entity->GetComponent<ECS::CameraComponent>().Data;
        const auto  view =
             ::Desert::Core::CameraEntityViewOf( entity->GetWorldTransform(), data.FOV, data.Near, data.Far );
        camera.PlaceAt( view.Position, view.Basis );
        camera.SetFOV( view.FovYDegrees );
        camera.SetNear( view.Near );
        camera.SetFar( view.Far );
        m_LastPosition = camera.GetPosition();
        m_LastBasis    = camera.GetBasis();
        return Common::MakeSuccess<bool>( true );
    }

    void CameraPilot::Eject( ::Desert::Core::EditorCamera* camera )
    {
        if ( !m_Entity )
            return;
        CloseSegment();
        if ( camera != nullptr )
            camera->RestorePose( m_Saved );
        m_Entity.reset();
        m_EntityName.clear();
    }

    void CameraPilot::CloseSegment()
    {
        if ( !m_InSegment || !m_Entity )
            return;
        m_InSegment = false;
        // One undo step for the whole stretch; RecordTransformEdit reads the CURRENT transform as "after"
        // and no-ops when nothing changed.
        Commands::RecordTransformEdit( *m_Entity, m_SegmentTranslation, m_SegmentRotation, m_SegmentScale );
    }

    void CameraPilot::Update( const ::Desert::Core::Scene& scene, ::Desert::Core::EditorCamera& camera )
    {
        if ( !m_Entity )
            return;

        auto entity = CameraEntity( scene, *m_Entity );
        if ( !entity )
        {
            // Deleted, or its camera component removed, while piloted: there is nothing left to look
            // through, so the viewport goes back to where it was rather than freezing on a stale view.
            LOG_WARN( "[Viewport] '{}' is no longer a camera; ejecting.", m_EntityName );
            m_InSegment = false;
            Eject( &camera );
            return;
        }

        auto& tc = entity->GetComponent<ECS::TransformComponent>();

        const glm::vec3                 position = camera.GetPosition();
        const ::Desert::Core::ViewBasis basis    = camera.GetBasis();
        if ( Moved( position, basis, m_LastPosition, m_LastBasis ) )
        {
            if ( !m_InSegment )
            {
                m_InSegment          = true;
                m_SegmentTranslation = tc.Translation;
                m_SegmentRotation    = tc.Rotation;
                m_SegmentScale       = tc.Scale;
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
            tc.Translation = glm::vec3( local[3] );
            tc.Rotation    = glm::eulerAngles( glm::quat_cast( rotation ) );
        }
        else
        {
            CloseSegment();
        }

        // The entity is the source of truth: whatever it says now — after the flight above, or after an
        // edit in the Details panel, or an undo — is what the viewport shows.
        const auto& data = entity->GetComponent<ECS::CameraComponent>().Data;
        const auto  view =
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
