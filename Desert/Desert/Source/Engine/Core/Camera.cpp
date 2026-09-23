#include <Engine/Core/Camera.hpp>
#include <Engine/Core/CameraPitchLimit.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/Input.hpp>
#include <Engine/Core/Projection.hpp>

#include <glm/gtc/quaternion.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

namespace Desert::Core
{
    // ─── Camera (base) ──────────────────────────────────────────────────────────
    void Camera::UpdateProjectionMatrix( const uint32_t width, const uint32_t height )
    {
        m_ViewportWidth         = width;
        m_ViewportHeight        = height;
        const float aspectRatio = static_cast<float>( width ) / static_cast<float>( height == 0 ? 1 : height );

        if ( m_ProjectionType == ProjectionType::Orthographic )
        {
            const float halfH  = m_OrthoSize;
            const float halfW  = m_OrthoSize * aspectRatio;
            m_ProjectionMatrix = MakeOrthographic( -halfW, halfW, -halfH, halfH, m_NearPlane, m_FarPlane );
        }
        else
        {
            m_ProjectionMatrix = MakePerspective( glm::radians( m_FOV ), aspectRatio, m_NearPlane, m_FarPlane );
        }
    }

    void Camera::SetFOV( float fovDegrees )
    {
        m_FOV = fovDegrees;
        if ( m_ViewportWidth > 0 )
            UpdateProjectionMatrix( m_ViewportWidth, m_ViewportHeight );
    }

    void Camera::SetNear( float nearPlane )
    {
        m_NearPlane = nearPlane;
        if ( m_ViewportWidth > 0 )
            UpdateProjectionMatrix( m_ViewportWidth, m_ViewportHeight );
    }

    void Camera::SetFar( float farPlane )
    {
        m_FarPlane = farPlane;
        if ( m_ViewportWidth > 0 )
            UpdateProjectionMatrix( m_ViewportWidth, m_ViewportHeight );
    }

    void Camera::SetProjectionType( ProjectionType type )
    {
        m_ProjectionType = type;
        if ( m_ViewportWidth > 0 )
            UpdateProjectionMatrix( m_ViewportWidth, m_ViewportHeight );
    }

    void Camera::SetOrthoSize( float halfHeight )
    {
        m_OrthoSize = halfHeight;
        if ( m_ViewportWidth > 0 )
            UpdateProjectionMatrix( m_ViewportWidth, m_ViewportHeight );
    }

    Frustum Camera::GetFrustum() const
    {
        return Frustum( m_ProjectionMatrix, m_ViewMatrix );
    }

    // ─── EditorCamera (orbit / fly) ─────────────────────────────────────────────
    EditorCamera::EditorCamera()
    {
        const auto window = EngineContext::GetInstance().GetWindow();
        const auto width  = window ? window->GetWidth() : 1280;
        const auto height = window ? window->GetHeight() : 720;

        UpdateProjectionMatrix( width, height );

        // THE EDITOR OPENS LOOKING AT THE HORIZON, NOT AT THE FLOOR.
        //
        // It used to open at (500, 500, 500) with a pitch of forty-five degrees DOWN, so the entire frame
        // was below the horizon: every scene opened on its ground, the sky was off-screen, and the editor
        // grid — which lies in the y = 0 plane — was seen from five metres up at a steep angle.
        //
        // Now: two metres above the plane, aimed a few degrees below the horizon. The grid stays visible
        // and gives the scale, the horizon sits just above centre, and the sky — which in this engine
        // carries an atmosphere and a cloud layer — is the larger half of the frame. Two metres is eye
        // height, which is also the height every sky and cloud calibration in this repository is checked
        // from.
        // Eye height, aimed slightly ABOVE the horizon.
        //
        // The orbit camera derives its position from the focal point: position = focal - forward *
        // distance. With the focal point at the origin, an upward pitch puts the camera BELOW the ground,
        // which is why the old default could only look down. Lifting the focal point to eye height frees
        // it: the camera sits just under the pivot and looks up past it, and both stay above the plane.
        //
        // Eight degrees up puts the horizon in the lower fifth of the frame. The grid and the ground stay
        // visible for scale, and the rest is sky — which in this engine carries an atmosphere and a cloud
        // layer and is the thing most scenes are opened to look at.
        constexpr float kEyeHeightWorldUnits = 200.0f; // 2 m, in centimetres
        constexpr float kPitchAboveHorizon   = -0.14f; // radians, ~8 degrees up

        m_FocalPoint = glm::vec3( 0.0f, kEyeHeightWorldUnits, 0.0f );
        m_Pitch      = kPitchAboveHorizon;
        m_Yaw        = 3.0f * glm::pi<float>() / 4.0f;
        m_Distance   = 100.0f; // 1 m — the pivot sits just in front of the eye

        m_Position                  = m_FocalPoint - GetForwardDirection() * m_Distance + m_LocationDelta;
        const glm::quat orientation = GetOrientation();

        m_Direction  = glm::eulerAngles( orientation ) * ( 180.f / glm::pi<float>() );
        m_ViewMatrix = glm::translate( glm::mat4( 1.0 ), m_Position ) * glm::toMat4( orientation );
        m_ViewMatrix = glm::inverse( m_ViewMatrix );
    }

