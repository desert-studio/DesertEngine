#include <Engine/Core/Camera.hpp>
#include <Engine/Core/CameraPitchLimit.hpp>
#include <Engine/Core/EditorCameraBasis.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/Input.hpp>
#include <Engine/Core/Projection.hpp>

#include <glm/gtc/quaternion.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

#include <cmath>

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

        m_Position                  = m_FocalPoint - CurrentBasis().Forward * m_Distance + m_LocationDelta;
        const glm::quat orientation = OrbitOrientation( m_Yaw, m_Pitch );

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

        m_Position = m_FocalPoint - CurrentBasis().Forward * m_Distance;
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
        if ( m_ExactLens )
        {
            // A piloted camera entity: the lens is the component's, so the projection is the one a
            // GameplayCamera builds and FOV is not rescaled by the viewport's height.
            Camera::UpdateProjectionMatrix( width, height );
            return;
        }

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
            const float YAWSign = CurrentBasis().Up.y < 0 ? -1.0f : 1.0f;

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
            const float     YAWSign       = CurrentBasis().Up.y < 0 ? -1.0f : 1.0f;
            const float     rotationSpeed = 0.133f * timestep.GetMilliseconds();
            constexpr float maxRate       = 0.12f;
            m_YawDelta   += glm::clamp( YAWSign * MouseDelta.x * rotationSpeed, -maxRate, maxRate );
            m_PitchDelta += glm::clamp( MouseDelta.y * rotationSpeed, -maxRate, maxRate );
        }

        m_InitialMousePosition = MousePosition;

        m_Position += m_LocationDelta;

        // ── LEAVING AN AXIS VIEW IS WHAT TURNING MEANS ────────────────────────────────────────────
        //
        // A rotation arrived, so the user is orbiting and the exact basis is given up HERE — before the
        // angles are advanced, so the drag starts from where the axis view pointed. Moving the camera
        // (the m_LocationDelta above) is deliberately not a reason: panning across a plan view must not
        // tilt it, which is the whole difference between a plan and a high camera.
        //
        // The deltas are exactly zero while nothing rotates: SnapToAxisView zeroes them, the damping
        // below multiplies zero by 0.6, and both input sites add nothing when the mouse has not moved.
        if ( m_AxisView && ( m_YawDelta != 0.0f || m_PitchDelta != 0.0f ) )
            LeaveAxisView();

        if ( !m_AxisView )
        {
            m_Yaw += m_YawDelta;
            m_Pitch += m_PitchDelta;

            // Clamp pitch BEFORE computing the view matrix — exceeding ±90° makes the lookAt target
            // parallel to the up vector, producing a degenerate matrix that collapses all vertices to one
            // clip position.
            //
            // THE LIMIT IS NAMED IN ITS OWN HEADER (CameraPitchLimit.hpp) and applied through its one
            // function (EditorCameraBasis.hpp), and it is UNCHANGED: widening it would mean choosing a
            // second up vector at the poles for every orbiting viewport in the editor. What changed is
            // that the two views which cannot be expressed as an orbit angle no longer try to be one —
            // they are held as a basis and never reach this line at all.
            m_Pitch = ClampOrbitPitch( m_Pitch );
        }

        if ( m_Flying )
        {
            const float distance = glm::distance( m_FocalPoint, m_Position );
            m_FocalPoint         = m_Position + CurrentBasis().Forward * distance;
            m_Distance           = distance;
        }

        UpdateCameraView();
    }

    // THE FOUR PRIVATE DIRECTION ACCESSORS ARE GONE AND ARE NOT MISSED. Each of them read m_Yaw/m_Pitch
    // directly, which is exactly the thing that must not happen while an axis view is held — the angles
    // are stale then by construction. CurrentBasis() is the one reader, and Engine/Core/EditorCameraBasis.hpp
    // holds the orbit arithmetic itself so that a suite can compile it without a device.
    ViewBasis EditorCamera::CurrentBasis() const
    {
        if ( m_AxisView )
            return *m_AxisView;

        // THE ORBIT, SPELLED EXACTLY AS IT WAS. The up vector is world up flipped by the sign of the
        // orbit's own up — which is what `glm::lookAt` was handed before this function existed, and the
        // reason the clamp is needed: at ±90° that vector is parallel to the forward direction.
        const float     YAWsign = OrbitUp( m_Yaw, m_Pitch ).y > 0 ? 1.0f : -1.0f;
        const glm::vec3 forward = glm::normalize( OrbitForward( m_Yaw, m_Pitch ) );
        // A piloted, rolled camera keeps its roll while it orbits. Zero roll takes the old spelling
        // exactly, so every viewport that never pilots is untouched to the bit.
        if ( m_Roll != 0.0f )
            return ViewBasis{ forward, UpWithRoll( forward, m_Roll ) };
        return ViewBasis{ forward, glm::vec3{ 0.f, YAWsign, 0.f } };
    }

    void EditorCamera::LeaveAxisView()
    {
        if ( !m_AxisView )
            return;
        // The orbit takes over FROM WHERE THE AXIS VIEW POINTED, so the first degree of a drag off Top
        // starts at Top and not at wherever the yaw/pitch pair was left before the preset was applied.
        // `OrbitAnglesFor` is unclamped; OnUpdate clamps one line later, which is the moment the camera
        // legitimately gives up the exact angle because the user asked it to turn.
        OrbitAnglesFor( m_AxisView->Forward, m_Yaw, m_Pitch );
        m_AxisView.reset();
    }

    void EditorCamera::SnapToDirection( const glm::vec3& forward )
    {
        if ( glm::length( forward ) < 1e-5f )
            return;
        const glm::vec3 f = glm::normalize( forward );

        // THE ORBIT GESTURE, and it stays one: this is the clickable view-axis triad and the `--look`
        // placement, both of which mean "turn the camera to face that way" and are expected to behave
        // like a drag that ended there — including the clamp. An EXACT axis view is a different request
        // with a different entry point (SnapToAxisView).
        m_AxisView.reset();
        m_Roll = 0.0f; // a named direction is an upright one
        OrbitAnglesFor( f, m_Yaw, m_Pitch );
        m_YawDelta   = 0.0f;
        m_PitchDelta = 0.0f;

        // Keep the current framing distance; orbit the position onto the new direction.
        const float dist = glm::max( glm::distance( m_Position, m_FocalPoint ), 1.0f );
        m_Position       = m_FocalPoint - CurrentBasis().Forward * dist;
        UpdateCameraView();
    }

    void EditorCamera::SnapToAxisView( const ViewBasis& basis )
    {
        if ( glm::length( basis.Forward ) < 1e-5f || glm::length( basis.Up ) < 1e-5f )
            return;

        m_AxisView   = ViewBasis{ glm::normalize( basis.Forward ), glm::normalize( basis.Up ) };
        m_Roll       = 0.0f; // the axis view names its own up; leaving it returns to an upright orbit
        m_YawDelta   = 0.0f;
        m_PitchDelta = 0.0f;

        // Same framing as SnapToDirection: a named angle changes where you look FROM, never what you are
        // looking at, which is what makes flipping between Top and Front useful rather than disorienting.
        const float dist = glm::max( glm::distance( m_Position, m_FocalPoint ), 1.0f );
        m_Position       = m_FocalPoint - m_AxisView->Forward * dist;
        UpdateCameraView();
    }

    void EditorCamera::Focus( const glm::vec3& point, float distance )
    {
        // Keep the current orientation — only re-center and re-frame (matches Unity/Godot 'F'). An axis
        // view survives it: framing is not a rotation, and an F-focus that quietly dropped the plan view
        // would be the same defect one level up.
        m_FocalPoint = point;
        m_Position   = point - CurrentBasis().Forward * glm::max( distance, 50.0f );
        UpdateCameraView();
    }

    void EditorCamera::PlaceAt( const glm::vec3& position, const ViewBasis& basis )
    {
        if ( glm::length( basis.Forward ) < 1e-5f || glm::length( basis.Up ) < 1e-5f )
            return;

        const glm::vec3 f = glm::normalize( basis.Forward );
        const glm::vec3 u = glm::normalize( basis.Up - f * glm::dot( basis.Up, f ) );
        m_AxisView        = ViewBasis{ f, u };
        m_Roll            = RollOf( *m_AxisView );

        // Keep the framing distance so F-focus and the orbit pivot behave as they did before piloting.
        const float dist = glm::max( m_Distance, 1.0f );
        m_Position       = position;
        m_FocalPoint     = position + f * dist;
        UpdateCameraView();
    }

    void EditorCamera::SetExactLens( bool exact )
    {
        m_ExactLens = exact;
        if ( m_ViewportWidth > 0 )
            UpdateProjectionMatrix( m_ViewportWidth, m_ViewportHeight );
    }

    EditorCamera::Pose EditorCamera::CapturePose() const
    {
        Pose pose;
        pose.Position   = m_Position;
        pose.FocalPoint = m_FocalPoint;
        pose.Distance   = m_Distance;
        pose.Yaw        = m_Yaw;
        pose.Pitch      = m_Pitch;
        pose.Roll       = m_Roll;
        pose.AxisView   = m_AxisView;
        pose.FOV        = m_FOV;
        pose.NearPlane  = m_NearPlane;
        pose.FarPlane   = m_FarPlane;
        pose.Projection = m_ProjectionType;
        pose.ExactLens  = m_ExactLens;
        return pose;
    }

    void EditorCamera::RestorePose( const Pose& pose )
    {
        m_Position       = pose.Position;
        m_FocalPoint     = pose.FocalPoint;
        m_Distance       = pose.Distance;
        m_Yaw            = pose.Yaw;
        m_Pitch          = pose.Pitch;
        m_Roll           = pose.Roll;
        m_AxisView       = pose.AxisView;
        m_FOV            = pose.FOV;
        m_NearPlane      = pose.NearPlane;
        m_FarPlane       = pose.FarPlane;
        m_ProjectionType = pose.Projection;
        m_ExactLens      = pose.ExactLens;
        m_YawDelta       = 0.0f;
        m_PitchDelta     = 0.0f;
        m_LocationDelta  = glm::vec3( 0.0f );
        if ( m_ViewportWidth > 0 )
            UpdateProjectionMatrix( m_ViewportWidth, m_ViewportHeight );
        UpdateCameraView();
    }

    void EditorCamera::UpdateCameraView()
    {
        const ViewBasis basis         = CurrentBasis();
        const glm::vec3 lookDirection = m_Position + basis.Forward;
        m_Direction                   = basis.Forward;
        m_Distance                    = glm::distance( lookDirection, m_FocalPoint );
        m_RightDirection              = glm::cross( m_Direction, basis.Up );

        m_ViewMatrix = glm::lookAt( m_Position, lookDirection, basis.Up );

        // Damping
        m_YawDelta *= 0.6f;
        m_PitchDelta *= 0.6f;
        m_LocationDelta *= 0.8f;
    }

    // ─── GameplayCamera (driven by a CameraComponent) ───────────────────────────
    void GameplayCamera::SetView( const CameraEntityView& view, uint32_t width, uint32_t height )
    {
        m_Position   = view.Position;
        m_FOV        = view.FovYDegrees;
        m_NearPlane  = view.Near;
        m_FarPlane   = view.Far;
        m_ViewMatrix = ViewMatrixOf( view );
        UpdateProjectionMatrix( width, height );
    }

    void GameplayCamera::SetFromTransform( const glm::vec3& position, const glm::vec3& eulerRotation,
                                           float fovDegrees, float nearPlane, float farPlane, uint32_t width,
                                           uint32_t height )
    {
        const glm::mat4 world =
             glm::translate( glm::mat4( 1.0f ), position ) * glm::toMat4( glm::quat( eulerRotation ) );
        SetView( CameraEntityViewOf( world, fovDegrees, nearPlane, farPlane ), width, height );
    }
} // namespace Desert::Core
