#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace Desert::Runtime
{
    /**
     * TEXTURES THAT ARE COOKED BUT NOT YET ON THE GPU (AM2), in the order their workers finished.
     *
     * UE's pattern (texture streaming: the render thread takes pending mip uploads each frame up to a byte
     * budget) reduced to what this engine needs: workers `Push` a finished payload from any thread, and the
     * frame loop `TakeWithinBudget` once per frame on the main thread. The budget is what keeps a scene that
     * asks for forty textures at once from putting forty staging copies into one frame; a texture that has not
     * been taken yet keeps drawing the slot default.
     *
     * The first item of a frame is always taken, even when it alone is larger than the budget: a 2k BC7 texture
     * (5.6 MB) under a 4 MB budget would otherwise wait forever. So a frame uploads at most one item past the
     * budget, never zero items while something waits.
     *
     * `Ticket` is the caller's generation for the handle: an item whose ticket no longer matches (the entry was
     * cleared, evicted or re-requested while the worker ran) is the caller's to discard. The queue does not
     * know about entries; it only keeps order and bytes.
     */
    template <typename Key, typename Payload>
    class TextureUploadQueue
    {
    public:
        struct Item
        {
            Key      Handle{};
            uint64_t Ticket = 0;
            uint64_t Bytes  = 0;
            Payload  Data{};
        };

        /// Any thread. Order of arrival is order of upload.
        void Push( Item item )
        {
            const std::lock_guard<std::mutex> lock( m_Mutex );
            m_PendingBytes += item.Bytes;
            m_Items.push_back( std::move( item ) );
        }

        /// Main thread, once per frame: the items to upload now. Stops before the item that would take the sum
        /// past @p budgetBytes, except that the first item always goes.
        std::vector<Item> TakeWithinBudget( const uint64_t budgetBytes )
        {
            std::vector<Item>                 taken;
            const std::lock_guard<std::mutex> lock( m_Mutex );
            uint64_t                          spent = 0;
            while ( !m_Items.empty() )
            {
                const uint64_t next = m_Items.front().Bytes;
                if ( !taken.empty() && spent + next > budgetBytes )
                    break;
                spent += next;
                m_PendingBytes -= next;
                taken.push_back( std::move( m_Items.front() ) );
                m_Items.pop_front();
            }
            return taken;
        }

        void Clear()
        {
            const std::lock_guard<std::mutex> lock( m_Mutex );
            m_Items.clear();
            m_PendingBytes = 0;
        }

        std::size_t Pending() const
        {
            const std::lock_guard<std::mutex> lock( m_Mutex );
            return m_Items.size();
        }

        uint64_t PendingBytes() const
        {
            const std::lock_guard<std::mutex> lock( m_Mutex );
            return m_PendingBytes;
        }

    private:
        mutable std::mutex m_Mutex;
        std::deque<Item>   m_Items;
        uint64_t           m_PendingBytes = 0;
    };

    /**
     * HOW MANY COOKS MAY HOLD DECODED PIXELS AT ONCE (SHOT-SETTLE-c). A cook occupies a slot from the moment it
     * is handed to a worker until the frame loop takes its result off the upload queue; everything asked for
     * beyond the capacity waits here as a key, which costs nothing, instead of on a worker, which costs the
     * whole decoded texture.
     *
     * Measured on Bistro night_street_a: 398 platform-data reads were admitted in 15 s while the main thread
     * sat in one long splash frame, every decoded result (22 MB for a 2k RGBA8 with mips) waited in the upload
     * queue, and Malloc Large reached 1.8 GB in 10 s and the footprint 5.3 GB before a single upload ran. The
     * upload budget bounds the GPU side per frame; nothing bounded the CPU side between the worker and the
     * frame. UE's pattern: the streamer keeps a bounded number of requests in flight
     * (FRenderAssetStreamingManager's pending-request limit) and leaves the rest as wanted, not loaded.
     *
     * Main thread only: the slots are taken and returned where the queue is pumped.
     */
    template <typename Key>
    class CookAdmission
    {
    public:
        explicit CookAdmission( const std::size_t capacity ) : m_Capacity( capacity )
        {
        }

        /// True: start the cook now (a slot is taken). False: @p key waits for `NextToStart`. A capacity of zero
        /// admits nothing.
        bool TryAdmit( const Key& key )
        {
            if ( m_Running < m_Capacity )
            {
                ++m_Running;
                return true;
            }
            m_Waiting.push_back( key );
            return false;
        }

        /// One cook's result was taken off the upload queue (built, failed or stale alike): its slot is free.
        void Release()
        {
            if ( m_Running > 0 )
                --m_Running;
        }

        /// The oldest waiting key, with a slot taken for it; empty when nothing waits or no slot is free.
        std::optional<Key> NextToStart()
        {
            if ( m_Waiting.empty() || m_Running >= m_Capacity )
                return std::nullopt;
            ++m_Running;
            Key key = m_Waiting.front();
            m_Waiting.pop_front();
            return key;
        }

        /// Forget the waiting keys; the running cooks keep their slots until their results are taken.
        void DropWaiting()
        {
            m_Waiting.clear();
        }

        std::size_t Running() const
        {
            return m_Running;
        }

        std::size_t Waiting() const
        {
            return m_Waiting.size();
        }

    private:
        std::size_t     m_Capacity = 0;
        std::size_t     m_Running  = 0;
        std::deque<Key> m_Waiting;
    };
} // namespace Desert::Runtime
