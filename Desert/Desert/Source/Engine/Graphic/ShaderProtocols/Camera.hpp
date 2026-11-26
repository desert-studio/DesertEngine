#pragma once

#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>
#include <Engine/Graphic/ShaderProtocols/CameraLayout.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>

#include <glm/glm.hpp>

#include <type_traits>

// THE CAMERA BLOCK'S WRITER AND BINDING PARAMETER. Its layout, and the generated Common/CameraUB.glslh, are
// CameraLayout.hpp's.
namespace Desert::Graphic::ShaderProtocols
{
    static_assert( std::is_same_v<CameraLocalWindSources, decltype( LocalWindSources::Source )> &&
                        std::is_same_v<CameraLocalWindSpeeds, decltype( LocalWindSources::Speed )>,
                   "CameraUB's LocalWind arrays are the ViewFrame's LocalWindSources arrays" );
    static_assert( kMaxLocalWindSources == 8, "CameraUB holds LocalWind[8] and LocalWindSpeed[2]" );

    /// The camera block as a binding parameter (RDGShaderParameters.hpp), named by the generated block.
    inline constexpr RDG::UniformBufferParameter kCameraUBParameter{ kCameraUBBlock.BlockName };

    /// PURE. CameraUB::PixelToNdc of a raster viewport @p rect (x, y its origin, z, w its size, target pixels),
    /// drawn as the engine draws every view - negative height, +Y up (VulkanRendererAPI::SetViewportRegion,
    /// VulkanRenderGraph's whole-target viewport): ndc.x = 2 (px - x) / w - 1, ndc.y = 1 - 2 (py - y) / h. An
    /// empty rect (a view that draws no G-buffer cell: a light view, a still preview) maps to 0.
    [[nodiscard]] inline glm::vec4 MakePixelToNdc( const glm::vec4& rect ) noexcept
    {
        if ( !( rect.z > 0.0f ) || !( rect.w > 0.0f ) )
            return glm::vec4( 0.0f );
        return glm::vec4( 2.0f / rect.z, -2.0f / rect.w, -1.0f - 2.0f * rect.x / rect.z,
                          1.0f + 2.0f * rect.y / rect.w );
    }

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
        ub.ShadowSlopeBias           = frame.ShadowSlopeBias;
        ub.ShadowSlopeClamp          = frame.ShadowSlopeClamp;
        ub.ParallaxMinSteps          = static_cast<float>( frame.Parallax.MinSteps );
        ub.ParallaxMaxSteps          = static_cast<float>( frame.Parallax.MaxSteps );
        ub.ParallaxShadowSteps       = static_cast<float>( frame.Parallax.ShadowSteps );
        ub.SunDirection              = frame.SunDirection;
        ub.PreExposure               = frame.PreExposure;
        ub.PrevPreExposure           = frame.PrevPreExposure;
        ub.FrameIndexMod8            = static_cast<float>( frame.FrameIndex % 8u );
        ub.CasterWindOrigin          = frame.CasterWindOrigin;
        ub.CasterWindRing            = frame.CasterWindRing;
        ub.GrassLens                 = frame.GrassLens;
        ub.PixelToNdc                = MakePixelToNdc( frame.RasterRect );
        ub.LodTransition             = glm::vec4( frame.TemporalLOD.Transition,
                                                  frame.TemporalLOD.Active ? 1.0f : 0.0f, 0.0f, 0.0f );
        ub.WindGust                  = frame.WindGust;
        ub.PrevWindGust              = frame.PrevWindGust;
        ub.LocalWind                 = frame.LocalWind.Source;
        ub.LocalWindSpeed            = frame.LocalWind.Speed;
        ub.PrevLocalWind             = frame.PrevLocalWind.Source;
        ub.PrevLocalWindSpeed        = frame.PrevLocalWind.Speed;
        return ub;
    }
} // namespace Desert::Graphic::ShaderProtocols
