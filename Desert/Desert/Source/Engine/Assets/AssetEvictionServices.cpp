#include <Engine/Assets/AssetEviction.hpp>
#include <Engine/Assets/EvictionDeadline.hpp>

#include <Engine/Graphic/GeometryPool.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>
#include <Engine/Runtime/Services/Mesh/MeshService.hpp>
#include <Engine/Runtime/Services/Texture/TextureService.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>

namespace Desert::Assets
{
    namespace
    {
        /// THE ONLY PLACE IN THE EVICTION PATH THAT REACHES THE GPU LAYER, and it is four forwarding calls.
        ///
        /// Everything else — which assets are unreachable, which refuse, what the outcome says — lives in
        /// AssetEviction.cpp and compiles without Vulkan, which is what lets the suite drive the whole
        /// sweep against a recording double and assert the exact set of handles it released.
        class ServiceEvictionSink final : public IEvictionSink
        {
        public:
            bool DropBuiltMesh( const Common::AssetHandle& handle ) override
            {
                return Runtime::ResourceRegistry::GetMeshService()->EvictBuilt( handle );
            }

            std::vector<Common::AssetHandle> BuiltMeshHandles() const override
            {
                return Runtime::ResourceRegistry::GetMeshService()->BuiltHandles();
            }

            bool HasBuiltMaterial( const Common::AssetHandle& handle ) const override
            {
                return Runtime::ResourceRegistry::GetMaterialService()->HasBuiltMaterial( handle );
            }

            void DropBuiltMaterial( const Common::AssetHandle& handle ) override
            {
                Runtime::ResourceRegistry::GetMaterialService()->Invalidate( handle );
            }

            bool DropBuiltTexture( const Common::AssetHandle& handle ) override
            {
                return Runtime::ResourceRegistry::GetTextureService()->EvictBuilt( handle );
            }

            void CollectGarbage() override
            {
                Runtime::ResourceRegistry::GetMaterialService()->CollectGarbage();
            }
        };
    } // namespace

    IEvictionSink& EngineEvictionSink()
    {
        static ServiceEvictionSink sink;
        return sink;
    }

    // ────────────────────────────────────────────────────────────────────────────────────────────────
    // The trigger
    // ────────────────────────────────────────────────────────────────────────────────────────────────

    namespace
    {
        // Function-local static so the schedule is usable from a static initialiser. The rule is in
        // EvictionDeadline.hpp, where the suite holds it.
        EvictionDeadline& Deadline()
        {
            static EvictionDeadline deadline;
            return deadline;
        }
    } // namespace

    void AssetEvictionSchedule::Request( std::string why )
    {
        Deadline().Request( std::move( why ) );
    }

    namespace
    {
        /// The sweep in progress, if any: one per live registry, the reason that started it, and its cost so far.
        struct SweepInProgress
        {
            std::vector<std::unique_ptr<AssetEvictionSweep>> Sweeps;
            std::string                                      Why;
            uint32_t                                         Slices    = 0;
            double                                           TotalMs   = 0.0;
            double                                           LongestMs = 0.0;
        };

        std::optional<SweepInProgress>& InProgress()
        {
            static std::optional<SweepInProgress> sweep;
            return sweep;
        }

        bool IsLive( const AssetManager& manager )
        {
            const auto& live = AssetManager::LiveManagers();
            return std::find( live.begin(), live.end(), &manager ) != live.end();
        }
    } // namespace

    void AssetEvictionSchedule::RunIfDue( const std::function<AssetRootSet()>& collectRoots,
                                          const AssetEvictionSettings&         settings )
    {
        auto& progress = InProgress();

        // ONE SWEEP AT A TIME. While one is in progress the deadline is not consulted, so the requests every
        // departing cell makes meanwhile wait in it and start the next sweep after this one has finished.
        if ( !progress )
        {
            std::string why;
            if ( !Deadline().Due( why ) )
                return;
            if ( !collectRoots )
                return;
            progress.emplace();
            progress->Why = std::move( why );
            for ( AssetManager* manager : AssetManager::LiveManagers() )
                if ( manager != nullptr )
                    progress->Sweeps.push_back(
                         std::make_unique<AssetEvictionSweep>( *manager, EngineEvictionSink() ) );
        }

        const auto started  = std::chrono::steady_clock::now();
        const auto deadline = started + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                             std::chrono::duration<double, std::milli>( settings.SliceBudgetMs ) );

        // THE ROOTS OF THIS FRAME, every slice: a cell activated since the sweep began adds what it names before
        // anything more is released (AssetEvictionSweep::AddRoots). A registry gone with its scene is dropped.
        const AssetRootSet roots = collectRoots();
        std::erase_if( progress->Sweeps, []( const auto& sweep ) { return !IsLive( sweep->Manager() ); } );
        for ( const auto& sweep : progress->Sweeps )
            sweep->AddRoots( roots );

        bool finished = true;
        for ( const auto& sweep : progress->Sweeps )
        {
            if ( sweep->Done() )
                continue;
            if ( !sweep->Step( deadline ) )
            {
                finished = false;
                break;
            }
        }

        const double ms =
             std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - started ).count();
        progress->Slices++;
        progress->TotalMs += ms;
        progress->LongestMs = std::max( progress->LongestMs, ms );
        if ( !finished )
            return;

        for ( const auto& sweep : progress->Sweeps )
        {
            // ONE LINE PER SWEEP, ALWAYS, INCLUDING THE SWEEPS THAT RELEASED NOTHING. A sweep that frees
            // nothing because everything is still reachable and a sweep that frees nothing because its
            // root walk is broken produce the same memory graph, and only this line separates them: the
            // first reports a large `Reachable`, the second reports `Reachable` equal to a handful of
            // roots. That distinction is the whole reason the outcome carries counts rather than a bool.
            // The cost is the whole sweep's (every registry), one slice per frame.
            LOG_INFO(
                 "[Assets] eviction ({}) in {} frame(s), one slice per frame, {:.2f} ms in all, {:.2f} ms the "
                 "longest slice (budget {:.2f} ms): {}",
                 progress->Why, progress->Slices, progress->TotalMs, progress->LongestMs, settings.SliceBudgetMs,
                 sweep->Outcome().Describe() );
        }
        progress.reset();

        // The meshes this sweep dropped release their pool spans frames later (the mesh retire queue, then the
        // pool's own retire margin); the pool frees the pages they leave empty as they come back (FIELD-MEM).
        Graphic::GeometryPool::Get().RequestReleaseEmptyPages();
    }

} // namespace Desert::Assets
