#pragma once

#include "RenderGraphBuilder.hpp"

namespace Desert::Graphic
{
    class IRenderSystem
    {
    public:
        virtual ~IRenderSystem() = default;

        virtual void RegisterPasses( RenderGraphBuilder& builder ) = 0;

        /**
         * @brief THE WORLD THIS SYSTEM HAS BEEN ACCUMULATING OVER HAS BEEN REPLACED.
         *
         * A different scene — or a cleared one — is now bound to the SceneRenderer that owns this system.
         * Called from SceneRenderer::RebindScene, AFTER a WaitDeviceIdle, so an override may release GPU
         * objects the last submitted frame was still reading.
         *
         * IT IS NOT A GENERAL "RESET YOURSELF", and the narrowness is the whole point. Render systems
         * outlive the scene they were built for — they are constructed from a SceneRenderer* and a
         * Framebuffer and never see a Scene at all — and two properties are what make that legal:
         *
         *   * every per-frame input is RESTATED by its producer each frame, absence included. The
         *     canonical example is SkyboxECSSystem, which emits an explicit "no sky" command rather than
         *     emitting nothing, precisely so a renderer that keeps its state cannot keep a deleted one;
         *   * everything expensive enough to be cached ACROSS frames is keyed on a fingerprint of the
         *     content it was built from, never on "have I built one" — the cloud modelling volume, the sky
         *     IBL, the atmosphere LUTs.
         *
         * State that is neither is what this hook is for, and there are exactly two kinds of it:
         *
         *   1. A TEMPORAL HISTORY. Reprojecting the previous frame is correct only while the previous
         *      frame showed the same world; across a scene change it ghosts a world that is gone.
         *   2. A PER-ENTITY GPU RESOURCE cached by identity. A fresh registry re-issues entity ids from
         *      zero, so the next scene's first emitter is the previous scene's first emitter as far as a
         *      cache keyed on the id can tell.
         *
         * Anything else wanting an override is a producer that is not restating itself, or a cache with
         * the wrong key. Fix that instead — it is also broken mid-session, where this hook never runs.
         *
         * Default: nothing, which is the honest answer for most systems and is asserted rather than
         * assumed — Desert/Tests/Engine/RendererSceneLifetime names every system that overrides it and why.
         */
        virtual void OnSceneReplaced()
        {
        }

        /**
         * @brief THE VIEW'S TEMPORAL HISTORY IS CUT: THE NEXT FRAME IS THE FIRST ONE OF A NEW SEQUENCE.
         *
         * The camera-cut hook (UE: FSceneViewState / bCameraCut). The world is the SAME one — nothing is
         * released and the device is not idled — but nothing this system integrated over earlier frames may
         * reach the next one: the frame index that drives jitter, noise seeds and history ping-pong starts
         * again from zero, reprojected histories are invalidated, adaptations snap, simulations restart from
         * their authored state.
         *
         * Called from SceneRenderer::ResetTemporalHistory, between frames. Callers are anyone who needs a
         * frame sequence that does not depend on what was rendered before it — a capture that has to be the
         * same picture on every run is the first one; a camera cut or a teleport is the same request.
         *
         * WHAT IS RESET IS STATE, NEVER PASSES. When histories become render-graph external resources, this
         * zeroes their state (history-valid flags, counters); it does not register a pass or touch the graph.
         *
         * Default: nothing. Desert/Tests/Engine/RendererSceneLifetime names every override.
         */
        virtual void OnTemporalHistoryReset()
        {
        }
    };
} // namespace Desert::Graphic