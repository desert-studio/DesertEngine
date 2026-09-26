#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// THE STARTUP MESH COOK RUNS AFTER THE WINDOW IS SHOWN (AL1-11, owner decision V2 — UE's DDC/Interchange
// shape: the editor opens on what the disk already holds and a fresh cook arrives later). The two boot
// stages "Cooking meshes" / "Cooking collections" used to block the splash for the whole freshness pass
// (0.40 s warm: a full read of every 10 MB `.stmesh` plus a hash of its source) and for every Assimp
// re-import (≈3.2 s cold). Nothing on the main thread needs the verdict before the reveal: a cooked asset on
// the disk is read by the preloader whether it is fresh or not, and a missing one has nothing to read.
//
// So the main thread asks only the cheap question — is a cooked output on the disk at all — and the
// worker answers the expensive one (fresh or stale) and pays the cook when it is owed. The two tables below
// are the whole policy; the suite `BackgroundStartupCook` pins every row.
namespace Desert::Editor
{
    // What the worker's cook of one source did. `Cooked` means the output on the disk changed.
    enum class CookVerdict
    {
        UpToDate, // the output on the disk matched its source; nothing was written
        Cooked,   // the output was (re)written
        Failed,   // the source was parsed or read and no complete output reached the disk
        NotCookable,
    };

    // What the editor does with a source before the window is shown.
    enum class StartupCookAction
    {
        UseCookedVerifyInBackground, // the old cook is loaded now; the worker checks it and recooks if stale
        PendingCookInBackground,     // nothing to load: the asset is Pending (not drawn) until the cook lands
    };

    // Freshness is NOT a startup question: answering it costs the read the background exists to move.
    constexpr StartupCookAction DecideStartupCook( bool cookedOnDisk )
    {
        return cookedOnDisk ? StartupCookAction::UseCookedVerifyInBackground
                            : StartupCookAction::PendingCookInBackground;
    }

    // What the main thread does when the worker's verdict arrives.
    enum class CookCompletionAction
    {
        Nothing,       // the loaded cook was fresh
        Reload,        // a stale cook was loaded; drop it so the next use reads the fresh one
        Register,      // the asset was Pending; register it so the scene's reference resolves
        ReportFailure, // logged with the source's path; a stale cook stays loaded, a Pending asset stays Pending
    };

    constexpr CookCompletionAction DecideCookCompletion( StartupCookAction started, CookVerdict verdict )
    {
        switch ( verdict )
        {
            case CookVerdict::UpToDate:
            case CookVerdict::NotCookable:
                return CookCompletionAction::Nothing;
            case CookVerdict::Failed:
                return CookCompletionAction::ReportFailure;
            case CookVerdict::Cooked:
                return started == StartupCookAction::UseCookedVerifyInBackground ? CookCompletionAction::Reload
                                                                                 : CookCompletionAction::Register;
        }
        return CookCompletionAction::ReportFailure;
    }

    // One queue of source cooks, run on whatever `submit` hands them to (the JobSystem in the editor, an
    // inline runner in the suite), drained on the main thread once a frame. Holds no engine state: what a
    // completion MEANS is the caller's (DecideCookCompletion), so this stays testable without a device.
    class BackgroundCookQueue
    {
    public:
        using Cooker = std::function<CookVerdict( const std::filesystem::path& )>;
        using Submit = std::function<void( std::function<void()> )>;

        struct Completed
        {
            std::filesystem::path Source;
            StartupCookAction     Started = StartupCookAction::PendingCookInBackground;
            CookVerdict           Verdict = CookVerdict::Failed;
        };

        BackgroundCookQueue( Cooker cooker, Submit submit )
             : m_Cooker( std::move( cooker ) ), m_Submit( std::move( submit ) )
        {
        }
        BackgroundCookQueue( const BackgroundCookQueue& )            = delete;
        BackgroundCookQueue& operator=( const BackgroundCookQueue& ) = delete;
        ~BackgroundCookQueue()
        {
            // Jobs capture `this`: wait out every cook still on a worker before the members die.
            for ( ;; )
            {
                {
                    std::lock_guard<std::mutex> lock( m_Mutex );
                    if ( m_Running == 0 )
                        break;
                }
                std::this_thread::yield();
            }
        }

        // Main thread. `cookedOnDisk` is the startup's cheap answer, recorded so the completion can be decided.
        void Enqueue( const std::filesystem::path& source, bool cookedOnDisk )
        {
            const StartupCookAction started = DecideStartupCook( cookedOnDisk );
            {
                std::lock_guard<std::mutex> lock( m_Mutex );
                ++m_Running;
                ++m_Total;
            }
            m_Submit(
                 [this, source, started]
                 {
                     const CookVerdict           verdict = m_Cooker( source );
                     std::lock_guard<std::mutex> lock( m_Mutex );
                     m_Completed.push_back( { source, started, verdict } );
                     --m_Running;
                 } );
        }

        // Main thread, once a frame.
        std::vector<Completed> Drain()
        {
            std::lock_guard<std::mutex> lock( m_Mutex );
            return std::exchange( m_Completed, {} );
        }

        // Cooks queued or running — the status bar's counter.
        std::size_t Outstanding() const
        {
            std::lock_guard<std::mutex> lock( m_Mutex );
            return m_Running;
        }
        std::size_t Total() const
        {
            std::lock_guard<std::mutex> lock( m_Mutex );
            return m_Total;
        }

    private:
        Cooker                 m_Cooker;
        Submit                 m_Submit;
        mutable std::mutex     m_Mutex;
        std::vector<Completed> m_Completed;
        std::size_t            m_Running = 0;
        std::size_t            m_Total   = 0;
    };
} // namespace Desert::Editor
