#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
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
} // namespace Desert::Runtime
