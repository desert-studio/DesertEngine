#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Desert::Graphic
{
    /**
     * @brief The identity a shared GPU object (a uniform buffer, a storage buffer, a material backend) is
     * known by inside every view that holds a copy of it.
     *
     * MONOTONIC AND NEVER REUSED. A view finds its copy of a resource by this key, so a key handed out twice
     * would let a NEW resource find the copy an old, destroyed one left behind — and bind its stale contents
     * with nothing in the log. A 64-bit counter cannot wrap within the life of a process, so "never reused"
     * holds without any free list. Zero is reserved as "no key" so a default-constructed key is detectably
     * unassigned.
     */
    class ViewResourceKey
    {
    public:
        constexpr ViewResourceKey() noexcept = default;

        // A fresh key, distinct from every key this process has ever produced. Thread-safe: resources are
        // created on loader threads as well as the main one.
        [[nodiscard]] static ViewResourceKey Allocate() noexcept;

        [[nodiscard]] constexpr uint64_t Value() const noexcept
        {
            return m_Value;
        }

        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return m_Value != 0;
        }

        constexpr bool operator==( const ViewResourceKey& ) const noexcept = default;

    private:
        explicit constexpr ViewResourceKey( uint64_t value ) noexcept : m_Value( value )
        {
        }

        uint64_t m_Value = 0;
    };

    /**
     * @brief One view's copy of one shared resource for one frame in flight.
     *
     * Opaque on purpose: the concrete copy (a VkBuffer and its allocation, a descriptor set) lives with the
     * resource type that made it, and its DESTRUCTOR hands the GPU objects to the allocator's deferred
     * deletion queue (VulkanAllocator::RT_Destroy*). That is what makes dropping a copy safe at any point of a
     * frame without waiting for the device, and what lets this file be driven without a device at all.
     */
    class IViewResourceCopy
    {
    public:
        virtual ~IViewResourceCopy() = default;
    };

    /**
     * @brief The narrow allocator seam: how a copy comes into existence.
     *
     * Implemented by the shared resource itself (it knows its size, usage and name). A copy is created the
     * first time a view binds the resource in a given frame slot, never up front — a view that never draws a
     * material never pays for its copies, which is the whole point of removing the "frames x 6" eager matrix.
     */
    class IViewResourceCopyFactory
    {
    public:
        virtual ~IViewResourceCopyFactory() = default;

        [[nodiscard]] virtual std::unique_ptr<IViewResourceCopy> CreateViewCopy( std::string_view viewName,
                                                                                 uint32_t         frameIndex ) = 0;
    };

    /**
     * @brief Everything one view keeps per frame in flight, keyed by the shared resource it copies.
     *
     * UE's pattern (FViewInfo owning its per-view uniform buffers): the REGISTRY is in the view and the KEY
     * is in the resource, so no view number exists anywhere and there is no upper bound on views. Lookup is a
     * hash probe per bind.
     *
     * Every live instance is on ViewResourceRegistry's list, so a resource that dies or is recreated while
     * views are open can take its copies back from all of them (Forget). Destroying the view drops every copy
     * it holds; each copy's destructor defers the GPU release, so no device wait is needed.
     */
    class ViewResources
    {
    public:
        explicit ViewResources( std::string name );
        ~ViewResources();

        // Registered by address on the live list: a copy or a move would leave the list pointing at the
        // wrong object.
        ViewResources( const ViewResources& )            = delete;
        ViewResources& operator=( const ViewResources& ) = delete;
        ViewResources( ViewResources&& )                 = delete;
        ViewResources& operator=( ViewResources&& )      = delete;

        // This view's copy of `key` for `frameIndex`, created through `factory` the first time it is asked
        // for. The factory must not return null: a resource that cannot make its copy has to say why through
        // its own error path, not hand a view nothing to bind.
        [[nodiscard]] IViewResourceCopy& Acquire( ViewResourceKey key, uint32_t frameIndex,
                                                  IViewResourceCopyFactory& factory );

        // The existing copy, or null when this view has not bound `key` in that frame slot yet.
        [[nodiscard]] IViewResourceCopy* Find( ViewResourceKey key, uint32_t frameIndex ) const;

        // Drops this view's copies of `key` in every frame slot. Returns how many were dropped.
        uint32_t Forget( ViewResourceKey key );

        // Drops every copy. Used at shutdown for the process-wide frame context, which outlives every view
        // and must give its copies back while the allocator still exists.
        void Clear();

        [[nodiscard]] uint32_t CopyCount() const noexcept;

        [[nodiscard]] const std::string& GetName() const noexcept
        {
            return m_Name;
        }

    private:
        friend class ViewResourceRegistry;

        // The frame context only: it is not a view, so it is not on the live list and not counted.
        struct FrameContextTag
        {
        };
        ViewResources( FrameContextTag, std::string name );

        using CopyMap = std::unordered_map<uint64_t, std::unique_ptr<IViewResourceCopy>>;

        bool m_Registered = true;

        std::string          m_Name;
        // A deque, not a vector: growing it never relocates the maps. MSVC's unordered_map move constructor
        // is not noexcept, so vector::resize copies instead of moving -- and a map of unique_ptr cannot be
        // copied (C2672 on Windows only; clang moves and compiled it).
        std::deque<CopyMap> m_Frames; // index = frame in flight; grown on first use of a slot
    };

    /**
     * @brief The list of live ViewResources, the process-wide frame context, and which one is active.
     *
     * THE FRAME CONTEXT. Not everything that writes a per-frame resource is a view: Render2D/UI, bakes and
     * anything recorded after the last view of a frame. Under renderer slots those writes went, implicitly,
     * into the copies of whichever view had recorded last. Here they have their own named owner — the frame
     * context — which is active whenever no view is, so a write outside every view can never land in a
     * view's copy.
     *
     * THREADING. The live list is guarded, so views may be created and destroyed on any thread; the copies
     * themselves are touched only on the thread that records frames (Acquire, Find, Forget), the same
     * thread that bound renderer slots before this.
     */
    class ViewResourceRegistry
    {
    public:
        // Views alive right now, the frame context NOT included.
        [[nodiscard]] static uint32_t LiveCount();

        // Takes every live view's copies of `key` (the frame context's too). Called when a shared resource is
        // destroyed or recreated; without it the copies would leak until each view closed. Returns how many
        // copies were dropped.
        static uint32_t Forget( ViewResourceKey key );

        // The owner of every per-frame write made outside a view. Lives for the whole process.
        [[nodiscard]] static ViewResources& FrameContext();

        // Where a per-frame write goes right now: the view inside an ActiveViewScope, else the frame context.
        [[nodiscard]] static ViewResources& Active();

    private:
        friend class ViewResources;
        friend class ActiveViewScope;

        static void           Register( ViewResources& view );
        static void           Unregister( ViewResources& view );
        static ViewResources* ExchangeActive( ViewResources* view );
        static void           RestoreActive( ViewResources* previous );
    };

    /**
     * @brief Makes one view the target of per-frame writes for as long as the scope lives.
     *
     * RAII, and restoring on EVERY exit, exceptions included, because the failure it prevents is silent: a
     * view left active after its phase returns would take the UI's writes into its own copies, which is
     * exactly the implicit coupling the frame context replaces. SceneRenderer opens one at the top of each
     * frame phase (BeginScene/OnUpdate/EndScene), where it used to bind its renderer slot.
     */
    class ActiveViewScope
    {
    public:
        explicit ActiveViewScope( ViewResources& view ) noexcept;
        ~ActiveViewScope() noexcept;

        ActiveViewScope( const ActiveViewScope& )            = delete;
        ActiveViewScope& operator=( const ActiveViewScope& ) = delete;
        ActiveViewScope( ActiveViewScope&& )                 = delete;
        ActiveViewScope& operator=( ActiveViewScope&& )      = delete;

    private:
        ViewResources* m_Previous; // null = the frame context was active
    };
} // namespace Desert::Graphic
