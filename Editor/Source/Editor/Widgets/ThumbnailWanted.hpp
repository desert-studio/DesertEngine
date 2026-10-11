#pragma once

#include <string>
#include <unordered_set>
#include <vector>

namespace Desert::Editor
{
    /**
     * @brief WHAT THE THUMBNAIL QUEUE STILL OWES, AND TO WHOM — the service's visibility rule, without a device.
     *
     * A capture is queued once (Queue) and stays owed only while some shower keeps asking for it: every
     * frame a tile is drawn it renews its ask (Ask, or StillAsked from a tile that already queued), and the
     * service's tick drops whatever nobody asked for since the previous tick (DropUnwanted). This is UE's
     * tile-view shape — only what is on screen is rendered — and the rule is small enough to be lost in a
     * renderer-owning class: THUMB-FOLDER measured 1 picture in 3 minutes on a 132-material folder because the
     * tiles asked once and the second tick dropped every one of them.
     *
     * Keyed on ThumbnailKey::Identity, never on a path spelling (two panels naming one asset share an entry).
     */
    class ThumbnailWanted
    {
    public:
        /// A shower asked for @p identity this frame (queued or not).
        void Ask( const std::string& identity )
        {
            m_Wanted.insert( identity );
        }

        /// Renews the ask of a capture this shower already queued. False when the queue no longer holds it
        /// (dropped while the shower was away, or settled): the shower forgets its ask and makes it again.
        [[nodiscard]] bool StillAsked( const std::string& identity )
        {
            Ask( identity );
            return IsQueued( identity );
        }

        void Queue( const std::string& identity )
        {
            m_Queued.insert( identity );
        }
        /// Settled, failed, skipped or invalidated: no longer owed.
        void Release( const std::string& identity )
        {
            m_Queued.erase( identity );
        }
        [[nodiscard]] bool IsQueued( const std::string& identity ) const
        {
            return m_Queued.contains( identity );
        }

        /// Removes from @p queue every request no shower asked for since the last call (and releases it),
        /// then starts the next tick's count. A request already dispatched is not in a queue and finishes.
        template <typename Request>
        void DropUnwanted( std::vector<Request>& queue )
        {
            std::erase_if( queue,
                           [this]( const Request& req )
                           {
                               if ( m_Wanted.contains( req.Identity ) )
                                   return false;
                               Release( req.Identity );
                               return true;
                           } );
        }
        /// Closes the tick: the asks counted so far are spent.
        void EndTick()
        {
            m_Wanted.clear();
        }

        void Clear()
        {
            m_Wanted.clear();
            m_Queued.clear();
        }

    private:
        std::unordered_set<std::string> m_Wanted; // asked for since the last EndTick
        std::unordered_set<std::string> m_Queued; // queued or in flight
    };
} // namespace Desert::Editor
