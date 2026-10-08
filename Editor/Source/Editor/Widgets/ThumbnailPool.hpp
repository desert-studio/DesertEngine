#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::Editor
{
    // WHICH THUMBNAIL PICTURES STAY ON THE GPU — the bounded pool of UE's FAssetThumbnailPool (NumInPool, LRU by
    // LastAccessTime; UnrealEd/Private/AssetThumbnail.cpp ~2605), as a policy over keys with no device in it, so
    // a test can hold the bound. ThumbnailCache owns the images and asks this which ones to drop.
    //
    // A thumbnail is derived data: its PNG lives on disk (ThumbnailKey::DiskPath) and a picture dropped here is
    // decoded again by a worker from that file the next time a tile draws it. So the pool never has to keep a
    // picture nobody is looking at, and a project of N pictures costs at most Limit() textures, not N
    // (THM1n-13: the unbounded cache held 1297 pictures on Bistro).
    //
    // ONE DEPARTURE FROM UE, and it is the stricter side: a picture used in the CURRENT frame is never the one
    // evicted. UE evicts the least recently used even when it is on screen, so a view showing more tiles than
    // the pool holds re-renders every frame; here the pool grows past the limit for that frame instead and comes
    // back under it on the next admission after those tiles stop being drawn. The bound therefore reads:
    // resident <= max(Limit(), pictures used in one frame).
    class ThumbnailPool
    {
    public:
        // UE's Content Browser pool size (SAssetView: FAssetThumbnailPool(1024)).
        static constexpr std::size_t kDefaultLimit = 1024;

        explicit ThumbnailPool( std::size_t limit = kDefaultLimit ) : m_Limit( limit < 1 ? 1 : limit )
        {
        }

        [[nodiscard]] std::size_t Limit() const
        {
            return m_Limit;
        }
        [[nodiscard]] std::size_t Size() const
        {
            return m_Order.size();
        }
        [[nodiscard]] bool Contains( const std::string& key ) const
        {
            return m_Where.contains( key );
        }

        // A held picture was drawn in `frame`: it becomes the most recently used.
        void Touch( const std::string& key, std::uint64_t frame )
        {
            const auto it = m_Where.find( key );
            if ( it == m_Where.end() )
                return;
            it->second->Frame = frame;
            m_Order.splice( m_Order.end(), m_Order, it->second );
        }

        // A picture was uploaded in `frame`. Answers the keys whose pictures must be released to keep the pool
        // within the limit — least recently used first, never one used in `frame`.
        [[nodiscard]] std::vector<std::string> Admit( const std::string& key, std::uint64_t frame )
        {
            if ( m_Where.contains( key ) )
                Touch( key, frame );
            else
            {
                m_Order.push_back( { key, frame } );
                m_Where.emplace( key, std::prev( m_Order.end() ) );
            }
            return EvictDownTo( m_Limit, frame );
        }

        // A new limit (the preference changed). Evicts down to it at once, sparing `frame`'s pictures.
        [[nodiscard]] std::vector<std::string> SetLimit( std::size_t limit, std::uint64_t frame )
        {
            m_Limit = limit < 1 ? 1 : limit;
            return EvictDownTo( m_Limit, frame );
        }

        // The picture left by another route (invalidated, cache cleared).
        void Forget( const std::string& key )
        {
            const auto it = m_Where.find( key );
            if ( it == m_Where.end() )
                return;
            m_Order.erase( it->second );
            m_Where.erase( it );
        }

        void Clear()
        {
            m_Order.clear();
            m_Where.clear();
        }

    private:
        struct Slot
        {
            std::string   Key;
            std::uint64_t Frame = 0;
        };

        std::vector<std::string> EvictDownTo( std::size_t limit, std::uint64_t frame )
        {
            std::vector<std::string> evicted;
            // The front is the least recently used; once it was used this frame, so was everything behind it.
            while ( m_Order.size() > limit && m_Order.front().Frame != frame )
            {
                evicted.push_back( std::move( m_Order.front().Key ) );
                m_Where.erase( evicted.back() );
                m_Order.pop_front();
            }
            return evicted;
        }

        std::size_t                                                m_Limit;
        std::list<Slot>                                            m_Order; // least recently used first
        std::unordered_map<std::string, std::list<Slot>::iterator> m_Where;
    };
} // namespace Desert::Editor
