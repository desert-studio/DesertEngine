#include "FontCache.hpp"

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

#include <format>
#include <fstream>

namespace Desert::Text
{
    uint64_t FontCacheKey( const std::vector<uint8_t>& ttf, float pixelHeight,
                           const std::vector<uint32_t>& extraCodepoints )
    {
        constexpr uint64_t kFnvOffset = 1469598103934665603ull;
        constexpr uint64_t kFnvPrime  = 1099511628211ull;

        uint64_t h = kFnvOffset;
        h ^= static_cast<uint64_t>( static_cast<int>( pixelHeight ) );
        h *= kFnvPrime;
        h ^= static_cast<uint64_t>( kGlyphPadding );
        h *= kFnvPrime;
        h ^= static_cast<uint64_t>( kAtlasWidth );
        h *= kFnvPrime;
        h ^= static_cast<uint64_t>( kDistanceRangeTexels * 256.0f );
        h *= kFnvPrime;
        for ( uint8_t b : ttf )
        {
            h ^= b;
            h *= kFnvPrime;
        }
        for ( uint32_t cp : extraCodepoints ) // a different glyph set is a different atlas
        {
            h ^= cp;
            h *= kFnvPrime;
        }
        return h;
    }

    std::filesystem::path FontCachePath( uint64_t key )
    {
        return Common::Constants::Path::COOKED_PATH / "FontCache" / std::format( "{:016x}.dfont", key );
    }

    BakedFont BakeFontForCache( const std::vector<uint8_t>& ttf, float pixelHeight,
                                const std::vector<uint32_t>& extraCodepoints )
    {
        // Every parameter this bake has is a named constant in FontBaker.hpp and every one of them is
        // hashed into FontCacheKey above, so a bake with different parameters cannot land under a key
        // that already means something else.
        return BakeFontMSDF( ttf.data(), ttf.size(), pixelHeight, extraCodepoints );
    }

    bool TryLoadBakedFont( const std::filesystem::path& path, BakedFont& out )
    {
        // A decode that failed for a reason worth a human's attention is named here, once, for whichever
        // of the two sources produced the bytes. A MISS is silent because a cold cache is the normal
        // cold-start case; a file that exists and cannot be used is not a miss, it is a refusal.
        const auto decode = [&]( const uint8_t* data, size_t size, const char* source ) -> bool
        {
            uint32_t   fileVersion = 0;
            const auto status      = DeserializeBakedFont( data, size, out, &fileVersion );
            switch ( status )
            {
                case FontDecodeStatus::Ok:
                    // A well-formed atlas of the right generation can still have been baked with a
                    // different distance band than the shader is compiled for — the one bake parameter
                    // that also exists on the GPU side. Refuse it by name rather than draw text whose
                    // edge width is quietly wrong everywhere.
                    if ( out.DistanceRangeTexels != kDistanceRangeTexels )
                    {
                        LOG_ERROR( "[FontCache] {} '{}' was baked with a {} texel distance band and this "
                                   "build renders {} — refusing it and re-baking",
                                   source, path.string(), out.DistanceRangeTexels, kDistanceRangeTexels );
                        return false;
                    }
                    return true;
                case FontDecodeStatus::VersionMismatch:
                    LOG_ERROR( "[FontCache] {} '{}' is a v{} font atlas and this build reads v{} — refusing "
                               "it and re-baking",
                               source, path.string(), fileVersion, kBakedFontCacheVersion );
                    return false;
                case FontDecodeStatus::BadMagic:
                    LOG_ERROR( "[FontCache] {} '{}' ({} bytes) is not a font atlas — refusing it and re-baking",
                               source, path.string(), size );
                    return false;
                case FontDecodeStatus::Corrupt:
                    LOG_ERROR( "[FontCache] {} '{}' ({} bytes) is a truncated or inconsistent v{} font atlas "
                               "— refusing it and re-baking",
                               source, path.string(), size, fileVersion );
                    return false;
            }
            return false;
        };

        // Loose file first — the dev override, and where this very run's bakes land.
        {
            std::error_code ec;
            const auto      size = std::filesystem::file_size( path, ec );
            if ( !ec && size > 0 )
            {
                std::ifstream in( path, std::ios::binary );
                if ( in )
                {
                    std::vector<uint8_t> bytes( static_cast<size_t>( size ) );
                    in.read( reinterpret_cast<char*>( bytes.data() ), static_cast<std::streamsize>( size ) );
                    if ( in )
                        return decode( bytes.data(), bytes.size(), "loose" );
                }
            }
        }

        // Then the mounted archive — a packaged game's cooked atlases live ONLY here. Not routed
        // through FileSystem::ReadByteFileContent: that primitive logs an error for a missing file,
        // and a cache miss is the normal cold-start case, not an error.
        if ( auto packed = Common::Utils::VFS::ReadFile( path ) )
            return decode( reinterpret_cast<const uint8_t*>( packed->data() ), packed->size(), "packaged" );

        return false;
    }

    bool StoreBakedFont( const std::filesystem::path& path, const BakedFont& font )
    {
        std::error_code ec;
        std::filesystem::create_directories( path.parent_path(), ec );

        // Write-then-rename (И2) — see ShaderSpirvCache::StoreCachedSpirv: a half-written atlas under
        // a key that claims to be valid is worse than no atlas at all.
        const std::vector<uint8_t> bytes = SerializeBakedFont( font );
        return Common::Utils::FileSystem::WriteContentToFileAtomic(
                    path, std::string( reinterpret_cast<const char*>( bytes.data() ), bytes.size() ) )
             .IsSuccess();
    }
} // namespace Desert::Text
