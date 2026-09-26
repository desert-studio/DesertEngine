#pragma once

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    // Directional-shadow debug visualization (CSM). Off = normal lit; ShadowFactor = raw grayscale shadow
    // term; Cascades = tint each fragment by the cascade that shadows it (red/green/blue/yellow front→back).
    enum class ShadowDebugMode : int
    {
        Off = 0,
        ShadowFactor,
        Cascades,
    };

    // Deferred G-buffer debug visualization (UE-style buffer view). Off = normal lit; the others fill the
    // screen with a raw G-buffer channel so the deferred passes can be inspected. Only used in the Deferred path.
    enum class DeferredDebugMode : int
    {
        Off                = 0,
        Albedo             = 1,
        Normal             = 2,
        Metallic           = 3,
        Roughness          = 4,
        AO                 = 5,
        GI                 = 6, // indirect light only (whichever GIMode is active) — for judging GI in isolation
        LightComplexity    = 7, // per-pixel count of point/spot light volumes, heat-mapped (UE-style)
        Overdraw           = 8, // additive re-raster of all meshes -> heat-mapped overdraw count (both paths)
        MaterialComplexity = 9, // per-pixel sampled-texture count (from GBufferC.w), heat-mapped (UE-style)
    };

    // WHAT A VIEW IS SHOWING ON TOP OF THE WORLD — and the whole point of this struct is that it is NOT a
    // property of the world.
    //
    // Every field below used to be a reflected, serialized member of Core::SceneSettings, so the answer to
    // "is the grid on?" travelled through git in the level file. That is not a hypothetical cost; it was
    // measured over the eighty-three .desce files in this repository before К2 removed them:
    //
    //   * `ShowColliders` defaulted to TRUE and was written `true` in 55 of the 73 scenes that stated it.
    //     Green physics wireframes over everybody's viewport, shipped in the level, because at some point
    //     somebody left the toggle on and pressed Ctrl+S.
    //   * `ShowGrid` was stated by 77 scenes and was `false` in 72 of them — one person's view preference,
    //     smeared across the corpus, overriding the next person's every time they opened a different file.
    //   * The other six were uniform (`ShowBoundingBoxes` false, `WireframeMode` false, `ShadowDebug` 0,
    //     `DeferredDebug` 0 everywhere) — that is, pure noise in the file, but noise that ONE bad save
    //     would have turned into a scene that opens in wireframe for everyone, forever.
    //
    // So the ownership is: the VIEW owns this, one instance per SceneRenderer, and it is pushed in from
    // outside. The struct is deliberately NOT reflected and NOT serialized — a field added here cannot
    // reach a `.desce`, and Desert/Tests/Engine/SceneDebugFields derives its forbidden-key census from
    // THIS declaration, so adding a field here also extends the guard that keeps it out of scene files.
    //
    // DEFAULTS ARE "SHOW NOTHING", AND THAT IS THE LOAD-BEARING PROPERTY. A SceneRenderer nobody pushes to
    // — the shipping Runtime, the asset-thumbnail renderer, the inspector preview, the photogrammetry
    // preview — renders the lit world and no overlay, without having to remember to turn anything off. The
    // editor pushes its own persisted state (Editor::EditorPreferences::DebugView) into the viewport's
    // renderer every frame, exactly as it already does for the selection outline.
    //
    // WHAT IS PUSHED IS NOT ALWAYS WHAT IS STORED, and the difference has an owner. A viewport MODE — 2D
    // UI editing, which hides the ground grid the way Unity's 2D scene view does — suppresses a flag for
    // as long as the mode is on. That suppression is applied to a COPY of the persisted state on its way
    // in (Editor/Core/ViewportModes.hpp), and never to the persisted state itself: it belongs to one
    // viewport and dies with the toggle, while the stored flag belongs to the user and outlives the
    // session. К10 exists because the two were once the same field, which made every unrelated save of
    // the preference file able to write a transient mode into the user's permanent answer.
    struct DebugViewState
    {
        // The editor's infinite ground grid. Drawn by Editor::Render::EditorGridPass, which is compiled
        // into the Editor and into nothing else — it has never been able to reach a packaged game.
        bool ShowGrid = false;
        // The sky drawn behind the geometry (SkyboxRenderer::SetBackdropVisible). Off only hides the
        // backdrop: the environment still lights the scene, which is what a preview's "Show Environment"
        // means in UE.
        bool ShowSkyBackdrop = true;

        // Green physics-collider wireframes (UE-style authoring aid). Same story: the pass that draws them
        // is Editor::Render::EditorColliderPass and lives only in the Editor target, so the 55 scenes that
        // shipped `ShowColliders: true` were costing every EDITOR viewport a line pass, not the player's.
        bool ShowColliders = false;

        // Per-mesh AABB wireframes, drawn by MeshRenderer through the debug line renderer. Unlike the two
        // above this one IS in the engine, so it would have reached a packaged game had a scene ever
        // shipped it on.
        bool      ShowBoundingBoxes    = false;
        glm::vec3 BoundingBoxColor     = glm::vec3( 0.25f, 0.95f, 0.35f );
        float     BoundingBoxLineWidth = 1.5f;

        // View modes. These are alternatives to "Lit" rather than overlays, and the editor's View Mode
        // dropdown is what keeps them mutually exclusive; the renderer simply honours whatever it is given.
        //
        // WireframeMode keeps the on-disk spelling of the key it replaces, and deliberately: the census
        // in Desert/Tests/Engine/SceneDebugFields derives the set of names a .desce may not state FROM THIS
        // STRUCT, so a member renamed for tidiness would quietly stop guarding the key it came from.
        //
        // It additionally FORCES the forward path (the deferred G-buffer pipeline has no wireframe
        // variant), which is why a scene file was the worst possible owner for it: one saved `true` and the
        // level silently changed rendering path for everyone who opened it.
        bool WireframeMode = false;
        // Per-pixel normal colour (PBR shader branch) and the per-light "where light lands" branch.
        bool ShowNormals   = false;
        bool LightingDebug = false;

        ShadowDebugMode   ShadowDebug   = ShadowDebugMode::Off;
        DeferredDebugMode DeferredDebug = DeferredDebugMode::Off;
    };
} // namespace Desert::Graphic
