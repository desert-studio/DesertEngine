#pragma once

#include <Common/Core/Events/Event.hpp>
#include <Common/Core/Events/KeyEvents.hpp>
#include <Common/Core/Events/MouseEvents.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Projection.hpp>

#include "Frustum.hpp"

#include <Common/Core/EventRegistry.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Desert::Core
{
    // Projection kind for the viewport camera. Perspective is the default; Orthographic gives a flat
    // (parallel) projection for CAD-style/axis views (the view matrix is unchanged — only the projection).
    enum class ProjectionType : int
    {
        Perspective  = 0,
        Orthographic = 1,
    };

    // Base camera: owns the view/projection matrices + frustum. Concrete cameras (EditorCamera, the
    // input-driven viewport camera; GameplayCamera, driven by a scene CameraComponent) fill the matrices.
    // The renderer only ever sees this interface.
    class Camera
    {
    public:
        virtual ~Camera() = default;

        [[nodiscard]] virtual glm::mat4 GetViewMatrix() const { return m_ViewMatrix; }
        [[nodiscard]] virtual glm::mat4 GetProjectionMatrix() const { return m_ProjectionMatrix; }

        [[nodiscard]] const glm::vec3& GetPosition() const { return m_Position; }
        [[nodiscard]] float            GetNear() const { return m_NearPlane; }
        [[nodiscard]] float            GetFar() const { return m_FarPlane; }
        [[nodiscard]] float            GetFOV() const { return m_FOV; }
        [[nodiscard]] ProjectionType   GetProjectionType() const
        {
            return m_ProjectionType;
        }
        [[nodiscard]] float GetOrthoSize() const
        {
            return m_OrthoSize;
        }

        // Live setters used by the viewport camera-settings popup: update the param and rebuild the
        // projection at the last known viewport size so the change is visible immediately.
        void SetFOV( float fovDegrees );
        void SetNear( float nearPlane );
        void SetFar( float farPlane );
        void SetProjectionType( ProjectionType type );
        void SetOrthoSize( float halfHeight ); // orthographic vertical half-extent in world units

        virtual void OnUpdate( const Common::Timestep& /*timestep*/ )
        {
        }

        // Rebuild the projection (perspective or orthographic) for a new viewport aspect. Also caches the
        // viewport size so the live setters above can rebuild without the caller re-passing it.
        virtual void UpdateProjectionMatrix( const uint32_t width, const uint32_t height );

        // BY VALUE AND const. It used to return a reference into a mutable member that this call
        // rebuilt every time — a cache never read twice, whose only lasting effect was to make the
        // accessor non-const. That is why the engine had a Frustum class and nothing that culled with
        // it: `SceneRenderer::GetMainCamera()` hands the render path a CONST camera, so the one
        // accessor that could have produced a frustum was unreachable from the one place that needed
        // one. Zero callers in the engine was not an oversight; it was a signature.
        [[nodiscard]] Frustum GetFrustum() const;

    protected:
        glm::mat4 m_ProjectionMatrix = glm::mat4( 1.0f );
        glm::mat4 m_ViewMatrix       = glm::mat4( 1.0f );
        glm::vec3 m_Position         = glm::vec3( 0.0f );

        float          m_FOV            = 45.0f;
        float          m_NearPlane      = kDefaultNearPlane;
        float          m_FarPlane       = kDefaultFarPlane;
        ProjectionType m_ProjectionType = ProjectionType::Perspective;
        float          m_OrthoSize      = 1000.0f; // world half-height for the orthographic projection

        uint32_t m_ViewportWidth  = 0; // last viewport size (for the live setters to rebuild against)
        uint32_t m_ViewportHeight = 0;
    };

    // Free-orbit / fly viewport camera (RMB to look + WASDQE to move). Receives input globally via
    // EventHandler. This is what the editor renders through in Edit mode.
    class EditorCamera : public Camera, public Common::EventHandler
    {
    public:
        EditorCamera();
        explicit EditorCamera( const glm::mat4& projectionMatrix );

        void OnUpdate( const Common::Timestep& timestep ) override;
        void OnEvent( Common::Event& e ) override;

        // Editor projection anchors the apparent object SIZE to a reference height, so resizing the
        // viewport shows MORE of the scene instead of zooming objects in/out (UE/Unity editor feel).
        // Gameplay keeps the base (standard vertical-FOV) behaviour.
        void UpdateProjectionMatrix( const uint32_t width, const uint32_t height ) override;

        [[nodiscard]] const auto& GetDirection() const { return m_Direction; }

        // Editor fly-camera movement speed multiplier (1.0 = default). Exposed in the viewport overlay.
        [[nodiscard]] float GetMovementSpeed() const { return m_MovementSpeed; }
        void                SetMovementSpeed( float speed ) { m_MovementSpeed = speed; }

        // Set by the viewport panel every frame: keyboard/mouse input is only consumed while the
        // viewport is hovered and no ImGui text field wants the keyboard. An RMB-look that started
        // inside the viewport stays active until the button is released even if the cursor leaves.
        void SetInputEnabled( bool enabled ) { m_InputEnabled = enabled; }

        // A modeling tool owns the bare WASD/Q/E keys (Q/E = Push/Pull there), so while one is active the
        // fly-camera only listens to the keyboard during an RMB look — exactly like UE's Modeling Mode.
        void SetKeyboardRequiresLook( bool requiresLook )
        {
            m_KeyboardRequiresLook = requiresLook;
        }

        // Orbit the camera so it looks ALONG `forward` (a world-space direction) at the current focal point,
        // preserving the current framing distance. Drives the clickable view-axis gizmo (snap to Front/Top/
        // Right/... ortho-ish views). Derives yaw/pitch from the target forward under this camera's model.
        void SnapToDirection( const glm::vec3& forward );

        // Frame a world point: the focal point moves there and the camera backs off along its
        // CURRENT view direction to `distance`. Drives F-focus and hierarchy double-click.
        void Focus( const glm::vec3& point, float distance = 500.0f );

    private:
        bool OnKeyPress( Common::KeyPressedEvent& e );
        bool OnMouseMove( Common::MouseMovedEvent& e );

        glm::quat GetOrientation() const;
        glm::vec3 GetUpDirection() const;
        glm::vec3 GetRightDirection() const;
        glm::vec3 GetForwardDirection() const;

        void UpdateCameraView();

    private:
        glm::vec3 m_Orientation   = glm::vec3( 0.0f, 0.0f, -1.0f );
        glm::vec3 m_FocalPoint    = glm::vec3( 0.0f );
        glm::vec3 m_LocationDelta = glm::vec3( 0.0f );

        glm::vec2 m_InitialMousePosition = glm::vec2( 0.0f );

        glm::vec3 m_RightDirection = glm::vec3( 1.0, 0.0f, 0.0f );
        glm::vec3 m_Direction;

        float m_Distance = 0.0f;
        float m_Pitch = 0.0f, m_PitchDelta = 0.0f;
        float m_Yaw = 0.0f, m_YawDelta = 0.0f;
        float m_MovementSpeed = 1.0f; // multiplies the base fly speed (user-adjustable)

        // OFF until a viewport claims this camera (ViewportPanel sets it every frame from its hover state).
        // Defaulting to ON meant every offscreen scene's editor camera — the Details preview's, each asset
        // thumbnail's — flew with the viewport's RMB-look, because this reads the global mouse/keyboard
        // directly rather than through the panel that owns the view.
        bool m_InputEnabled         = false;
        bool m_Flying       = false; // RMB-look session in progress (started inside the viewport)
        bool m_KeyboardRequiresLook = false; // a modeling tool owns the keys unless RMB is held
    };

    // Camera driven by a scene entity (CameraComponent): view from the entity transform, projection from
    // the component's FOV/Near/Far. This is what the editor renders through in Play mode.
    class GameplayCamera : public Camera
    {
    public:
        GameplayCamera() = default;

        // Set from the entity's world position + rotation (Euler radians) and the component params.
        void SetFromTransform( const glm::vec3& position, const glm::vec3& eulerRotation, float fovDegrees,
                               float nearPlane, float farPlane, uint32_t width, uint32_t height );
    };
} // namespace Desert::Core
