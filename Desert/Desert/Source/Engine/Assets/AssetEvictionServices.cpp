#include <Engine/Assets/AssetEviction.hpp>
#include <Engine/Assets/EvictionDeadline.hpp>

#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/Logger.hpp>

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

    void AssetEvictionSchedule::RunIfDue( const std::function<AssetRootSet()>& collectRoots )
    {
        std::string why;
        if ( !Deadline().Due( why ) )
            return;

        if ( !collectRoots )
            return;

        const AssetRootSet roots = collectRoots();

        for ( AssetManager* manager : AssetManager::LiveManagers() )
        {
            if ( manager == nullptr )
                continue;

            const EvictionOutcome outcome = AssetEviction::Run( *manager, roots, EngineEvictionSink() );

            // ONE LINE PER SWEEP, ALWAYS, INCLUDING THE SWEEPS THAT RELEASED NOTHING. A sweep that frees
            // nothing because everything is still reachable and a sweep that frees nothing because its
            // root walk is broken produce the same memory graph, and only this line separates them: the
            // first reports a large `Reachable`, the second reports `Reachable` equal to a handful of
            // roots. That distinction is the whole reason the outcome carries counts rather than a bool.
            LOG_INFO( "[Assets] eviction ({}): {}", why, outcome.Describe() );
        }
    }

} // namespace Desert::Assets
