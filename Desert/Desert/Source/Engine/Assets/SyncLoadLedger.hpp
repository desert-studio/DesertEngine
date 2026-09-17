#pragma once

#include <cstdint>
#include <string>

namespace Desert::Assets
{
    /// WHEN a synchronous load happened, which is the whole point of the detector.
    ///
    /// `Boot` is before the first frame has begun: blocking there is normal and is what a loading screen
    /// is for. `Frame` is after it: blocking there is a hitch the player feels, and it is the failure the
    /// world programme's acceptance criterion names in its own words — "no jerks when moving". The same
    /// call, the same milliseconds, two entirely different verdicts; a counter that did not separate them
    /// would total a legitimate boot cost together with the defect and report neither.
    enum class LoadPhase : uint8_t
    {
        Boot = 0,
        Frame,
    };

    /**
     * @brief ONE POINT THROUGH WHICH EVERY SYNCHRONOUS ASSET LOAD PASSES, with a timer and a phase.
     *
     * WHY THIS IS THE HIGHEST-VALUE DETECTOR IN THE PROGRAMME, in the gap analysis's own assessment
     * (§T0.1, "highest value-per-line item"): every later claim about load time is unfalsifiable without
     * it. `MeshService::Get` parses a cooked mesh on the frame that first touches it — 40 185 992 bytes
     * of JSON for 105 317 vertices — and nothing counted that, so "the preloader is eager and that is
     * the cost" and "the preloader is lazy and the cost moved into the frame" produced the same evidence:
     * none.
     *
     * ── WHERE THE POINT IS, AND WHY IT IS NOT A CALL SITE ─────────────────────────────────────────────
     *
     * The tempting answer was `AssetManager::CreateAsset`, which is where an asset is registered and
     * eagerly loaded. It is not the point: measured on this tree, `AssetBase::Load()` is reached from
     * **45 call sites** outside the tests — hot reload has six, the component registry five, the cloud
     * panels eight, `EnsureLoaded` one, `CreateAsset` one — and a detector at any one of them counts a
     * fraction. Wrapping all 45 by hand is the arrangement `ResourceLedger`'s header refuses by name:
     * "a Register()/Unregister() pair is thirteen places for a fourteenth to be forgotten".
     *
     * So the point is `Load()` ITSELF. `AssetBase::Load()` used to be pure virtual; it is now a
     * NON-VIRTUAL member that times, counts and delegates to a protected `virtual LoadFromFile()`. Every
     * one of the 45 sites is instrumented without being edited, and a nineteenth asset type is
     * instrumented by the act of compiling: it cannot override `Load()` — the `override` keyword no
     * longer applies to it — and `Desert/Tests/Engine/SyncLoadChokepoint` asserts over the source text
     * that no subclass declares one anyway, because a declaration WITHOUT `override` would shadow
     * silently, which is the one remaining way past.
     *
     * ── NESTING, AND WHY THE TOTAL IS NOT THE SUM OF THE PARTS ───────────────────────────────────────
     *
     * Loads nest. `CloudTypeAsset::Load()` reaches into the asset manager mid-load to resolve the noise
     * volume it names, and the prefab loader loads nested prefabs. A timer that added every level's
     * duration would report a boot spending 250 % of its own wall clock. So `Loads` counts every load at
     * every depth — that is the number "how many files did we open" wants — while `TotalMs` accumulates
     * only the OUTERMOST scope, which is the number "how long did loading take" wants. They are
     * deliberately not the same question and deliberately not the same field.
     *
     * ── WHY THERE IS NO COMMAND-LINE SWITCH ──────────────────────────────────────────────────────────
     *
     * §T0.1 modelled this on Lyra's `ShouldLogAssetLoads()`, which is behind one. Ours is not, and the
     * reason is that the flag would put the interesting half behind a default of silence: a load that
     * happens IN A FRAME is the defect, and a defect that only reports itself when someone already
     * suspected it is not a detector. In-frame loads therefore log themselves at WARN, always, capped
     * per frame so that fifty thousand of them cannot drown the log they are the evidence in. Boot loads
     * are counted and totalled, and the total goes out with the memory readout; they are not individually
     * logged, because nothing is learnt from the four-thousandth line of a preload that is working.
     */
    class SyncLoadLedger final
    {
    public:
        /// Tell the ledger the BOOT IS OVER. Idempotent and one-way.
        ///
        /// CALLED BY THE LAYER, NOT BY THE RENDERER, and the distinction is the difference between a
        /// useful detector and a noisy one. The editor presents frames THROUGHOUT its staged boot — that
        /// is what the loading overlay is — so hanging the phase change off `Renderer::BeginFrame` would
        /// mark every preload stage as an in-frame load and bury the real ones under four thousand
        /// warnings. A loading screen is the boot; the phase flips when the layer says the world is
        /// playable: `EditorLayer` when its last startup stage completes, `RuntimeLayer` at the end of
        /// `OnAttach`.
        static void NoteBootFinished();

        [[nodiscard]] static LoadPhase Phase();

        /// Every load, at every nesting depth.
        [[nodiscard]] static uint64_t Loads();
        /// Wall time inside OUTERMOST loads only — see the nesting note above.
        [[nodiscard]] static double TotalMs();
        /// The subset that happened after the first frame began, and their time. This pair is the
        /// deliverable: nonzero `InFrameLoads` on a scene that has finished booting is a hitch with a
        /// name.
        [[nodiscard]] static uint64_t InFrameLoads();
        [[nodiscard]] static double   InFrameMs();
        /// The single slowest load seen, and what it was. For the line that says which file to look at.
        [[nodiscard]] static double      SlowestMs();
        [[nodiscard]] static std::string SlowestPath();

        /// The lines a person reads. Boot and in-frame separately, never summed.
        [[nodiscard]] static std::string Report();

        static void ResetForTest();

    private:
        friend class LoadTimingScope;
        static void Record( const std::string& path, double ms, bool outermost );
    };

    /**
     * @brief The scope that does the counting. Constructed by `AssetBase::Load()` and by nothing else.
     *
     * A TYPE RATHER THAN A PAIR OF CALLS, for the reason `ResourceOwnership` gives beside it: a load that
     * returns early — and eleven of the eighteen `LoadFromFile` bodies have at least one early return —
     * would skip the closing call, and a scope cannot. Its depth bookkeeping is what makes the nesting
     * rule above hold without any body knowing about it.
     */
    class LoadTimingScope final
    {
    public:
        explicit LoadTimingScope( std::string path );
        ~LoadTimingScope();

        LoadTimingScope( const LoadTimingScope& )            = delete;
        LoadTimingScope& operator=( const LoadTimingScope& ) = delete;
        LoadTimingScope( LoadTimingScope&& )                 = delete;
        LoadTimingScope& operator=( LoadTimingScope&& )      = delete;

    private:
        std::string m_Path;
        int64_t     m_StartNs  = 0;
        bool        m_Outermost = false;
    };

} // namespace Desert::Assets
