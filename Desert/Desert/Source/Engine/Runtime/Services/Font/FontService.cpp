#include "FontService.hpp"

#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Runtime/Services/ServiceScanRoots.hpp>
#include <Engine/Text/FontCache.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace Desert::Runtime
{
    // Key/path/load/store live in Engine/Text/FontCache — the seam the game packager cooks into and
    // a packaged game reads back out of its mounted archive. They were private to this file once,
    // and the load was a raw ifstream: the packager could ship a warm cache this service never saw.

    Font* FontService::Get( const std::string& ttfPath, float pixelHeight )
    {
        const std::string key = ttfPath + '|' + std::to_string( static_cast<int>( pixelHeight ) );
        if ( auto it = m_Fonts.find( key ); it != m_Fonts.end() )
            return it->second.get();

        // A zero-byte .ttf is as unusable as a missing one, so both land in the same refusal.
        const auto ttfRead = Common::Utils::FileSystem::ReadByteFileContent( ttfPath );
        if ( !ttfRead || ttfRead.GetValue().empty() )
        {
            LOG_ERROR( "[FontService] Cannot read font '{}'", ttfPath );
            return nullptr;
        }
        const auto& ttf = ttfRead.GetValue();

        // Disk cache: skip the (CPU-bound) SDF bake if a matching atlas was cooked on a previous run.
        // Glyphs beyond ASCII this font was asked for. Sorted+unique, so the same set always yields the
        // same cache key regardless of the order the strings requested them in.
        std::vector<uint32_t> extra;
        if ( const auto it = m_ExtraGlyphs.find( ttfPath ); it != m_ExtraGlyphs.end() )
            extra = it->second;

        // Timed across the cache lookup AND the fallback bake, so the reported number is the same
        // quantity on a hit and on a miss: what this atlas cost the startup. Reading a cooked atlas
        // out of the mounted archive and rasterizing one from the .ttf differ by an order of
        // magnitude, and the difference is only visible if both are measured the same way.
        const auto atlasStart = std::chrono::steady_clock::now();

        const std::filesystem::path cachePath =
             Text::FontCachePath( Text::FontCacheKey( ttf, pixelHeight, extra ) );
        Text::BakedFont baked;
        const bool      fromCache = Text::TryLoadBakedFont( cachePath, baked );
        if ( !fromCache )
        {
            baked = Text::BakeFontForCache( ttf, pixelHeight, extra );
            if ( !baked.Valid() )
            {
                LOG_ERROR( "[FontService] Failed to bake SDF atlas for '{}'", ttfPath );
                return nullptr;
            }
            Text::StoreBakedFont( cachePath, baked );
        }

        const auto atlasMs =
             std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - atlasStart )
                  .count();

        // The baker already produces the atlas in the engine's one sampled format (RGBA8): RGB is the
        // multi-channel field, alpha is opaque. There is no channel expansion left to do here, and no
        // place left for a copy of the field to drift from the original.
        std::vector<unsigned char> rgba( baked.AtlasRGBA.begin(), baked.AtlasRGBA.end() );

        Core::Formats::Image2DSpecification spec = {
             .Tag    = "FontAtlas:" + key,
             .Width  = baked.AtlasWidth,
             .Height = baked.AtlasHeight,
             .Format = Core::Formats::ImageFormat::RGBA8F,
             // NO MIPS, AND NOT BY OMISSION. `GenerateMips` builds lower levels with linear blits, and
             // averaging the three channels of a multi-channel field destroys it: the median of averaged
             // channels is not the average of medians, so a minified glyph would reconstruct edges that
             // are in none of its outlines. There is no CPU-supplied mip-chain upload path to hand it a
             // correctly built chain instead. Minification is answered in the shader, where the edge ramp
             // is floored at one screen pixel (Common/SdfText.glslh); scored against a glyph's own
             // supersampled coverage, that is worst-pixel 0.22 at 5 px of rendered height and 0.21 at
             // 3 px — Desert/Tests/Engine/FontBaker has the numbers and the alternative it beat.
             .Mips       = 1,
             .Data       = std::move( rgba ),
             .Usage      = Core::Formats::Image2DUsage::Image2D,
             .Properties = Core::Formats::Sample };

        auto atlas = Graphic::Image2D::Create( spec, nullptr );
        if ( !atlas )
        {
            LOG_ERROR( "[FontService] GPU atlas upload failed for '{}'", ttfPath );
            return nullptr;
        }

        auto font   = std::make_unique<Font>();
        font->Atlas = std::move( atlas );
        font->Baked = std::move( baked );
        Font* raw   = font.get();
        m_Fonts[key] = std::move( font );
        LOG_INFO( "[FontService] {} '{}' @ {}px -> {}x{} atlas in {} ms", fromCache ? "Loaded cached" : "Baked",
                  ttfPath, static_cast<int>( pixelHeight ), raw->Baked.AtlasWidth, raw->Baked.AtlasHeight,
                  atlasMs );
        return raw;
    }

    void FontService::Clear()
    {
        m_Fonts.clear();
        m_HandleToPath.clear();
        m_ExtraGlyphs.clear();
        m_Retired.clear();
        m_Available.clear();
        m_Scanned = false;
    }

    uint64_t FontService::RegisterFont( const std::string& ttfPath )
    {
        if ( ttfPath.empty() )
            return 0;
        // FromCookedPath, not FromKey: a `.ttf` picked from the dropdown arrives as a project-rooted path
        // while one dropped onto the viewport arrives absolute, and hashing the raw string gave those two
        // spellings of ONE font two handles -- so a UIText that named the font one way stopped resolving
        // when the same font was registered the other way.
        const uint64_t handle = static_cast<uint64_t>( Common::AssetHandle::FromCookedPath( ttfPath ) );
        if ( m_HandleToPath.emplace( handle, ttfPath ).second )
            m_Available.push_back( ttfPath ); // first time we've seen this path -> offer it in the picker
        return handle;
    }

    std::string FontService::PathForHandle( uint64_t handle )
    {
        if ( handle == 0 )
            return "";
        if ( const auto it = m_HandleToPath.find( handle ); it != m_HandleToPath.end() )
            return it->second;
        // A saved scene may reference a font we haven't scanned yet (project asset). Fill the registry from
        // the font roots, then retry — the deterministic handle will match if the .ttf is discoverable.
        EnsurePreloaded();
        const auto it = m_HandleToPath.find( handle );
        return it == m_HandleToPath.end() ? "" : it->second;
    }

    Font* FontService::Get( uint64_t handle, float pixelHeight )
    {
        const std::string path = PathForHandle( handle );
        if ( path.empty() )
            return nullptr;
        return Get( path, pixelHeight );
    }

    uint64_t FontService::DefaultFontHandle()
    {
        // Roboto-Regular ships under the engine's (non-relocatable) FONTS_PATH, so this literal matches the
        // form the scan produces and both map to the same handle.
        static const std::string kDefault =
             ( Common::Constants::Path::FONTS_PATH / "Roboto-Regular.ttf" ).generic_string();
        return RegisterFont( kDefault );
    }

    bool FontService::RequestGlyphs( uint64_t handle, const std::vector<uint32_t>& codepoints )
    {
        const std::string path = PathForHandle( handle );
        if ( path.empty() )
            return false;

        auto& have  = m_ExtraGlyphs[path];
        bool  added = false;
        for ( uint32_t cp : codepoints )
        {
            if ( cp <= 126 ) // printable ASCII is always baked; control chars have no glyph
                continue;
            if ( std::find( have.begin(), have.end(), cp ) != have.end() )
                continue;
            have.push_back( cp );
            added = true;
        }
        if ( !added )
            return false;
        std::sort( have.begin(), have.end() ); // order-independent cache key

        // Drop every baked size of this font so the next Get() re-bakes with the new glyphs. The atlas it
        // replaces may still be in flight, so it is retired rather than destroyed.
        for ( auto it = m_Fonts.begin(); it != m_Fonts.end(); )
        {
            if ( it->first.rfind( path + '|', 0 ) == 0 )
            {
                if ( it->second && it->second->Atlas )
                    m_Retired.push_back( it->second->Atlas );
                it = m_Fonts.erase( it );
            }
            else
            {
                ++it;
            }
        }
        return true;
    }

    const std::vector<std::string>& FontService::AvailableFonts()
    {
        EnsurePreloaded();
        return m_Available;
    }

    void FontService::EnsurePreloaded()
    {
        if ( m_Scanned )
            return;
        m_Scanned = true;

        DefaultFontHandle(); // guarantee the built-in font is always offered, even if a root is missing

        // BOTH halves of the content world (loose files + the mounted .dpak), through the one shared
        // enumeration. This scan used to walk only the disk half, so in a packaged game — where the
        // loose directories do not exist — no .ttf was ever registered and a saved font handle could
        // not resolve to a path.
        for ( const auto* root : FontScanRoots() )
            for ( const auto& p : Common::Utils::FileSystem::ListFilesRecursive( *root ) )
                if ( p.extension() == ".ttf" )
                    RegisterFont( p.generic_string() );
        std::sort( m_Available.begin(), m_Available.end() );
        m_Available.erase( std::unique( m_Available.begin(), m_Available.end() ), m_Available.end() );
    }
} // namespace Desert::Runtime
