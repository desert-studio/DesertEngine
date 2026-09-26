#pragma once

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>

// WHETHER A PREVIEW RENDERS THIS FRAME.
//
// A Material Editor preview is a second full scene render (deferred lighting, sky, post) at the size of the
// pane. Rendered every editor frame it took ~70 % of the frame on the ME1c capture (76 FPS) while the picture
// was not changing at all. Unreal's material preview is the same: it redraws when something it shows changes,
// and per frame only while "Realtime" is switched on.
//
// The rule re-renders when anything the picture is made of differs from what the last render was made of —
// the subject (material + its parameter fingerprint), the shape, the lighting setup, the orbit camera, the
// pane size — and for kSettleFrames after that, because a render is not converged on its first frame (the
// environment bake, per-frame-in-flight state). Time-dependent content (a sky volume still baking) and the
// Realtime toggle render every frame. Anything else reuses the last image: the tonemap/AA target a preview
// shows is a persistent framebuffer (TonemapRenderer::Initialize), not a per-frame transient.
//
// Kept apart from the widget so the rule is asserted (Desert/Tests/Editor/PreviewInput).
namespace Desert::Editor::PreviewRenderGate
{
    inline constexpr uint32_t kSettleFrames = 16;

    // Everything the preview picture is a function of. Compared as a whole; a field added here is covered.
    struct Inputs
    {
        uint64_t  Subject    = 0; // material handle / mesh handle the preview draws
        uint64_t  Parameters = 0; // fingerprint of the subject's parameter values and its shader rebuild count
        uint64_t  Setup      = 0; // fingerprint of the preview scene setup (shape, sky preset, sun, floor, ...)
        float     Yaw        = 0.0f;
        float     Pitch      = 0.0f;
        float     Zoom       = 0.0f;
        glm::vec3 Focus{ 0.0f };
        uint32_t  Width  = 0;
        uint32_t  Height = 0;

        bool operator==( const Inputs& ) const = default;
    };

    struct State
    {
        bool     Rendered = false; // an image exists
        Inputs   Last;             // what it was rendered from
        uint32_t SettleLeft = 0;   // frames still to render after the last change
    };

    /**
     * @brief Decides this frame and advances @p state. Returns true when the preview must render.
     * @param timeDependent the preview content changes on its own (a sky volume baking, animated content).
     * @param realtime the toolbar's Realtime toggle: render every frame.
     */
    [[nodiscard]] inline bool ShouldRender( State& state, const Inputs& now, const bool timeDependent,
                                            const bool realtime ) noexcept
    {
        if ( !state.Rendered || !( now == state.Last ) )
        {
            state.Rendered   = true;
            state.Last       = now;
            state.SettleLeft = kSettleFrames;
            return true;
        }
        if ( realtime || timeDependent )
            return true;
        if ( state.SettleLeft > 0 )
        {
            --state.SettleLeft;
            return true;
        }
        return false;
    }

    // FNV-1a over raw bytes, for the two fingerprints. Stable within a run, which is all a change test needs.
    [[nodiscard]] inline uint64_t Fingerprint( const void* data, const std::size_t size,
                                               uint64_t seed = 1469598103934665603ull ) noexcept
    {
        const auto* bytes = static_cast<const unsigned char*>( data );
        for ( std::size_t i = 0; i < size; ++i )
        {
            seed ^= bytes[i];
            seed *= 1099511628211ull;
        }
        return seed;
    }

    // How many materials a chain may hold, the subject included: MaterialService's own cap on the walk that
    // builds the drawn instance (CreateRuntimeInstance, ResolveOverrides), so the key covers exactly the
    // levels the picture is made from and a cyclic chain cannot loop.
    inline constexpr int kMaxMaterialChain = 8;

    /**
     * @brief The Parameters fingerprint of a subject drawn THROUGH ITS PARENT CHAIN: its own values, then each
     * ancestor's, nearest first. A material instance's picture is its parent's values under its overrides, so
     * a key over the instance's own values alone kept an edit applied to the parent off the instance's pane.
     * The ancestors' asset values are hashed rather than the runtime PropertyVersion stamps because only a
     * BASE material has a runtime Material whose properties are stamped; an instance parent has none (its
     * overrides are baked into cached instances, MaterialEditorPanel's Apply), so its edits carry no version.
     * @param valuesOf (const Node&, uint64_t seed) -> uint64_t: folds one material's own values into seed.
     * @param parentOf (const Node&) -> pointer-like to the resolved parent, null at the base.
     */
    template <typename Node, typename ValuesOf, typename ParentOf>
    [[nodiscard]] uint64_t ChainFingerprint( const Node& subject, const uint64_t seed, const ValuesOf& valuesOf,
                                             const ParentOf& parentOf )
    {
        uint64_t digest = valuesOf( subject, seed );
        auto     parent = parentOf( subject );
        for ( int depth = 1; parent && depth < kMaxMaterialChain; ++depth )
        {
            digest = valuesOf( *parent, digest );
            parent = parentOf( *parent );
        }
        return digest;
    }
} // namespace Desert::Editor::PreviewRenderGate
