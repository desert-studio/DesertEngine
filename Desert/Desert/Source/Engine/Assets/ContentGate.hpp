#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace Desert::Assets
{
    /**
     * @brief Is the world the host is about to show FINISHED, or is it still arriving?
     *
     * ── WHY THIS IS A TYPE AND NOT A BOOL SOMEWHERE ──────────────────────────────────────────────────
     *
     * The demand-driven loader (`AsyncAssetLoader`) removed the eager preload -- a measured 3347.5 ms
     * of start-up became 310.5 ms -- by moving the read out of the boot and into the frame that first
     * ASKS for the asset. That trade is only honest if "not here yet" is a state the host can branch
     * on. Where it is not, the deficit is silent: a volumetric cloud whose volume has not landed falls
     * back to a procedural sky, and a procedural sky is indistinguishable from an authored one to
     * everybody except the person who authored it. The shipping host had exactly that hole -- it
     * pumped the loader and marked "startup over", but had no state for the frames in between, so the
     * first frames of a packaged game could present a world missing its sky.
     *
     * So the state is named, it has one implementation, and both hosts hold one.
     *
     * ── SETTLED MEANS TWO THINGS, AND THE SECOND IS THE ONE THAT GETS LEFT OUT ───────────────────────
     *
     * 1. nothing is outstanding, AND
     * 2. the frame just rendered started no new read.
     *
     * The second is what makes it correct for a CHAIN. A read that has just COMPLETED is the most
     * likely reason the next one is about to be made: a cloud type arrives, and only then is there a
     * handle with which to ask the noise service for its volume. A gate that opened on the first empty
     * queue would open in the gap BETWEEN two links and hand the player a world one link short --
     * which is the same defect as before, only rarer and therefore worse.
     *
     * And it takes TWO QUIET TICKS IN A ROW, not merely two ticks of which the last is quiet. The first
     * frame is where the renderer asks, so an empty queue before anything has looked at the scene says
     * only that nobody has looked yet -- the same mistake as reading a screenshot at three frames on a
     * swapchain with three frames in flight. But the weaker spelling (`quiet && frames >= 2`) also opens
     * one tick early whenever the next link of a chain is requested by the RENDER rather than by the
     * completion delegate; the trace is in `Tick`, and `OpensNotInTheGapBetweenTwoLinks` is the test
     * that goes red if the streak is weakened back.
     *
     * ── WHY IT TAKES NUMBERS AND NOT THE LOADER ─────────────────────────────────────────────────────
     *
     * `Tick` is a pure function of (outstanding, startedCount) and this object's own state, so the rule
     * above is testable without a JobSystem, a file or a device. The two call sites that feed it the
     * real loader's counters are pinned by the `RuntimeLoadingState` census instead, which is the half
     * a unit test structurally cannot see.
     */
    enum class ContentState : uint8_t
    {
        Ready = 0, ///< nothing outstanding and nothing asked for during the frame just rendered
        Loading,   ///< a world has been handed over and what it asked for is still arriving
    };

    class ContentGate final
    {
    public:
        /**
         * @brief Construct in @p initial.
         *
         * THE INITIAL STATE IS AUTHORED RATHER THAN DEFAULTED, because the two hosts genuinely differ
         * and the wrong default is invisible. The editor opens on an empty scene behind its own staged
         * boot overlay and is legitimately `Ready` until a scene load calls `BeginWorld`. The shipping
         * runtime is `Loading` from the instant it exists: it has a world to read and a player looking
         * at the window, and a gate that defaulted to `Ready` would present exactly the frames this
         * type was introduced to cover.
         */
        explicit ContentGate( ContentState initial ) : m_State( initial )
        {
        }

        /// A world has just been handed over; nothing it wants has been asked for yet. @p startedCount is
        /// `AsyncAssetLoader::StartedCount()` as it stands right now.
        void BeginWorld( uint64_t startedCount );

        /**
         * @brief One tick of the wait. Call ONCE per frame, AFTER `AsyncAssetLoader::Pump()`.
         *
         * @param outstanding  `AsyncAssetLoader::Outstanding()`
         * @param startedCount `AsyncAssetLoader::StartedCount()` -- monotonic, per read
         * @return true on the single tick that took the gate from `Loading` to `Ready`, so the caller
         *         can do the once-only work (start the game, log the cost) without a second flag of its
         *         own. Returns false while loading, and false on every tick after it opened.
         */
        [[nodiscard]] bool Tick( size_t outstanding, uint64_t startedCount );

        [[nodiscard]] bool Loading() const
        {
            return m_State == ContentState::Loading;
        }
        [[nodiscard]] ContentState State() const
        {
            return m_State;
        }
        /// Frames ticked since `BeginWorld` (or since construction, for a gate built `Loading`).
        [[nodiscard]] uint32_t FramesWaited() const
        {
            return m_Frames;
        }
        /// Wall clock of the wait: still running while `Loading`, frozen at the total once open.
        [[nodiscard]] double ElapsedMs() const;

        /// How many CONSECUTIVE quiet ticks open the gate. Named so a test cannot drift from it.
        static constexpr uint32_t kQuietFramesToOpen = 2;

    private:
        ContentState m_State = ContentState::Ready;
        /// `StartedCount()` as it stood at the beginning of the frame just ticked. A change across a
        /// frame means that frame asked for something, so the chain has not closed.
        uint64_t m_StartedAtFrameBegin = 0;
        uint32_t m_Frames              = 0;
        /// Consecutive ticks that saw an empty queue and a frame that asked for nothing.
        uint32_t                              m_QuietStreak   = 0;
        std::chrono::steady_clock::time_point m_Began         = std::chrono::steady_clock::now();
        double                                m_OpenedAfterMs = 0.0;
    };
} // namespace Desert::Assets