    EditorCamera::EditorCamera( const glm::mat4& projectionMatrix )
    {
        m_ProjectionMatrix = projectionMatrix;
        m_Direction        = glm::vec3( 90.0f, 0.0f, 0.0f );
        m_FocalPoint       = glm::vec3( 0.0f );

        const glm::vec3 position = { -500, 500, 500 };
        m_Distance              = glm::distance( position, m_FocalPoint );

        m_Yaw   = 3.0f * glm::pi<float>() / 4.0f;
        m_Pitch = glm::pi<float>() / 4.0f;

        m_Position = m_FocalPoint - GetForwardDirection() * m_Distance;
        UpdateCameraView();
    }

    void EditorCamera::OnEvent( Common::Event& e )
    {
        Common::EventManager eventManager( e );
        eventManager.Notify<Common::KeyPressedEvent>( [this]( Common::KeyPressedEvent& e )
                                                      { return this->OnKeyPress( e ); } );

        eventManager.Notify<Common::MouseMovedEvent>( [this]( Common::MouseMovedEvent& e )
                                                      { return this->OnMouseMove( e ); } );
    }

    bool EditorCamera::OnKeyPress( Common::KeyPressedEvent& /*e*/ )
    {
        return false;
    }

    bool EditorCamera::OnMouseMove( Common::MouseMovedEvent& /*e*/ )
    {
        return false;
    }

    void EditorCamera::UpdateProjectionMatrix( const uint32_t width, const uint32_t height )
    {
        m_ViewportWidth  = width;
        m_ViewportHeight = height;

        const float hpx    = static_cast<float>( height == 0 ? 1u : height );
        const float aspect = static_cast<float>( width ) / hpx;

        // Anchor apparent object SIZE to a reference height: world-per-pixel stays constant as the viewport
        // resizes, so growing/shrinking the window shows MORE/less of the scene instead of zooming objects
        // (UE/Unity editor feel, and it kills the "objects move closer/farther on resize" complaint). FOV and
        // OrthoSize are authored at kReferenceHeight; at other heights the effective extent scales with hpx.
        constexpr float kReferenceHeight = 1080.0f;
        const float     heightScale      = hpx / kReferenceHeight;

        if ( m_ProjectionType == ProjectionType::Orthographic )
        {
            const float halfH  = m_OrthoSize * heightScale;
            const float halfW  = halfH * aspect;
            m_ProjectionMatrix = MakeOrthographic( -halfW, halfW, -halfH, halfH, m_NearPlane, m_FarPlane );
        }
        else
        {
            const float fovY = 2.0f * glm::atan( glm::tan( glm::radians( m_FOV ) * 0.5f ) * heightScale );
            m_ProjectionMatrix = MakePerspective( fovY, aspect, m_NearPlane, m_FarPlane );
        }
    }

