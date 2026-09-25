#include "ThumbnailCache.hpp"

#include <Editor/Widgets/ThumbnailKey.hpp>

#include <Common/Content/DerivedDataCache.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>

#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>

#include <stb_image/stb_image.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
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
        std::error_code stampEc;
        const auto      stamp = std::filesystem::last_write_time( sourcePath, stampEc );
        if ( const auto it = m_Cache.find( sourcePath ); it != m_Cache.end() )
        {
            const auto seen = m_Stamps.find( sourcePath );
            if ( stampEc || ( seen != m_Stamps.end() && seen->second == stamp ) )
                return it->second; // may be null (decode previously failed)
            m_Cache.erase( it ); // the file was rewritten since it was decoded
        }

        if ( m_Cache.size() >= kMaxEntries )
        {
            m_Cache.clear(); // simple bound; thumbnails re-decode lazily
            m_Stamps.clear();
        }

        std::shared_ptr<Graphic::Image2D> result;

        // THE CACHE-HIT PATH IS NOT FREE, and it was the only part of the thumbnail system that had never
        // been timed. This runs on the main thread inside the ImGui pass, once per PNG per session, and it
        // is what a user sees as "it is computing it again" even when nothing is being rendered at all.
        const auto began = std::chrono::steady_clock::now();

        int      w = 0, h = 0, ch = 0;
        stbi_uc* pixels = stbi_load( sourcePath.c_str(), &w, &h, &ch, 4 );
        const auto decoded = std::chrono::steady_clock::now();
        if ( pixels && w > 0 && h > 0 )
        {
            // Box-average downscale to <= kThumbMaxDim. Rendered thumbnails are written AT that size since
            // v9, so this is a straight copy for them; it still earns its place for the other images this
            // cache decodes — textures and video posters the browser previews at their authored size,
            // where nearest-neighbour would alias and shimmer.
            const int maxSide = std::max( w, h );
            const int tw      = maxSide > kThumbMaxDim ? std::max( 1, w * kThumbMaxDim / maxSide ) : w;
            const int th      = maxSide > kThumbMaxDim ? std::max( 1, h * kThumbMaxDim / maxSide ) : h;

            std::vector<unsigned char> dst( static_cast<size_t>( tw ) * th * 4 );
            for ( int y = 0; y < th; ++y )
            {
                const int sy0 = y * h / th;
                const int sy1 = std::max( sy0 + 1, ( y + 1 ) * h / th );
                for ( int x = 0; x < tw; ++x )
                {
                    const int sx0 = x * w / tw;
                    const int sx1 = std::max( sx0 + 1, ( x + 1 ) * w / tw );

                    uint32_t acc[4] = { 0, 0, 0, 0 };
                    uint32_t n      = 0;
                    for ( int yy = sy0; yy < sy1; ++yy )
                        for ( int xx = sx0; xx < sx1; ++xx )
                        {
                            const unsigned char* s = pixels + ( static_cast<size_t>( yy ) * w + xx ) * 4;
                            acc[0] += s[0]; acc[1] += s[1]; acc[2] += s[2]; acc[3] += s[3];
                            ++n;
                        }
                    const uint32_t   div = std::max( 1u, n );
                    unsigned char*   d   = dst.data() + ( static_cast<size_t>( y ) * tw + x ) * 4;
                    d[0] = static_cast<unsigned char>( acc[0] / div );
                    d[1] = static_cast<unsigned char>( acc[1] / div );
                    d[2] = static_cast<unsigned char>( acc[2] / div );
                    d[3] = static_cast<unsigned char>( acc[3] / div );
                }
            }

            Core::Formats::Image2DSpecification spec = {
                 .Tag        = "Thumb_" + std::filesystem::path( sourcePath ).filename().string(),
                 .Width      = static_cast<uint32_t>( tw ),
                 .Height     = static_cast<uint32_t>( th ),
                 .Format     = Core::Formats::ImageFormat::RGBA8F,
                 .Mips       = 1u,
                 .Data       = std::move( dst ),
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

            const auto ms = []( auto from, auto to )
            { return std::chrono::duration<double, std::milli>( to - from ).count(); };
            LOG_DEBUG( "[Thumbnails] decoded '{}' {}x{} -> {}x{} in {:.0f} ms (png decode {:.0f}, "
                       "box filter + upload {:.0f})",
                       std::filesystem::path( sourcePath ).filename().string(), w, h, tw, th,
                       ms( began, std::chrono::steady_clock::now() ), ms( began, decoded ),
                       ms( decoded, std::chrono::steady_clock::now() ) );
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
            LOG_ERROR( "[Thumbnails] the cached thumbnail '{}' could not be decoded ({}); {}", sourcePath,
                       stbi_failure_reason() ? stbi_failure_reason() : "no reason given",
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
                       sourcePath, stbi_failure_reason() ? stbi_failure_reason() : "no reason given" );
        }
        if ( pixels )
            stbi_image_free( pixels );

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

        LOG_INFO( "[Thumbnails] released {} cached image(s) from {} cache(s) on shutdown.", images,
                  Live().size() );
    }
} // namespace Desert::Editor
