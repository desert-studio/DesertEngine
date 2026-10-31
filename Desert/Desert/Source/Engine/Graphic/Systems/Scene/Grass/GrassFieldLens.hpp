#pragma once

// The grass field's per-view distances (GRASS-VIS-16): the Foliage quality and the lens that turn a grass
// variety's authored Start / EndCullDistance and its screen-width floor into this view's - one statement read by
// the grass generator (GrassRenderer) and by the ground's far cover (ViewFrame::GrassLens -> TerrainUB::GrassLens
// for the raster, CameraUB::GrassLens for a ray tracing hit).

namespace Desert::Graphic
{
    struct ViewFrame;
}

namespace Desert::Graphic::System
{
    // THE VIEW'S FOLIAGE QUALITY (GRASS-DENSITY; UE sg.FoliageQuality: grass.DensityScale multiplies every
    // variety's GrassDensity, grass.CullDistanceScale its Start and End cull distances), from the Foliage
    // scalability group (Scalability.json; SceneRenderer::BeginScene -> GrassRenderer::SetQuality and the scene
    // view's ViewFrame::GrassLens). Fractions: 1 = the asset as authored. A machine's quality, never a
    // scene's or an asset's: the asset states the density and the distances, the level only scales them.
    struct GrassQuality
    {
        float              DensityScale      = 1.0f; // Foliage.GrassDensityScalePercent / 100, 0.1..1
        float              CullDistanceScale = 1.0f; // Foliage.GrassCullDistanceScalePercent / 100, 0.1..4
        [[nodiscard]] bool operator==( const GrassQuality& ) const = default;
    };

    // THE GRASS FIELD'S DISTANCES THROUGH ONE VIEW (GRASS-VIS-16): what turns a variety's authored distances into
    // the ones its blades are thinned by in this view. Read by the generator's parameters (GrassRenderer::
    // PrepareVariety: the screen-width floor's D) and by the ground's far cover (TerrainRenderer: the frame's
    // TerrainUB::GrassLens over TerrainInstance::GrassCoverRange x / y and GrassCoverFloorWidth - view data, never
    // the tile's row, GRASS-VIS-17), so the cover takes over from the same min(S,
    // D) the blades and their cascades thin from (GpuInstanceBladeShadowLod::StartCull) - under the same Foliage
    // quality and the same lens.
    struct GrassFieldLens
    {
        // grass.CullDistanceScale x the lens's DrawDistanceScale: an authored Start / EndCullDistance -> this
        // view's (GrassRenderer: ScaledPlacement, then x DrawDistanceScale).
        float DistanceScale = 1.0f;
        // Render pixels per unit of x per unit of depth, |Projection[0][0]| x Render.Width / 2; 0 = the view draws
        // no screen-width floor (an orthographic lens, no render extent).
        float PixelsPerUnit = 0.0f;
    };
    [[nodiscard]] GrassFieldLens MakeGrassFieldLens( const GrassQuality& quality, const ViewFrame& frame );

    // The screen-width floor's distance D (GRASS-WIDTH, cm; 0 = none): where a mean blade @p meanWidth cm wide
    // stands @p minScreenWidth render pixels wide through @p lens. Past it the generator keeps (D / d)^2 of the
    // blades and lifts each kept card to the floor width (GrassGenerate.shader, GrassFieldShare.glslh).
    [[nodiscard]] inline float GrassFloorDistance( const float meanWidth, const float minScreenWidth,
                                                   const GrassFieldLens& lens )
    {
        return minScreenWidth > 0.0f && meanWidth > 0.0f && lens.PixelsPerUnit > 0.0f
                    ? meanWidth * lens.PixelsPerUnit / minScreenWidth
                    : 0.0f;
    }
} // namespace Desert::Graphic::System
