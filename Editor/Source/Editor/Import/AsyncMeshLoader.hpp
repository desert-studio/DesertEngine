#pragma once

#include "ImportManager.hpp"

#include <Common/Core/JobSystem.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace Desert::Editor
{
    // Header-only async cooker on the engine JobSystem (it used to own a dedicated std::thread). Dropping
    // a heavy mesh must not HITCH the editor: the Assimp cook (parse FBX -> .stmesh/.tex) is pure CPU +
    // file I/O (NO GPU / no AssetManager / no ECS), so it runs on a pool worker; the MAIN thread later
    // drains completed cooks (PollCompleted) and does all the GPU/AssetManager/ECS work (register + spawn).
    //
    // Cooks stay ONE-AT-A-TIME (m_CookRunning gate): drag-dropped meshes may share textures, and the
    // serialized order is the long-standing guarantee here. Bulk parallel cooking lives in
    // ImportManager::ImportAllFromDirectory instead.
    class AsyncMeshLoader
    {
    public:
        struct Done
        {
            std::string SourcePath; // the dropped mesh source
            uint64_t    UserData;   // caller token (e.g. the pending entity's UUID)
        };

        AsyncMeshLoader() = default;

        ~AsyncMeshLoader()
        {
            // Jobs capture `this` — wait out any cook still in flight before the members die.
            for ( ;; )
            {
                {
                    std::lock_guard<std::mutex> lk( m_Mutex );
                    m_ShuttingDown = true;
                    if ( !m_CookRunning )
                        break;
                }
                std::this_thread::yield();
            }
        }

        AsyncMeshLoader( const AsyncMeshLoader& )            = delete;
        AsyncMeshLoader& operator=( const AsyncMeshLoader& ) = delete;

        // Queue a cook (main thread). Returns immediately; a pool worker does the heavy parse.
        void Request( const std::string& sourcePath, uint64_t userData )
        {
            {
                std::lock_guard<std::mutex> lk( m_Mutex );
                m_Queue.push( { sourcePath, userData } );
            }
            ++m_Total;
            Pump();
        }

        // Main thread, once per frame: take everything the workers have finished cooking.
        std::vector<Done> PollCompleted()
        {
            std::vector<Done> out;
            {
                std::lock_guard<std::mutex> lk( m_Mutex );
                while ( !m_Completed.empty() )
                {
                    out.push_back( m_Completed.front() );
                    m_Completed.pop();
                }
                // Idle (nothing queued, nothing cooking, nothing left to hand out) -> reset progress.
                if ( m_Queue.empty() && !m_CookRunning )
                {
                    m_Total = 0;
                    m_Done  = 0;
                }
            }
            return out;
        }

        bool IsBusy() const
        {
            std::lock_guard<std::mutex> lk( m_Mutex );
            return !m_Queue.empty() || m_CookRunning || !m_Completed.empty();
        }
        int   Total() const { return m_Total.load(); }
        int   Done2() const { return m_Done.load(); }
        float Progress() const
        {
            const int t = m_Total.load();
            return t > 0 ? static_cast<float>( m_Done.load() ) / static_cast<float>( t ) : 0.0f;
        }

    private:
        // Starts the next cook if none is running. Called with the queue freshly filled (Request) and
        // after every finished cook (from the worker) — so the queue always drains, one job at a time.
        void Pump()
        {
            Done job;
            {
                std::lock_guard<std::mutex> lk( m_Mutex );
                if ( m_CookRunning || m_ShuttingDown || m_Queue.empty() )
                    return;
                job           = m_Queue.front();
                m_Queue.pop();
                m_CookRunning = true;
            }

            Common::JobSystem::Get().Submit(
                 [this, job]
                 {
                     // The heavy part, OFF the main thread. No GPU, no AssetManager, no ECS — only this
                     // loader's own ImportManager + the filesystem.
                     // A drop is the user's import (UE: the factory), never the cook.
                     m_Importer.Import( job.SourcePath, ImportPass::Import );

                     {
                         std::lock_guard<std::mutex> lk( m_Mutex );
                         m_Completed.push( job );
                         m_CookRunning = false;
                     }
                     ++m_Done;
                     Pump(); // chain the next queued cook
                 } );
        }

        mutable std::mutex m_Mutex;
        std::queue<Done>   m_Queue;     // pending cooks
        std::queue<Done>   m_Completed; // cooked, awaiting main-thread spawn
        bool               m_CookRunning  = false;
        bool               m_ShuttingDown = false;
        std::atomic<int>   m_Total{ 0 };
        std::atomic<int>   m_Done{ 0 };
        ImportManager      m_Importer; // used by ONE in-flight cook at a time (m_CookRunning gate)
    };
} // namespace Desert::Editor
