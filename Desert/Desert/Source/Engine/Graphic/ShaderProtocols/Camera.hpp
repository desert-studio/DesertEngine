#pragma once

#include <Engine/Graphic/View/ViewFrame.hpp>

#include <glm/glm.hpp>

#include <string>

namespace Desert::Graphic::ShaderProtocols
{
    // The C++ twin of Common/CameraUB.glslh — the shader view of the per-frame ViewFrame (TAA1 step 3). Laid out
    // exactly as std140 places the GLSL block (mat4 at 64-byte steps, vec2s together, each vec3 completed by a
    // float, explicit tail pads), so sizeof equals the block size and no member has implicit padding;
    // Desert/Tests/Engine/TemporalViewContract/camera_ub_layout_test.cpp holds every offset against the .glslh.
    //
    // WHICH MATRIX (ViewFrame.hpp): raster geometry -> JitteredViewProjection; world position from depth ->
    // InvJitteredViewProjection; reprojection -> PrevViewProjection vs ViewProjection (both unjittered).
    // Projection / View stay the camera's own, unjittered (the vertex shaders still read them, TAA1-B step 5
    // moves geometry to JitteredViewProjection).
    struct Camera
    {
        inline const static std::string Name = "CameraUB";

        glm::mat4 Projection{ 1.0f };                // offset   0
        glm::mat4 View{ 1.0f };                      // offset  64
        glm::mat4 JitteredViewProjection{ 1.0f };    // offset 128
        glm::mat4 ViewProjection{ 1.0f };            // offset 192
        glm::mat4 PrevViewProjection{ 1.0f };        // offset 256
        glm::mat4 InvJitteredViewProjection{ 1.0f }; // offset 320
        glm::vec2 JitterNdc{ 0.0f };                 // offset 384
        glm::vec2 PrevJitterNdc{ 0.0f };             // offset 392
        glm::vec3 CameraPos{ 0.0f };                 // offset 400, world cm
        float     Time = 0.0f;                       // offset 412, seconds (ViewFrame::TimeSeconds)
        glm::vec3 PrevCameraPos{ 0.0f };             // offset 416
        float     PrevTime        = 0.0f;            // offset 428
        float     MaterialMipBias = 0.0f;            // offset 432
        float     Pad0            = 0.0f;            // offset 436 (CameraPad0 in the block)
        float     Pad1            = 0.0f;            // offset 440
        float     Pad2            = 0.0f;            // offset 444; block size 448
    };

    // THE one writer of the camera block: every material that declares CameraUB is filled from the frame's
    // ViewFrame through this, never field by field from a Core::Camera.
    [[nodiscard]] inline Camera MakeCameraUB( const ViewFrame& frame )
    {
        Camera ub;
        ub.Projection                = frame.Projection;
        ub.View                      = frame.View;
        ub.JitteredViewProjection    = frame.JitteredViewProjection;
        ub.ViewProjection            = frame.ViewProjection;
        ub.PrevViewProjection        = frame.PrevViewProjection;
        ub.InvJitteredViewProjection = frame.InvJitteredViewProjection;
        ub.JitterNdc                 = frame.JitterNdc;
        ub.PrevJitterNdc             = frame.PrevJitterNdc;
        ub.CameraPos                 = frame.CameraPosition;
        ub.Time                      = static_cast<float>( frame.TimeSeconds );
        ub.PrevCameraPos             = frame.PrevCameraPosition;
        ub.PrevTime                  = static_cast<float>( frame.PrevTimeSeconds );
        ub.MaterialMipBias           = frame.MaterialMipBias;
        return ub;
    }
} // namespace Desert::Graphic::ShaderProtocols