    void EditorCamera::OnUpdate( const Common::Timestep& timestep )
    {
        const glm::vec2& MousePosition{ Input::Mouse::Get().GetMouseX(), Input::Mouse::Get().GetMouseY() };
        const glm::vec2  MouseDelta = ( MousePosition - m_InitialMousePosition ) * 0.002f;

        const bool mousePressed = Input::Mouse::Get().IsMouseButtonPressed( Common::MouseButton::Right );

        // An RMB-look only STARTS inside the viewport, but stays active until release so the drag
        // can leave the window. Keyboard movement works whenever the viewport is hovered — no RMB
        // required (UE-style).
        if ( mousePressed && m_InputEnabled )
            m_Flying = true;
        if ( !mousePressed )
            m_Flying = false;

        const bool allowKeyboard = ( m_InputEnabled && !m_KeyboardRequiresLook ) || m_Flying;

        if ( allowKeyboard )
        {
            const float YAWSign = GetUpDirection().y < 0 ? -1.0f : 1.0f;

            // Shift = x4 boost, Ctrl = x0.25 precision crawl.
            float speedScale = 1.0f;
            if ( Input::Keyboard::IsKeyPressed( Common::KeyCode::LeftShift ) ||
                 Input::Keyboard::IsKeyPressed( Common::KeyCode::RightShift ) )
                speedScale *= 4.0f;
            if ( Input::Keyboard::IsKeyPressed( Common::KeyCode::LeftControl ) ||
                 Input::Keyboard::IsKeyPressed( Common::KeyCode::RightControl ) )
                speedScale *= 0.25f;

            const float cameraSpeed   = 0.02f * m_MovementSpeed * speedScale * timestep.GetMilliseconds();
            const float rotationSpeed = 0.133f * timestep.GetMilliseconds();

            if ( Input::Keyboard::IsKeyPressed( Common::KeyCode::S ) )
                m_LocationDelta -= cameraSpeed * m_Direction;
            if ( Input::Keyboard::IsKeyPressed( Common::KeyCode::W ) )
                m_LocationDelta += cameraSpeed * m_Direction;
            if ( Input::Keyboard::IsKeyPressed( Common::KeyCode::A ) )
                m_LocationDelta -= cameraSpeed * m_RightDirection;
            if ( Input::Keyboard::IsKeyPressed( Common::KeyCode::D ) )
                m_LocationDelta += cameraSpeed * m_RightDirection;
            if ( Input::Keyboard::IsKeyPressed( Common::KeyCode::Q ) )
                m_LocationDelta -= cameraSpeed * glm::vec3{ 0.f, YAWSign, 0.f };
            if ( Input::Keyboard::IsKeyPressed( Common::KeyCode::E ) )
                m_LocationDelta += cameraSpeed * glm::vec3{ 0.f, YAWSign, 0.f };

            // Arrow keys rotate without touching the mouse: left/right = yaw, up/down = pitch.
            constexpr float arrowRate = 0.0022f;
            const float     arrowStep = arrowRate * rotationSpeed;
            if ( Input::Keyboard::IsKeyPressed( Common::KeyCode::Left ) )
                m_YawDelta -= arrowStep;
            if ( Input::Keyboard::IsKeyPressed( Common::KeyCode::Right ) )
                m_YawDelta += arrowStep;
            if ( Input::Keyboard::IsKeyPressed( Common::KeyCode::Up ) )
                m_PitchDelta -= arrowStep;
            if ( Input::Keyboard::IsKeyPressed( Common::KeyCode::Down ) )
                m_PitchDelta += arrowStep;
        }

        if ( m_Flying )
        {
            const float     YAWSign       = GetUpDirection().y < 0 ? -1.0f : 1.0f;
            const float     rotationSpeed = 0.133f * timestep.GetMilliseconds();
            constexpr float maxRate       = 0.12f;
            m_YawDelta   += glm::clamp( YAWSign * MouseDelta.x * rotationSpeed, -maxRate, maxRate );
            m_PitchDelta += glm::clamp( MouseDelta.y * rotationSpeed, -maxRate, maxRate );
        }

        m_InitialMousePosition = MousePosition;

        m_Position += m_LocationDelta;
        m_Yaw      += m_YawDelta;
        m_Pitch    += m_PitchDelta;

        // Clamp pitch BEFORE computing the view matrix — exceeding ±90° makes the lookAt target parallel to
        // the up vector, producing a degenerate matrix that collapses all vertices to one clip position.
        //
        // THE LIMIT IS NAMED IN ITS OWN HEADER, and not because this line needed tidying: it also decides
        // how far off-axis a camera put on the Top or Bottom viewport preset ends up, and the code that
        // has to recognise that camera was carrying its own copy of the number. Read CameraPitchLimit.hpp.
        m_Pitch = glm::clamp( m_Pitch, -kMaxCameraPitch, kMaxCameraPitch );

        if ( m_Flying )
        {
            const float distance = glm::distance( m_FocalPoint, m_Position );
            m_FocalPoint         = m_Position + GetForwardDirection() * distance;
            m_Distance           = distance;
        }

        UpdateCameraView();
    }

