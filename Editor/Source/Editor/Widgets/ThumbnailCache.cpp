#include "ThumbnailCache.hpp"

#include <Editor/Widgets/ThumbnailKey.hpp>
#include <Editor/Widgets/ThumbnailPrefetch.hpp>

#include <Common/Content/DerivedDataCache.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>

#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace Desert::Editor
{
    bool ThumbnailCache::IsOurGeneratedThumbnail( const std::string& path )
    {
        // Derived from the SAME root DiskPath() builds its answers under, rather than matched by name.
        // A predicate that looked for "Thumbnails" or ".png" in the string would also answer yes for a
        // texture an artist happened to file under a folder called Thumbnails — and this predicate is the
        // one thing standing between a decode failure and `remove()`.
        std::error_code ec;
        const auto      root = std::filesystem::weakly_canonical( Common::DDC::BucketDir( "Thumbnails" ), ec );
        if ( ec )
            return false;

        const auto candidate = std::filesystem::weakly_canonical( std::filesystem::path( path ), ec );
        if ( ec )
            return false;

        const std::string relative = candidate.lexically_relative( root ).generic_string();
        return !relative.empty() && relative != "." && relative.rfind( "..", 0 ) != 0;
    }

    void ThumbnailCache::PurgeOldVersions()
    {
        std::error_code             ec;
        const std::filesystem::path root( Common::DDC::BucketDir( "Thumbnails" ) );
        if ( !std::filesystem::exists( root, ec ) )
            return;
        const std::string keep = "v" + std::to_string( ThumbnailKey::CacheVersion() );
        for ( const auto& entry : std::filesystem::directory_iterator( root, ec ) )
        {
            if ( entry.is_directory( ec ) && entry.path().filename() == keep )
                continue;
            std::filesystem::remove_all( entry.path(), ec );
        }
    }

    std::shared_ptr<Graphic::Image2D> ThumbnailCache::Get( const std::string& sourcePath )
    {
        std::error_code                   stampEc;
        const auto                        stamp = std::filesystem::last_write_time( sourcePath, stampEc );
        std::shared_ptr<Graphic::Image2D> previous;
        if ( const auto it = m_Cache.find( sourcePath ); it != m_Cache.end() )
        {
            const auto seen = m_Stamps.find( sourcePath );
            if ( stampEc || ( seen != m_Stamps.end() && seen->second == stamp ) )
                return it->second; // may be null (decode previously failed)
            // The file was rewritten since it was decoded (a capture landed): the old picture stays on
            // screen until the worker has the new one, instead of the icon for those frames.
            previous = it->second;
        }

        if ( m_Cache.size() >= kMaxEntries )
        {
            m_Cache.clear(); // simple bound; thumbnails re-decode lazily
            m_Stamps.clear();
        }

        // NOTHING IS DECODED HERE, IN ANY CASE. The decode (file read, stb, box filter: ~24 ms for a 512px
        // PNG) is always a worker's: ThumbnailPrefetch::Acquire hands over pixels a worker finished from the
        // file as it is now, or queues the file and this tile draws `previous` (or its icon) for the frames
        // that takes. What is left on the main thread is the upload. Measured before this rule, the Materials
        // folder put 36-42 such decodes on the main thread ~1.5 s after it was opened — a rescan cleared this
        // cache, and every picture a capture rewrote was read back here — about a second of stalled frames.
        const auto                  began = std::chrono::steady_clock::now();
        ThumbnailPrefetch::Acquired acquired;
        if ( !stampEc )
        {
            acquired = ThumbnailPrefetch::Get().Acquire( sourcePath, stamp );
            if ( !acquired.Pixels && !acquired.Undecodable )
                return previous;
        }

        std::shared_ptr<Graphic::Image2D> result;
        std::optional<ThumbnailPixels>&   decoded = acquired.Pixels;
        const std::string failure = stampEc ? stampEc.message() : std::string( "a worker could not decode it" );
        if ( decoded.has_value() )
        {
            const int                                 w        = decoded->SourceWidth;
            const int                                 h        = decoded->SourceHeight;
            const int                                 tw       = decoded->Width;
            const int                                 th       = decoded->Height;
            const double                              decodeMs = decoded->DecodeMs;
            const bool                                onMain   = decoded->DecodedOn == std::this_thread::get_id();
            const Core::Formats::Image2DSpecification spec     = {
                     .Tag        = "Thumb_" + std::filesystem::path( sourcePath ).filename().string(),
                     .Width      = static_cast<uint32_t>( tw ),
                     .Height     = static_cast<uint32_t>( th ),
                     .Format     = Core::Formats::ImageFormat::RGBA8F,
                     .Mips       = 1u,
                     .Data       = std::move( decoded->Rgba ),
                     .Usage      = Core::Formats::Image2DUsage::Image2D,
                     .Properties = Core::Formats::Sample,
            };
            // THIS IMAGE HAS AN OWNER, AND IT SAYS SO. Every thumbnail in the editor is created on this
            // one line — the browser grid, the Collections cards, both Details slots, the drag ghost —
            // so one scope here attributes all of them and the sixth panel written next year is
            // attributed the day it calls Get(). That is the arithmetic ResourceAttributionScope exists
            // for (Engine/Graphic/ResourceLedger.hpp): claiming them one at a time would mean touching
            // five files to answer one question and silently missing the sixth.
            //
            // EditorTool rather than AssetService, and the difference is not cosmetic: AssetService is
            // the ONLY category asset eviction may release, because there the asset's file is the recipe.
            // A thumbnail's recipe is a render, not a file read — dropping one to reclaim memory would
            // cost a capture to rebuild, not a load. Mis-filing it here would hand eviction a lever it
            // must not have.
            const Graphic::ResourceAttributionScope owned( Graphic::ResourceOwner::EditorTool );
            result = Graphic::Image2D::Create( spec );

            const double mainMs =
                 std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - began ).count();
            const std::string name = std::filesystem::path( sourcePath ).filename().string();
            LOG_DEBUG( "[Thumbnails] '{}' {}x{} -> {}x{}: {:.0f} ms on the main thread ({}, decode {:.0f} ms)",
                       name, w, h, tw, th, mainMs, onMain ? "decoded on the main thread" : "decoded on a worker",
                       decodeMs );
            if ( onMain )
                LOG_WARN(
                     "[Thumbnails] '{}' was decoded on the main thread ({:.0f} ms): decoded on main thread: {}",
                     name, decodeMs, ThumbnailPrefetch::Get().DecodedOnTheTakingThread() );

            // The one line "window shown -> first picture" is read from (TH2's measurement): its timestamp
            // against [Startup] reveal.
            static bool s_FirstUploaded = false;
            if ( !s_FirstUploaded )
            {
                s_FirstUploaded = true;
                LOG_INFO( "[Thumbnails] first picture uploaded: '{}' ({}, {:.0f} ms on the main thread)", name,
                          onMain ? "decoded on the main thread" : "decoded on a worker", mainMs );
            }
        }
        else if ( IsOurGeneratedThumbnail( sourcePath ) )
        {
            // A GENERATED file that will not decode is deleted, not merely reported, and the reason is the
            // invariant ThumbnailFreshness exists for: every state must be either shown or scheduled. An
            // undecodable PNG is "fresh" by modification time, so the freshness rule says show it and the
            // decoder cannot — leaving that asset with no picture and no capture queued, permanently. Not
            // hypothetical: an editor killed during a capture used to leave a truncated file here (new
            // captures write to a temp file and rename, so they no longer can — see AssetThumbnailRenderer
            // — but files already on disk still can be).
            //
            // Nothing is lost: this is a derived cache entry that has just proved it cannot be read, and
            // the next Request renders it again.
            //
            // ONLY inside our own cache directory, and that guard is the whole reason IsOurGeneratedThumbnail
            // exists. This same Get() decodes the USER'S source images for the asset browser's texture
            // previews (FileExplorerPanel::DrawTextureThumbnail passes entry->AssetPath straight in), and an
            // unguarded remove() here would delete an artist's .png because stb could not read it.
            std::error_code removeEc;
            const bool      removed = std::filesystem::remove( sourcePath, removeEc );
            LOG_ERROR( "[Thumbnails] the cached thumbnail '{}' could not be decoded ({}); {}", sourcePath, failure,
                       removed ? "it was deleted and will be rendered again."
                               : "it could NOT be deleted (" + removeEc.message() +
                                      "), so this asset will show its type icon." );
        }
        else
        {
            // Somebody else's image (a texture the browser previews). Report and fall back to an icon;
            // never touch the file.
            LOG_ERROR( "[Thumbnails] '{}' could not be decoded ({}); the asset will show its type icon "
                       "instead of a preview.",
                       sourcePath, failure );
        }
        m_Cache[sourcePath] = result; // cache success or failure (null)
        if ( !stampEc )
            m_Stamps[sourcePath] = stamp;
        else
            m_Stamps.erase( sourcePath );
        return result;
    }

    void ThumbnailCache::Invalidate( const std::string& sourcePath )
    {
        m_Cache.erase( sourcePath );
        m_Stamps.erase( sourcePath );
    }

    void ThumbnailCache::Clear()
    {
        m_Cache.clear();
        m_Stamps.clear();
    }

    std::unordered_set<ThumbnailCache*>& ThumbnailCache::Live()
    {
        // Function-local so it is constructed before the first cache registers, whatever the translation
        // unit order is — three of the owners are themselves function-statics in other files.
        static std::unordered_set<ThumbnailCache*> s_Live;
        return s_Live;
    }

    ThumbnailCache::ThumbnailCache()
    {
        Live().insert( this );
    }

    ThumbnailCache::~ThumbnailCache()
    {
        Live().erase( this );
    }

    void ThumbnailCache::ReleaseAll()
    {
        // See the header. Clear(), not destroy: these caches outlive this call by design — the three that
        // matter are function-statics that will not be destroyed until the process ends — and what has to
        // go is the GPU image each one holds, not the map that held it.
        std::size_t images = 0;
        for ( ThumbnailCache* cache : Live() )
        {
            images += cache->m_Cache.size();
            cache->Clear();
        }

        LOG_INFO(
             "[Thumbnails] released {} cached image(s) from {} cache(s) on shutdown; decoded on main thread: {}.",
             images, Live().size(), ThumbnailPrefetch::Get().DecodedOnTheTakingThread() );
    }
} // namespace Desert::Editor
