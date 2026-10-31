#pragma once

#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>

#include <glm/glm.hpp>

#include <cstddef>

// THE RAY TRACING VIEW BLOCK (GRASS-VIS-19; UE: the View uniform buffer every ray tracing shader of a view reads):
// what a hit group knows of the VIEW whose rays it shades, once per frame - never a copy in every primitive's
// shader-record row, so a primitive's row does not change when the camera moves. Made by the frame's owner
// (SceneRenderer::AddFrameRayTracingView, one upload a frame whichever ray generations trace), bound by every ray
// generation that traces into hit groups (PathTracer "PathTracing: Trace", SSR "Deferred: Reflections
// (HitLighting)"). GLSL twin: Common/RayTracingView.glslh RayTracingViewUB (std140) at Core::kRayTracingViewSet /
// kRayTracingViewBinding.
namespace Desert::Graphic::RayTracing
{
    struct RayTracingViewBlock
    {
        // xyz = the camera the raster drew the frame for (cm): a landscape hit's view distance (far tiling, grass
        // cover) is measured from it, as the raster's.
        glm::vec4 ViewOrigin{ 0.0f };
        // The view's grass field lens (TerrainUB::GrassLens: x DistanceScale, y PixelsPerUnit;
        // TerrainRenderer::GetFrameGrassLens), the lens LandscapeGrassCoverAt lays the far cover through.
        glm::vec4 GrassLens{ 0.0f };
    };
    static_assert( sizeof( RayTracingViewBlock ) == 2u * sizeof( glm::vec4 ) &&
                        offsetof( RayTracingViewBlock, GrassLens ) == sizeof( glm::vec4 ),
                   "RayTracingViewBlock must match Common/RayTracingView.glslh RayTracingViewUB (std140, 32 B)" );

    // The slot name every hit-group-tracing pass binds, spelled once (RDGShaderParameters.hpp).
    struct RayTracingViewShaderParameters
    {
        DESERT_SHADER_PARAMETER( UniformBuffer, RayTracingViewUB );
    };
} // namespace Desert::Graphic::RayTracing
