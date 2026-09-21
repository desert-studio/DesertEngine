#pragma once

#include "AssetBase.hpp"
#include "Common.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace Desert::Assets
{
    class AsyncAssetLoader;

    /// How a load ended. There is no `Cancelled` enumerator here on purpose: a cancelled request does not
    /// reach the completion delegate at all, it reaches the cancel delegate, and an outcome that could
    /// carry "cancelled" into `OnReady` would let a caller forget to branch on it.
    enum class LoadOutcome : uint8_t
    {
        Loaded = 0,
        Failed,
    };

    /**
     * @brief THE REQUEST HANDLE, AND IT *IS* THE KEEP-ALIVE.
     *
     * While one of these is alive the loader holds a `shared_ptr` to the asset it names, so "who wants
     * this loaded" and "what keeps it in memory" are one fact rather than two that can disagree. Drop
     * the handle and the interest is gone; there is no second `Unload` call to forget.
     *
     * ── `Cancel()` IS NOT `Release()`, AND CONFUSING THEM WEDGES THE CALLER ──────────────────────────
     *
     * `Docs/World/05_asset_loading_model.md` §2.3 separates them and `GAP_ANALYSIS.md` T2.3 makes the
     * cancel delegate MANDATORY for this reason: a caller that binds only a completion and then has its
     * request cancelled waits forever for a call that is never coming. So:
     *
     *   * **`Release()`** (and the destructor) — *"I no longer care."* No delegate fires, ever. The
     *     caller is the one who did it and does not need to be told. This is the ordinary end of a
     *     request whose result arrived.
     *   * **`Cancel()`** — *"I give up, and I need to know it is over."* The **cancel delegate** fires,
     *     on a later `Pump()`, exactly once. If the load had already finished and its completion was
     *     queued, the cancel still wins: the completion is dropped and the cancel delegate is what the
     *     caller hears. One request produces exactly one delegate call, and which one is decided by the
     *     caller rather than by a race.
     *
     * Both are idempotent and both leave `IsValid()` false.
     */
    class LoadRequest final
    {
    public:
        LoadRequest() = default;
        ~LoadRequest();

        LoadRequest( LoadRequest&& other ) noexcept;
        LoadRequest& operator=( LoadRequest&& other ) noexcept;

        // NOT COPYABLE, and that is the keep-alive being one fact: two handles to one request would be
        // two owners of a single delegate pair, and destroying either would have to decide whether the
        // other one still counts.
        LoadRequest( const LoadRequest& )            = delete;
        LoadRequest& operator=( const LoadRequest& ) = delete;

        /// Does this handle still name a live request? False for a default-constructed handle, for one
        /// that was cancelled or released, and for one whose delegate has already run.
        [[nodiscard]] bool IsValid() const noexcept;

        /// Which asset was asked for. Answers in every state, so a released handle can still say what it
        /// was about.
        [[nodiscard]] const AssetHandle& Handle() const noexcept
        {
            return m_Handle;
        }

        /// See the class comment: the cancel delegate fires, on a later `Pump()`.
        void Cancel();

        /// See the class comment: nothing fires.
        void Release();

    private:
        friend class AsyncAssetLoader;
        LoadRequest( uint64_t id, AssetHandle handle ) : m_Id( id ), m_Handle( handle )
        {
        }

        uint64_t    m_Id = 0;
        AssetHandle m_Handle{ 0 };
    };

    /**
     * @brief Reads assets on `Common::JobSystem` and hands the result back on a LATER tick, always.
     *
     * ── WHY THIS EXISTS AT ALL, IN ONE SENTENCE FROM OUR OWN TREE ────────────────────────────────────
     *
     * `AssetPreloader::PreloadCloudNoiseVolumes` explains its own eagerness with *"Deferring would buy a
     * stall exactly where the sky first appears"*, and that sentence is correct: measured on this
     * machine, `CloudNoise_Default.dcnv` costs **607.12 ms by its own time** and the four cloud stages
     * cost **2918.9 ms of a 5707.0 ms boot**. Making those kinds lazy without an async path would not
     * remove that cost, it would move it into whichever frame first looked at the sky — which is worse,
     * because a boot is a loading screen and a frame is a hitch. Laziness is not the feature;
     * *asynchrony plus an expressible "not here yet"* is (`GAP_ANALYSIS.md` §3.1).
     *
     * ── COMPLETION IS ALWAYS DEFERRED, EVEN WHEN THE ASSET IS ALREADY RESIDENT ────────────────────────
     *
     * `Request()` never calls a delegate before it returns. Not when the file has to be read, and not
     * when `IsReadyForUse()` is already true — the resident case is queued like every other and fires on
     * the next `Pump()`. It costs one frame in the case that was free, and it buys the removal of a
     * whole defect class: no caller's delegate can ever run before `Request()` has given that caller the
     * handle it is about to store, so no caller needs reentrancy code for a state it has not finished
     * building. Epic ships this rule for the same reason (`05` §2.1).
     *
     * ── WHAT RUNS WHERE ──────────────────────────────────────────────────────────────────────────────
     *
     * `Request()`, `Pump()`, `Cancel()`, `Release()` and every delegate call are MAIN-THREAD. Only
     * `AssetBase::Load()` runs on a `JobSystem` worker, and it touches nothing but the asset it was
     * handed — deliberately, because `AssetManager` is not thread-safe and nothing here asks it
     * anything. A GPU upload is therefore the caller's job inside its completion delegate, where it is
     * back on the main thread; this loader reads bytes and never touches Vulkan.
     */
    class AsyncAssetLoader final
    {
    public:
        /// Called on the main thread when the read finished — successfully or not. @p outcome is the
        /// whole of the difference; @p error is empty on success.
        using OnReady = std::function<void( const Asset<AssetBase>& asset, LoadOutcome outcome,
                                            const std::string& error )>;

        /// Called on the main thread when `Cancel()` won. Takes nothing: the caller knows what it
        /// cancelled, and handing it an asset it decided not to want is how a "cancel" ends up being
        /// treated as a completion.
        using OnCancel = std::function<void()>;

        static AsyncAssetLoader& Get();

        /**
         * @brief Ask for @p asset to be read on a worker.
         *
         * BOTH DELEGATES ARE REQUIRED PARAMETERS. There is no defaulted `onCancel` and there will not
         * be one: T2.3 calls it mandatory because the failure it prevents is silent and permanent.
         *
         * A null @p asset, a null @p onReady or a null @p onCancel is a REFUSAL — logged with the
         * handle, no job submitted, and the returned handle answers `IsValid() == false`. That is not
         * an empty successful answer: the caller can see it and the log names it.
         */
        [[nodiscard]] LoadRequest Request( const Asset<AssetBase>& asset, OnReady onReady, OnCancel onCancel );

        /**
         * @brief Run the delegates whose work has landed. ONE CALL PER TICK, on the main thread.
         *
         * A host that forgets this call gets a loader that never completes anything, which is why both
         * hosts' call sites are asserted by `AsyncAssetPump` rather than left to memory — the preloader
         * next door spent a whole feature's lifetime being a function nobody called.
         */
        void Pump();

        /// How many requests are still live — in flight, or finished but not yet pumped. A host waits on
        /// this to know its content has settled; a test waits on it to know the loader is quiet.
        [[nodiscard]] size_t Outstanding() const;

        /// How many reads have been handed to a worker since the process started. The counterpart of
        /// `SyncLoadLedger::Loads()`: together they say what share of this boot's reading stopped
        /// blocking anything.
        [[nodiscard]] uint64_t StartedCount() const;

        /// How many requests ended in the cancel delegate rather than the completion one.
        [[nodiscard]] uint64_t CancelledCount() const;

        /**
         * @brief Does a LIVE request name @p handle? Asked by anything that would take the payload away.
         *
         * WHAT THIS FIXES, FOUND BY RUNNING IT. The request handle owns a `shared_ptr` to the asset, so
         * the OBJECT cannot die under a worker. That is not the same as the PAYLOAD surviving, and the
         * difference cost a scene its whole sky: on `Clouds_HeroTrio` the read of `CloudNoise_Default`
         * finished at 04.671, `AssetEviction` swept at 04.761 and called `Unload()` on it -- the volume
         * is reached from a cloud TYPE, and no live scene root named that type -- and the completion
         * delegate pumped afterwards found `IsReadyForUse()` false and logged *"was read but could not be
         * uploaded: ... is not loaded"*. The frame drew no clouds at all.
         *
         * The eager model could not have this defect because the upload happened in the same call as the
         * read, with no sweep in between; deferral is what opens the window, and closing it is part of
         * the deferral rather than an extra. "The request handle IS the keep-alive" has to mean the
         * payload too, or it means very little.
         */
        [[nodiscard]] bool IsRequested( const AssetHandle& handle ) const;

        /// Drops every live request WITHOUT firing a delegate, and waits for any worker still inside a
        /// read to leave. For teardown and for tests; a scene close uses `Cancel()` on its own handles
        /// so the owners hear about it.
        void ShutdownAndDrain();

        /// Counters and live set back to the starting state. Tests only.
        void ResetForTest();

    private:
        AsyncAssetLoader() = default;

        friend class LoadRequest;
        void CancelById( uint64_t id );
        void ReleaseById( uint64_t id );
        bool IsLive( uint64_t id ) const;
    };
} // namespace Desert::Assets
