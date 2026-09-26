#pragma once

#include <algorithm>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/ViewResources.hpp>
#include <Engine/ShaderResources/ViewCopiedBlock.hpp>

#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Desert::Graphic
{
    /**
     * @brief What one frame's descriptor writes actually bound into one set, per binding.
     *
     * Enough to tell a harmless re-apply of the SAME resource (a property inside its dirty window, applied
     * again by a second draw) from a REBIND to a different resource after the set was already flushed — the
     * write that has to be swallowed (rewriting a set bound in a recording command buffer is illegal without
     * update-after-bind) and must never be swallowed silently.
     */
    struct FrameWriteRecord
    {
        uint64_t                               Frame = std::numeric_limits<uint64_t>::max();
        std::unordered_map<uint32_t, uint64_t> Handles; // binding -> VkBuffer / VkImageView bits
    };

    /**
     * @brief One view's descriptor sets of one material for one frame in flight, and the bookkeeping that
     * used to be indexed [frame][renderer slot] inside the material backend.
     *
     * The concrete sets (VkDescriptorSet handles and the view pool they came from) live in the Vulkan
     * subclass; its destructor hands them to the allocator's deferred queue, so a view may close at any point
     * of a frame.
     */
    class IViewDescriptorSetCopy : public IViewResourceCopy
    {
    public:
        static constexpr uint64_t kNeverFlushed = std::numeric_limits<uint64_t>::max();

        // The absolute frame whose FlushUpdates last ran for these sets: a write after it in the same frame
        // targets a set that is already bound in the recording command buffer.
        uint64_t         FlushedFrame = kNeverFlushed;
        FrameWriteRecord Writes;
        // Which buffer COPY each binding was last written with. A fresh set points at fallbacks, so every
        // buffer binding is owed a write — which an empty record says by construction.
        ShaderResources::DescriptorCopyRecord BoundCopies;
    };

    /**
     * @brief The per-view descriptor sets of one material: lazily created in the ACTIVE view, filled with
     * fallbacks, then with the material's last known images.
     *
     * UE's pattern again (FViewInfo holds the per-view state, the resource holds the key). Under renderer
     * slots every (frame x slot) set existed from the material's construction and was filled with fallbacks
     * up front; a view reusing a slot also inherited whatever textures the slot's previous owner had bound.
     * Here a set exists only once a view binds the material, so two rules replace that inheritance:
     *
     *   - FALLBACKS FIRST. A set is never bound with an undefined descriptor: the initializer writes a
     *     fallback into every declared binding the moment the set is made.
     *   - THEN THE PROPERTIES' OWN WRITES. Every binding is checked on every apply against BoundCopies (the
     *     resource and the property version it was written with), which a fresh set starts empty — so a view
     *     opened long after a one-off texture write still takes it on its first apply.
     *
     * A write never touches another view's set: the active view's copy is the only one resolved.
     */
    class ViewDescriptorSets
    {
    public:
        using SetMaker = std::function<Common::BoolResultStr( std::string_view viewName, uint32_t frameIndex,
                                                              std::unique_ptr<IViewDescriptorSetCopy>& out )>;
        // Writes into the sets of the ACTIVE view for `frameIndex` (they are already registered there, so the
        // writer may resolve them again by key).
        using SetWriter = std::function<void( uint32_t frameIndex )>;

        ViewDescriptorSets() = default;
        ~ViewDescriptorSets()
        {
            ViewResourceRegistry::Forget( m_Key );
        }

        ViewDescriptorSets( const ViewDescriptorSets& )            = delete;
        ViewDescriptorSets& operator=( const ViewDescriptorSets& ) = delete;
        ViewDescriptorSets( ViewDescriptorSets&& )                 = delete;
        ViewDescriptorSets& operator=( ViewDescriptorSets&& )      = delete;

        [[nodiscard]] ViewResourceKey GetKey() const noexcept
        {
            return m_Key;
        }

        // The active view's sets for `frameIndex`, or null when that view has not bound this material yet.
        [[nodiscard]] IViewDescriptorSetCopy* FindActive( const uint32_t frameIndex ) const
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            return static_cast<IViewDescriptorSetCopy*>(
                 ViewResourceRegistry::Active().Find( m_Key, frameIndex ) );
        }

        // The active view's sets for `frameIndex`, made through `make` the first time, then given
        // `initialize` (the fallbacks).
        [[nodiscard]] Common::BoolResultStr Resolve( const uint32_t frameIndex, const SetMaker& make,
                                                     const SetWriter& initialize, IViewDescriptorSetCopy*& out )
        {
            if ( IViewDescriptorSetCopy* existing = FindActive( frameIndex ) )
            {
                out = existing;
                return Common::MakeSuccess( true );
            }

            ViewResources&                          view = ViewResourceRegistry::Active();
            std::unique_ptr<IViewDescriptorSetCopy> made;
            auto                                    created = make( view.GetName(), frameIndex, made );
            if ( !created.IsSuccess() )
                return created;
            if ( !made )
                return Common::MakeFormattedError<bool>( "view '{}', frame {}: the descriptor-set maker reported "
                                                         "success and returned no sets",
                                                         view.GetName(), frameIndex );

            HandOver handOver( std::move( made ) );
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            out = static_cast<IViewDescriptorSetCopy*>( &view.Acquire( m_Key, frameIndex, handOver ) );
            initialize( frameIndex );
            return Common::MakeSuccess( true );
        }

    private:
        class HandOver final : public IViewResourceCopyFactory
        {
        public:
            explicit HandOver( std::unique_ptr<IViewDescriptorSetCopy> copy ) : m_Copy( std::move( copy ) )
            {
            }
            std::unique_ptr<IViewResourceCopy> CreateViewCopy( std::string_view /*viewName*/,
                                                               uint32_t /*frameIndex*/ ) override
            {
                return std::move( m_Copy );
            }

        private:
            std::unique_ptr<IViewDescriptorSetCopy> m_Copy;
        };

        ViewResourceKey m_Key = ViewResourceKey::Allocate();
        // Ordered by binding so the replay order is deterministic.
    };

    /**
     * @brief What one allocation takes out of a descriptor pool: the sets themselves, and the descriptors by
     * type.
     *
     * Device-free on purpose -- the type is a VkDescriptorType's underlying integer rather than the enum --
     * because the rule it feeds ("has this block room?") is arithmetic and has to be testable without a
     * device.
     */
    struct DescriptorRequest
    {
        uint32_t Sets = 0;
        // VkDescriptorType (as its underlying integer) -> how many descriptors of that type are needed.
        std::map<uint32_t, uint32_t> Descriptors;
    };

    /**
     * @brief One descriptor pool's remaining room, counted the way the driver counts it.
     *
     * WHY THIS EXISTS. The chain below used to discover that a pool was full BY ALLOCATING FROM IT AND
     * FAILING: vkAllocateDescriptorSets answers VK_ERROR_OUT_OF_POOL_MEMORY, the chain reads that as "open
     * the next block" and carries on. But it is still a FAILED Vulkan call, and the validation layer reports
     * every one of them as an error ("Unable to allocate N descriptors of type ... this pool only has M
     * remaining"); with the debug callback breaking into the debugger, an entirely routine block turnover
     * stopped the editor on Windows. On MoltenVK that branch almost never ran, which is how it shipped.
     *
     * Counting the room here means the block is chosen BEFORE the driver is asked, so a turnover makes no
     * failed call and produces no validation message.
     *
     * A TYPE THE POOL WAS NOT CREATED WITH HAS NO ROOM, and answering false for it is the point: a shader
     * that declares a descriptor type the block sizes never listed is a gap in those sizes, and it now says
     * so by name instead of failing once per draw somewhere inside the driver.
     */
    class DescriptorBudget
    {
    public:
        DescriptorBudget() = default;
        DescriptorBudget( const uint32_t sets, std::map<uint32_t, uint32_t> descriptors )
             : m_Sets( sets ), m_Descriptors( std::move( descriptors ) )
        {
        }

        [[nodiscard]] bool CanHold( const DescriptorRequest& request ) const
        {
            if ( request.Sets > m_Sets )
                return false;
            return std::ranges::all_of( request.Descriptors,
                                        [this]( const auto& entry )
                                        {
                                            const auto room = m_Descriptors.find( entry.first );
                                            return room != m_Descriptors.end() && entry.second <= room->second;
                                        } );
        }

        // Takes the request out of the budget, or leaves the budget UNTOUCHED and answers false. All or
        // nothing: a partial take would leave the block short of the sets it had just promised, and the
        // shortfall would surface as the same failed allocation this class exists to avoid.
        [[nodiscard]] bool Reserve( const DescriptorRequest& request )
        {
            if ( !CanHold( request ) )
                return false;
            m_Sets -= request.Sets;
            for ( const auto& [type, count] : request.Descriptors )
                m_Descriptors[type] -= count;
            return true;
        }

        [[nodiscard]] uint32_t RemainingSets() const noexcept
        {
            return m_Sets;
        }

        [[nodiscard]] uint32_t Remaining( const uint32_t descriptorType ) const
        {
            const auto room = m_Descriptors.find( descriptorType );
            return room == m_Descriptors.end() ? 0U : room->second;
        }

    private:
        uint32_t                     m_Sets = 0;
        std::map<uint32_t, uint32_t> m_Descriptors;
    };

    /// What one allocation attempt from one pool came to.
    enum class PoolAllocation : uint8_t
    {
        Allocated,
        // VK_ERROR_OUT_OF_POOL_MEMORY / VK_ERROR_FRAGMENTED_POOL: this pool is full, a new one may succeed.
        PoolExhausted,
        // Anything else: a new pool would fail the same way, so the chain stops and says why.
        Failed,
    };

    /**
     * @brief A view's descriptor pools: allocate from the newest, open a new one when it is full.
     *
     * A view has no upper bound on the materials it draws, so no single pool size is right; a chain of
     * fixed-size pools grows with what the view actually binds. The pools are shared_ptr because every set
     * keeps its pool alive until the set itself is freed — the deferred free of the set has to reach the
     * deletion queue before the pool's own destruction does.
     */
    template <class Pool>
    class DescriptorPoolChain
    {
    public:
        using CreatePool  = std::function<Common::BoolResultStr( uint32_t ordinal, std::shared_ptr<Pool>& out )>;
        using TryAllocate = std::function<PoolAllocation( Pool& pool, std::string& why )>;
        // Takes the request out of the pool's own budget, or answers false without changing it. Asked BEFORE
        // TryAllocate, so a full block is left alone instead of being allocated from and failed -- see
        // DescriptorBudget for what that failed call costs on Windows.
        using Reserve = std::function<bool( Pool& pool )>;

        [[nodiscard]] Common::BoolResultStr Allocate( const CreatePool& create, const Reserve& reserve,
                                                      const TryAllocate& tryAllocate, std::shared_ptr<Pool>& from )
        {
            bool fresh = false;
            if ( m_Pools.empty() )
            {
                auto opened = Open( create );
                if ( !opened.IsSuccess() )
                    return opened;
                fresh = true;
            }
            for ( ;; )
            {
                std::string why;
                // THE BLOCK IS CHOSEN FROM OUR OWN ACCOUNTING, and the driver is asked only once a block is
                // known to have room. Letting vkAllocateDescriptorSets be the one to say "full" made every
                // routine turnover a failed Vulkan call, which the validation layer reports as an error and
                // the debug callback turns into a break.
                if ( reserve( *m_Pools.back() ) )
                {
                    const PoolAllocation outcome = tryAllocate( *m_Pools.back(), why );
                    if ( outcome == PoolAllocation::Allocated )
                    {
                        from = m_Pools.back();
                        return Common::MakeSuccess( true );
                    }
                    if ( outcome == PoolAllocation::Failed )
                        return Common::MakeFormattedError<bool>( "descriptor pool #{} of {}: {}",
                                                                 m_Pools.size() - 1, m_Pools.size(), why );
                    // RESERVED AND STILL REFUSED: the driver's accounting and ours disagree. The chain moves
                    // on to a new block so the frame survives, but the reason is carried into the refusal
                    // below -- this branch is a defect in the block sizes, not a normal turnover.
                    why = "the block's budget said it fit and the driver refused it anyway -- " + why;
                }
                else
                {
                    why = "the block has no room left for this request";
                }
                // A pool that was opened for this very request and still cannot hold it never will; a
                // third pool would fail the same way, and so on without end.
                if ( fresh )
                    return Common::MakeFormattedError<bool>( "a fresh descriptor pool (#{}) cannot hold one "
                                                             "material's sets: {}",
                                                             m_Pools.size() - 1, why );
                auto opened = Open( create );
                if ( !opened.IsSuccess() )
                    return opened;
                fresh = true;
            }
        }

        [[nodiscard]] std::size_t PoolCount() const noexcept
        {
            return m_Pools.size();
        }

    private:
        Common::BoolResultStr Open( const CreatePool& create )
        {
            std::shared_ptr<Pool> pool;
            auto                  created = create( static_cast<uint32_t>( m_Pools.size() ), pool );
            if ( !created.IsSuccess() )
                return created;
            if ( !pool )
                return Common::MakeFormattedError<bool>( "descriptor pool #{}: creation reported success and "
                                                         "returned no pool",
                                                         m_Pools.size() );
            m_Pools.push_back( std::move( pool ) );
            return Common::MakeSuccess( true );
        }

        std::vector<std::shared_ptr<Pool>> m_Pools;
    };
} // namespace Desert::Graphic
