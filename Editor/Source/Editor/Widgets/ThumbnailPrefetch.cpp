#include "ThumbnailPrefetch.hpp"

#include <Editor/Widgets/ThumbnailFreshness.hpp>

#include <Common/Core/JobSystem.hpp>

#include <stb_image/stb_image.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <thread>

namespace Desert::Editor
{
    std::optional<ThumbnailPixels> ThumbnailPixels::Decode( const std::string& path )
    {
        const auto began = std::chrono::steady_clock::now();

        int      w      = 0;
        int      h      = 0;
        int      ch     = 0;
        stbi_uc* pixels = stbi_load( path.c_str(), &w, &h, &ch, 4 );
        if ( pixels == nullptr || w <= 0 || h <= 0 )
        {
            stbi_image_free( pixels );
            return std::nullopt;
        }

        // Box-average downscale to <= kMaxDim. Rendered thumbnails are written AT that size, so this is a
        // straight copy for them; it still earns its place for the other images the browser previews at
        // their authored size (textures, video posters), where nearest-neighbour would alias and shimmer.
        const int maxSide = std::max( w, h );
        const int tw      = maxSide > kMaxDim ? std::max( 1, w * kMaxDim / maxSide ) : w;
        const int th      = maxSide > kMaxDim ? std::max( 1, h * kMaxDim / maxSide ) : h;

        ThumbnailPixels out;
        out.SourceWidth  = w;
        out.SourceHeight = h;
        out.Width        = tw;
        out.Height       = th;
        out.Rgba.resize( static_cast<size_t>( tw ) * th * 4 );
        for ( int y = 0; y < th; ++y )
        {
            const int sy0 = y * h / th;
            const int sy1 = std::max( sy0 + 1, ( y + 1 ) * h / th );
            for ( int x = 0; x < tw; ++x )
            {
                const int sx0 = x * w / tw;
                const int sx1 = std::max( sx0 + 1, ( x + 1 ) * w / tw );

                std::array<uint32_t, 4> acc{};
                uint32_t                n = 0;
                for ( int yy = sy0; yy < sy1; ++yy )
                    for ( int xx = sx0; xx < sx1; ++xx )
                    {
                        const unsigned char* s = pixels + ( static_cast<size_t>( yy ) * w + xx ) * 4;
                        for ( int c = 0; c < 4; ++c )
                            acc[c] += s[c];
                        ++n;
                    }
                const uint32_t div = std::max( 1u, n );
                unsigned char* d   = out.Rgba.data() + ( static_cast<size_t>( y ) * tw + x ) * 4;
                for ( int c = 0; c < 4; ++c )
                    d[c] = static_cast<unsigned char>( acc[c] / div );
            }
        }
        stbi_image_free( pixels );

        out.DecodedOn = std::this_thread::get_id();
        out.DecodeMs =
             std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - began ).count();
        return out;
    }

    ThumbnailPrefetch& ThumbnailPrefetch::Get()
    {
        static ThumbnailPrefetch s_Instance;
        return s_Instance;
    }

    void ThumbnailPrefetch::Request( std::vector<Item> items )
    {
        // Replace, not append: a folder the user has already left is not worth a worker's time, and what
        // was already decoded for it stays in m_Ready in case they come straight back.
        m_Waiting.clear();
        for ( Item& item : items )
        {
            if ( m_Ready.contains( item.Picture ) )
                continue;
            const bool running = std::any_of( m_InFlight.begin(), m_InFlight.end(),
                                              [&]( const InFlight& f ) { return f.Picture == item.Picture; } );
            if ( !running )
                m_Waiting.push_back( std::move( item ) );
        }
    }

    ThumbnailPrefetch::Decoded ThumbnailPrefetch::Run( const Item& item )
    {
        Decoded         out;
        std::error_code ec;
        // The stamp is read BEFORE the decode: a file rewritten while the worker reads it then carries a
        // newer stamp than this entry, Take() refuses it and Get() decodes the new file itself.
        out.Stamp = std::filesystem::last_write_time( item.Picture, ec );
        if ( ec )
            return out;

        // The same rule the tile applies before it calls Get(): only the picture it will actually draw is
        // decoded (an outdated PNG is, since it stays on screen while its capture is made). A missing one is the
        // capture queue's business, and that queue waits for the window.
        if ( !item.FreshnessSource.empty() &&
             ThumbnailFreshness::Choose( ThumbnailFreshness::Observe( item.Picture, item.FreshnessSource ) ) !=
                  ThumbnailFreshness::Picture::CachedPng )
            return out;

        out.Pixels    = ThumbnailPixels::Decode( item.Picture );
        out.Attempted = true;
        return out;
    }

    void ThumbnailPrefetch::Tick()
    {
        for ( auto it = m_InFlight.begin(); it != m_InFlight.end(); )
        {
            if ( it->Result.wait_for( std::chrono::seconds( 0 ) ) != std::future_status::ready )
            {
                ++it;
                continue;
            }
            if ( m_Ready.size() >= kMaxReady )
                m_Ready.clear(); // the same simple bound as ThumbnailCache; an evicted picture re-decodes lazily
            m_Ready[it->Picture] = it->Result.get();
            it                   = m_InFlight.erase( it );
        }

        // THROUGH THE JobSystem, like the cloud paint: work outside the pool is invisible to the profiler and
        // obeys no shared budget. The item is captured BY VALUE, so nothing the job touches can outlive this.
        while ( m_InFlight.size() < kMaxInFlight && !m_Waiting.empty() )
        {
            Item item = std::move( m_Waiting.front() );
            m_Waiting.erase( m_Waiting.begin() );
            std::string picture = item.Picture;
            m_InFlight.push_back(
                 { std::move( picture ),
                   Common::JobSystem::Get().Async( [item = std::move( item )]() { return Run( item ); } ) } );
        }
    }

    std::optional<ThumbnailPixels> ThumbnailPrefetch::Take( const std::string&              picture,
                                                            std::filesystem::file_time_type stamp )
    {
        const auto it = m_Ready.find( picture );
        if ( it == m_Ready.end() )
            return std::nullopt;
        Decoded decoded = std::move( it->second );
        m_Ready.erase( it );
        if ( decoded.Stamp != stamp )
            return std::nullopt;
        return std::move( decoded.Pixels );
    }

    ThumbnailPrefetch::Acquired ThumbnailPrefetch::Acquire( const std::string&              picture,
                                                            std::filesystem::file_time_type stamp )
    {
        Acquired out;
        if ( const auto it = m_Ready.find( picture ); it != m_Ready.end() )
        {
            Decoded decoded = std::move( it->second );
            m_Ready.erase( it );
            if ( decoded.Stamp == stamp && decoded.Pixels.has_value() )
            {
                if ( decoded.Pixels->DecodedOn == std::this_thread::get_id() )
                    ++m_DecodedOnTheTakingThread;
                out.Pixels = std::move( decoded.Pixels );
                return out;
            }
            if ( decoded.Stamp == stamp && decoded.Attempted )
            {
                out.Undecodable = true;
                return out;
            }
            // Decoded from an older file (a capture rewrote it since), or skipped by the folder prefetch's
            // freshness rule: this file as it is now has not been decoded yet — queue it below.
        }

        // Appended, not a Request(): the folder's own list stays ahead of it. No freshness source — the
        // caller has already decided this is the picture it draws.
        if ( !Pending( picture ) )
            m_Waiting.push_back( { picture, {} } );
        return out;
    }

    bool ThumbnailPrefetch::Pending( const std::string& picture ) const
    {
        return std::any_of( m_InFlight.begin(), m_InFlight.end(),
                            [&]( const InFlight& f ) { return f.Picture == picture; } ) ||
               std::any_of( m_Waiting.begin(), m_Waiting.end(),
                            [&]( const Item& i ) { return i.Picture == picture; } );
    }

    void ThumbnailPrefetch::Drain()
    {
        while ( !Idle() )
        {
            for ( const InFlight& f : m_InFlight )
                f.Result.wait();
            Tick();
        }
    }

    void ThumbnailPrefetch::Clear()
    {
        m_Waiting.clear();
        for ( const InFlight& f : m_InFlight )
            f.Result.wait(); // a job must not outlive the store it reports to
        m_InFlight.clear();
        m_Ready.clear();
    }
} // namespace Desert::Editor