    glm::quat EditorCamera::GetOrientation() const
    {
        return glm::quat( glm::vec3( -m_Pitch, -m_Yaw, 0.0f ) );
    }

    glm::vec3 EditorCamera::GetUpDirection() const
    {
        return glm::rotate( GetOrientation(), glm::vec3( 0.0, 1.0, 0.0 ) );
    }

    glm::vec3 EditorCamera::GetRightDirection() const
    {
        return glm::rotate( GetOrientation(), glm::vec3( 1.0, 0.0, 0.0 ) );
    }

    glm::vec3 EditorCamera::GetForwardDirection() const
    {
        return glm::rotate( GetOrientation(), glm::vec3( 0.0, 0.0, -1.0 ) );
    }

    void EditorCamera::SnapToDirection( const glm::vec3& forward )
    {
        if ( glm::length( forward ) < 1e-5f )
            return;
        const glm::vec3 f = glm::normalize( forward );

        // Invert this camera's forward model  f = ( sin(yaw)cos(pitch), -sin(pitch), -cos(yaw)cos(pitch) ).
        m_Pitch      = glm::asin( glm::clamp( -f.y, -1.0f, 1.0f ) );
        m_Yaw        = std::atan2( f.x, -f.z );
        m_YawDelta   = 0.0f;
        m_PitchDelta = 0.0f;

        // Keep the current framing distance; orbit the position onto the new direction.
        const float dist = glm::max( glm::distance( m_Position, m_FocalPoint ), 1.0f );
        m_Position       = m_FocalPoint - GetForwardDirection() * dist;
        UpdateCameraView();
    }

    void EditorCamera::Focus( const glm::vec3& point, float distance )
    {
        // Keep the current orientation — only re-center and re-frame (matches Unity/Godot 'F').
        m_FocalPoint = point;
        m_Position   = point - GetForwardDirection() * glm::max( distance, 50.0f );
        UpdateCameraView();
    }

    void EditorCamera::UpdateCameraView()
    {
        const float     YAWsign       = GetUpDirection().y > 0 ? 1 : -1;
        const glm::vec3 lookDirection = m_Position + GetForwardDirection();
        m_Direction                   = glm::normalize( GetForwardDirection() );
        m_Distance                    = glm::distance( lookDirection, m_FocalPoint );
        m_RightDirection              = glm::cross( m_Direction, glm::vec3{ 0.f, YAWsign, 0.f } );

        m_ViewMatrix = glm::lookAt( m_Position, lookDirection, glm::vec3{ 0.f, YAWsign, 0.f } );

        // Damping
        m_YawDelta *= 0.6f;
        m_PitchDelta *= 0.6f;
        m_LocationDelta *= 0.8f;
    }

    // ─── GameplayCamera (driven by a CameraComponent) ───────────────────────────
    void GameplayCamera::SetFromTransform( const glm::vec3& position, const glm::vec3& eulerRotation,
                                           float fovDegrees, float nearPlane, float farPlane, uint32_t width,
                                           uint32_t height )
    {
        m_Position  = position;
        m_FOV       = fovDegrees;
        m_NearPlane = nearPlane;
        m_FarPlane  = farPlane;

        const glm::quat orientation = glm::quat( eulerRotation );
        const glm::vec3 forward     = glm::rotate( orientation, glm::vec3( 0.0f, 0.0f, -1.0f ) );
        const glm::vec3 up          = glm::rotate( orientation, glm::vec3( 0.0f, 1.0f, 0.0f ) );
        m_ViewMatrix                = glm::lookAt( position, position + forward, up );

        UpdateProjectionMatrix( width, height );
    }
} // namespace Desert::Core
