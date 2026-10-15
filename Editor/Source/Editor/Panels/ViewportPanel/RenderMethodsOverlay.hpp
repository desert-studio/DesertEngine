#pragma once

#include <imgui.h>

namespace Desert::Graphic
{
    struct RenderMethods;
}

namespace Desert::Graphic::RayTracing
{
    struct RayTracingSceneStats;
    struct LandscapeRayTracingFrame;
} // namespace Desert::Graphic::RayTracing

namespace Desert::Editor
{
    // THE VIEW'S RENDER METHODS, in its bottom-left corner: what this viewport's frame renders for GI, reflections
    // and shadows (Graphic::ResolveRenderMethods), one row each. A row lowered below the view's request (by the
    // Scalability ceiling, the device or an unimplemented method) is drawn amber with its cause — the same text
    // the log got once (Graphic::FormatRenderMethods), so the overlay and the log cannot disagree.
    void DrawRenderMethodsOverlay( const Graphic::RenderMethods& methods, const ImVec2& viewportMin,
                                   const ImVec2& viewportMax );

    // THE VIEW'S RAY-TRACING SCENE, in its bottom-right corner, only on a frame that built a TLAS
    // (RayTracingSceneStats::Built): TLAS instances per primitive kind, queued primitives the rays cannot see per
    // reason (amber when non-zero), and the bytes of the BLASes the TLAS traces, the scene's variant / procedural
    // / skin-cache BLASes, the skin cache's positions, the TLAS and its instance rows; @p landscape (the view's
    // SceneRenderer::GetLandscapeRayTracingFrame) adds the landscape tiles: count, built / refit this frame, their
    // vertex + index buffers and their BLASes.
    void DrawRayTracingSceneOverlay( const Graphic::RayTracing::RayTracingSceneStats&     stats,
                                     const Graphic::RayTracing::LandscapeRayTracingFrame& landscape,
                                     const ImVec2& viewportMin, const ImVec2& viewportMax );
} // namespace Desert::Editor
