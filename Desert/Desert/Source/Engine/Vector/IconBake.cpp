#include "IconBake.hpp"

#include <Engine/Vector/VectorImage.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>
#include <Common/Content/DerivedDataCache.hpp>
#include <Common/Utilities/PakFile.hpp>

#include <cstring>
#include <format>
#include <fstream>

namespace Desert::Vector
{
    namespace
    {
        constexpr uint32_t kMagic = 0x4E434944u; // "DICN", little-endian

        template <typename T>
        void PutPod( std::vector<uint8_t>& out, const T& value )
        {
            const auto* p = reinterpret_cast<const uint8_t*>( &value );
            out.insert( out.end(), p, p + sizeof( T ) );
        }

        template <typename T>
        bool GetPod( const uint8_t*& cursor, const uint8_t* end, T& value )
        {
            if ( static_cast<size_t>( end - cursor ) < sizeof( T ) )
                return false;
            std::memcpy( &value, cursor, sizeof( T ) );
            cursor += sizeof( T );
            return true;
        }
    } // namespace

    BakedIcon BakeIconSdf( const uint8_t* svg, size_t size )
    {
        BakedIcon out;

        const VectorImage image = ParseSvg( reinterpret_cast<const char*>( svg ), size );
        if ( !image.Valid() )
            return out;
        out.Aspect = image.Height > 0.0f ? image.Width / image.Height : 1.0f;

        // One layer per COLOUR RUN. Consecutive shapes sharing a fill collapse into a single layer
        // (fewer quads); a new colour starts a new one, and document order is preserved so
        // overlapping paths still paint back-to-front exactly as the .svg says.
        size_t runStart = 0;
        for ( size_t i = 1; i <= image.Shapes.size(); ++i )
        {
            const bool endOfRun =
                 ( i == image.Shapes.size() ) || ( image.Shapes[i].FillRGBA != image.Shapes[runStart].FillRGBA );
            if ( !endOfRun )
                continue;

            std::vector<uint8_t> sdf = RasterizeSdf( image, kIconSize, kIconPadding, runStart, i );
            if ( !sdf.empty() )
                out.Layers.push_back( { std::move( sdf ), image.Shapes[runStart].FillRGBA } );
            runStart = i;
        }
        return out;
    }

    namespace
    {
        constexpr Common::DDC::Deriver kIconDeriver{
             "IconCache", ".dicon", { 0x91c6e2b04f7a3d58ULL, 0x0d7f35a9c2e81b46ULL } };
    } // namespace

    uint64_t IconCacheKey( const std::vector<uint8_t>& svg )
    {
        // Payload = the SVG's bytes; the one setting is the serialized format's version, so a format
        // bump makes every old .dicon unreachable rather than refused.
        const uint64_t formatVersion = kBakedIconCacheVersion;
        return Common::DDC::MakeKey( kIconDeriver, Common::Utils::PakContentHash( svg.data(), svg.size() ),
                                     &formatVersion, sizeof( formatVersion ) );
    }

    std::filesystem::path IconCachePath( uint64_t key )
    {
        return Common::DDC::PathFor( kIconDeriver, key );
    }

    std::vector<uint8_t> SerializeBakedIcon( const BakedIcon& icon )
    {
        std::vector<uint8_t> out;
        PutPod( out, kMagic );
        PutPod( out, kBakedIconCacheVersion );
        PutPod( out, icon.Aspect );
        PutPod( out, static_cast<uint32_t>( icon.Layers.size() ) );
        for ( const BakedIconLayer& layer : icon.Layers )
        {
            PutPod( out, layer.RGBA );
            PutPod( out, static_cast<uint32_t>( layer.Sdf.size() ) );
            out.insert( out.end(), layer.Sdf.begin(), layer.Sdf.end() );
        }
        return out;
    }

    bool DeserializeBakedIcon( const uint8_t* data, size_t size, BakedIcon& out )
    {
        out = BakedIcon{};

        const uint8_t* cursor = data;
        const uint8_t* end    = data + size;

        uint32_t magic = 0, version = 0, layerCount = 0;
        if ( !GetPod( cursor, end, magic ) || magic != kMagic )
            return false;
        if ( !GetPod( cursor, end, version ) || version != kBakedIconCacheVersion )
            return false;
        if ( !GetPod( cursor, end, out.Aspect ) || !GetPod( cursor, end, layerCount ) )
            return false;

        // Every layer of a well-formed bake is one fixed-size cell; anything else is corruption.
        constexpr uint32_t kCellTexels = kIconCellDim * kIconCellDim;
        for ( uint32_t i = 0; i < layerCount; ++i )
        {
            BakedIconLayer layer;
            uint32_t       sdfSize = 0;
            if ( !GetPod( cursor, end, layer.RGBA ) || !GetPod( cursor, end, sdfSize ) )
                return false;
            if ( sdfSize != kCellTexels || static_cast<size_t>( end - cursor ) < sdfSize )
                return false;
            layer.Sdf.assign( cursor, cursor + sdfSize );
            cursor += sdfSize;
            out.Layers.push_back( std::move( layer ) );
        }
        return out.Valid();
    }

    bool TryLoadBakedIcon( const std::filesystem::path& path, BakedIcon& out )
    {
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
                        return DeserializeBakedIcon( bytes.data(), bytes.size(), out );
                }
            }
        }

        // Then the mounted archive — a packaged game's cooked bakes live ONLY here. Not routed
        // through FileSystem::ReadByteFileContent: that primitive logs an error for a missing file,
        // and a cache miss is the normal cold-start case, not an error.
        if ( auto packed = Common::Utils::VFS::ReadFile( Common::DDC::PackagedPath( path ) ) )
            return DeserializeBakedIcon( reinterpret_cast<const uint8_t*>( packed->data() ), packed->size(), out );

        return false;
    }

    bool StoreBakedIcon( const std::filesystem::path& path, const BakedIcon& icon )
    {
        std::error_code ec;
        std::filesystem::create_directories( path.parent_path(), ec );

        // Write-then-rename (И2) — see ShaderSpirvCache::StoreCachedSpirv.
        const std::vector<uint8_t> bytes = SerializeBakedIcon( icon );
        return Common::Utils::FileSystem::WriteContentToFileAtomic(
                    path, std::string( reinterpret_cast<const char*>( bytes.data() ), bytes.size() ) )
             .IsSuccess();
    }
} // namespace Desert::Vector
