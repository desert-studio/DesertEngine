#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// THE STARTUP MESH COOK RUNS AFTER THE WINDOW IS SHOWN (AL1-11, owner decision V2 — UE's DDC/Interchange
// shape: the editor opens on what the cache already holds and a fresh cook arrives later). The boot stages
// "Cooking meshes" / "Cooking collections" used to block the splash for the whole freshness pass (0.40 s warm)
// and for every Assimp import (≈3.2 s cold).
//
// WHAT "THE COOK ON THE DISK" MEANS SINCE AF4h. An imported static mesh's envelope is a DDC entry keyed by the
// source file's own bytes (Assets::kMeshSourceDeriver), and the loader (Assets::LoadMeshSourceAsset) looks
// only under the CURRENT bytes' key. So there is no "stale but usable" cook to show while a fresh one is
// built: an entry for the current bytes is fresh by construction, and without one the mesh cannot load. The
// three startup states therefore collapse into two, decided with no work on the main thread:
//   fresh            -> the preload reads it now; the worker's check comes back UpToDate
//   stale or missing -> the load fails loudly and the asset is Pending (not drawn); the worker cooks it
// The DDC write is content-addressed and atomic (DDC::Put -> WriteContentToFileAtomic): a reader sees an
// entry whole or not at all, and a recook of changed bytes writes a DIFFERENT key rather than over the old one.
namespace Desert::Editor
{
    // What the worker's cook of one source did. `Cooked` means a new entry reached the cache.
    enum class CookVerdict
    {
        UpToDate, // the cache already held the entry for the source's current bytes; nothing was built
        Cooked,   // the entry was built and written
        Failed,   // the source was parsed or read and no complete entry reached the cache
        NotCookable,
    };

    // What the main thread does when the worker's verdict arrives.
    enum class CookCompletionAction
    {
        Nothing,       // the preload already read the fresh entry
        Reload,        // the asset was Pending: drop its failed load so the next use reads the new entry
        ReportFailure, // logged with the source's path; the asset stays Pending — never a silent substitute
    };

    constexpr CookCompletionAction DecideCookCompletion( CookVerdict verdict )
    {
        switch ( verdict )
        {
            case CookVerdict::UpToDate:
            case CookVerdict::NotCookable:
                return CookCompletionAction::Nothing;
            case CookVerdict::Failed:
                return CookCompletionAction::ReportFailure;
            case CookVerdict::Cooked:
                return CookCompletionAction::Reload;
        }
        return CookCompletionAction::ReportFailure;
    }

    // One queue of source cooks, run on whatever `submit` hands them to (the JobSystem in the editor, a held
    // list in the suite), drained on the main thread once a frame. Holds no engine state: what a completion
    // MEANS is DecideCookCompletion's, so this stays testable without a device.
    class BackgroundCookQueue
    {
    public:
        using Cooker = std::function<CookVerdict( const std::filesystem::path& )>;
        using Submit = std::function<void( std::function<void()> )>;

        struct Completed
        {
            std::filesystem::path Source;
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

        // Main thread. Returns at once; the freshness check and any cook run on the worker.
        void Enqueue( const std::filesystem::path& source )
        {
            {
                std::lock_guard<std::mutex> lock( m_Mutex );
                ++m_Running;
                ++m_Total;
            }
            m_Submit(
                 [this, source]
                 {
                     const CookVerdict           verdict = m_Cooker( source );
                     std::lock_guard<std::mutex> lock( m_Mutex );
                     m_Completed.push_back( { source, verdict } );
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
        // Every queued cook has completed (the startup report's trigger). A named question rather than
        // `Outstanding() == 0` in the host, which RuntimeLoadingState reserves for the loader's rule.
        bool AllSettled() const
        {
            std::lock_guard<std::mutex> lock( m_Mutex );
            return m_Running == 0;
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
