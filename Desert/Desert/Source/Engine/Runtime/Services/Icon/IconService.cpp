#include "IconService.hpp"

#include <Engine/Core/Formats/SdfAtlasEncoding.hpp>

#include <Engine/Runtime/Services/ServiceScanRoots.hpp>
#include <Engine/Vector/IconBake.hpp>
#include <Engine/Vector/VectorImage.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/AssetPathIndex.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace Desert::Runtime
{
    namespace
    {
        // Bake resolution/padding live in Engine/Vector/IconBake (Vector::kIconSize et al.) — the
        // bake is shared with the game packager's cook, so its parameters cannot be private here.
        // What stays local is the ATLAS page: a property of the running icon set, not of one file.
        constexpr uint32_t kAtlasStart = 256;  // grows by doubling as icons are imported
        constexpr uint32_t kAtlasMax   = 2048; // 16 MB RGBA8 — hundreds of icons; refuse rather than crawl
        constexpr uint32_t kSpacing    = 1;    // gutter so bilinear sampling never pulls in a neighbour
    } // namespace

    uint64_t IconService::RegisterIcon( const std::string& svgPath )
    {
        if ( svgPath.empty() )
            return 0;
        // FromCookedPath, not FromKey -- one file is one handle whatever spelling registered it. See the
        // note in FontService::RegisterFont; the three path-keyed services agree on this.
        const uint64_t handle = static_cast<uint64_t>( Common::AssetHandle::FromCookedPath( svgPath ) );

        // Deduplicated on the CANONICAL path, for the reason FontService::RegisterFont states beside
        // the same line: the handle map that used to answer "have I seen this file" is gone, and the
        // index that replaced it answers it through the key the call above has just recorded.
        const std::string canonical = Common::AssetPathIndex::PathFor( handle ).generic_string();
        if ( !canonical.empty() &&
             std::find( m_Available.begin(), m_Available.end(), canonical ) == m_Available.end() )
        {
            m_Available.push_back( canonical );
        }
        return handle;
    }

    std::string IconService::PathForHandle( uint64_t handle )
    {
        if ( handle == 0 )
            return "";

        // From `Common::AssetPathIndex`, which `FromCookedPath` records; the private second table this
        // replaces died with `Clear()`, exactly as the font service's did.
        if ( const std::string known = Common::AssetPathIndex::PathFor( handle ).generic_string(); !known.empty() )
        {
            return known;
        }

        // A saved scene may reference an icon nothing has derived a handle for yet. Scanning is what
        // mints those handles, and minting is what records the inverse.
        EnsurePreloaded();
        return Common::AssetPathIndex::PathFor( handle ).generic_string();
    }

    Icon* IconService::Get( uint64_t handle )
    {
        const std::string path = PathForHandle( handle );
        if ( path.empty() )
            return nullptr;
        if ( const auto it = m_Icons.find( path ); it != m_Icons.end() )
            return it->second.get();

        auto  icon    = std::make_unique<Icon>();
        Icon* raw     = icon.get();
        m_Icons[path] = std::move( icon ); // insert first: a failed import negative-caches itself

        const auto svgRead = Common::Utils::FileSystem::ReadByteFileContent( path );
        if ( !svgRead || svgRead.GetValue().empty() )
        {
            LOG_ERROR( "[IconService] Cannot read icon '{}'", path );
            return raw;
        }
        const auto& svgFile = svgRead.GetValue();

        // Disk cache first: the parse + per-layer SDF rasterization is the expensive half of an icon
        // import, and a packaged game ships it pre-baked in Cooked/IconCache (read VFS-aware, so the
        // archive serves it when no loose file exists). A miss bakes and stores, same as fonts.
        // Timed across the lookup AND the fallback bake, so a hit and a miss report the same
        // quantity: what this icon cost the startup (see FontService::Get for the same pattern).
        const auto bakeStart = std::chrono::steady_clock::now();

        const std::filesystem::path cachePath = Vector::IconCachePath( Vector::IconCacheKey( svgFile ) );
        Vector::BakedIcon           baked;
        const bool                  fromCache = Vector::TryLoadBakedIcon( cachePath, baked );
        if ( !fromCache )
        {
            baked = Vector::BakeIconSdf( svgFile.data(), svgFile.size() );
            if ( !baked.Valid() )
            {
                LOG_ERROR( "[IconService] '{}' has no filled shapes this importer understands", path );
                return raw;
            }
            Vector::StoreBakedIcon( cachePath, baked );
        }

        const auto bakeMs =
             std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - bakeStart )
                  .count();
        raw->Aspect = baked.Aspect;

        const size_t firstBitmap = m_Bitmaps.size();
        for ( Vector::BakedIconLayer& layer : baked.Layers )
        {
            const uint32_t rgba = layer.RGBA;

            LayerBitmap lb;
            lb.Sdf   = std::move( layer.Sdf );
            lb.Dim   = Vector::kIconCellDim;
            lb.RGBA  = rgba;
            lb.Owner = path;
            m_Bitmaps.push_back( std::move( lb ) );
            raw->Layers.push_back( IconLayer{ 0.0f, 0.0f, 1.0f, 1.0f, rgba } );
        }

        // THE REPACK IS THE OTHER HALF OF WHAT AN IMPORT COSTS, and it was not in the number above.
        // Every import rebuilds the WHOLE page — a dim*dim*4 CPU buffer, one Image2D::Create upload,
        // and the old page retired for the session — so N icons cost N repacks, the last of which is
        // the biggest. The line below said "this icon cost the startup X ms" while reporting only the
        // parse/cache half; the two are now separate numbers, because they answer different questions
        // (a cache hit removes the first and never the second).
        const auto repackStart = std::chrono::steady_clock::now();
        const bool packed      = RepackAtlas();
        const auto repackUs    = std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - repackStart )
                                   .count();
        if ( !packed )
        {
            m_Bitmaps.resize( firstBitmap ); // roll the new runs back out so the atlas stays consistent
            raw->Layers.clear();
            RepackAtlas();
            return raw;
        }
        LOG_INFO( "[IconService] {} '{}' ({} layer(s)) into the {}x{} icon atlas in {} ms + {} us repacking "
                  "({} layer(s) on the page)",
                  fromCache ? "Loaded cached" : "Imported", path, raw->Layers.size(), m_AtlasSize, m_AtlasSize,
                  bakeMs, repackUs, m_Bitmaps.size() );
        return raw;
    }

    bool IconService::RepackAtlas()
    {
        if ( m_Bitmaps.empty() )
        {
            m_Atlas.reset();
            m_AtlasSize = 0;
            return true;
        }

        // Every cell is the same size, so the shelf packer degenerates into a grid: find the smallest
        // power-of-two page that holds them all, growing by doubling.
        const uint32_t stride = Vector::kIconCellDim + kSpacing;
        uint32_t       dim    = std::max( kAtlasStart, m_AtlasSize );
        uint32_t       perRow = 0;
        while ( true )
        {
            perRow              = dim / stride;
            const uint32_t rows = perRow ? ( static_cast<uint32_t>( m_Bitmaps.size() ) + perRow - 1 ) / perRow : 0;
            if ( perRow > 0 && rows * stride <= dim )
                break;
            if ( dim >= kAtlasMax )
            {
                LOG_ERROR( "[IconService] Icon atlas is full at {}x{} ({} layers) — icon not imported", dim, dim,
                           m_Bitmaps.size() );
                return false;
            }
            dim *= 2;
        }

        // RGB carries the distance field, REPLICATED across all three channels. The UI text shader now
        // reconstructs a glyph with the median of its three channels (Common/SdfText.glslh); the median of
        // three equal channels is that one field, so an icon decodes exactly as it did when the shader
        // sampled .r — no branch, no second shader, and an icon costs nothing for text's corners.
        // ALPHA is free, so it gets a sharpened coverage mask: any plain alpha-blended draw — the editor's
        // Details preview — then shows the icon's real silhouette rather than a soft grey blob.
        std::vector<unsigned char> rgba( static_cast<size_t>( dim ) * dim * 4, 0 );
        const auto                 edge = static_cast<float>( Core::Formats::kSdfAtlasOnEdgeByte );
        // Bytes per texel of distance, from the ONE encoding both atlases use — not from this atlas's own
        // padding, which is a packing decision and was never the same number.
        const float perTexel = 255.0F / Core::Formats::kSdfAtlasDistanceRangeTexels;

        for ( size_t i = 0; i < m_Bitmaps.size(); ++i )
        {
            const LayerBitmap& lb = m_Bitmaps[i];
            const uint32_t     ox = static_cast<uint32_t>( i % perRow ) * stride;
            const uint32_t     oy = static_cast<uint32_t>( i / perRow ) * stride;
            for ( uint32_t y = 0; y < lb.Dim; ++y )
                for ( uint32_t x = 0; x < lb.Dim; ++x )
                {
                    const uint8_t v   = lb.Sdf[static_cast<size_t>( y ) * lb.Dim + x];
                    const float   cov = ( static_cast<float>( v ) - edge ) * ( 255.0f / perTexel ) + 128.0f;
                    const size_t  d   = ( static_cast<size_t>( oy + y ) * dim + ( ox + x ) ) * 4;
                    rgba[d + 0]       = v;
                    rgba[d + 1]       = v;
                    rgba[d + 2]       = v;
                    rgba[d + 3]       = static_cast<unsigned char>( std::clamp( cov, 0.0f, 255.0f ) );
                }
        }

        Core::Formats::Image2DSpecification spec = { .Tag        = "IconAtlas",
                                                     .Width      = dim,
                                                     .Height     = dim,
                                                     .Format     = Core::Formats::ImageFormat::RGBA8F,
                                                     .Mips       = 1,
                                                     .Data       = std::move( rgba ),
                                                     .Usage      = Core::Formats::Image2DUsage::Image2D,
                                                     .Properties = Core::Formats::Sample };

        auto atlas = Graphic::Image2D::Create( spec );
        if ( !atlas )
        {
            LOG_ERROR( "[IconService] GPU upload failed for the {}x{} icon atlas", dim, dim );
            return false;
        }
        if ( m_Atlas )
            m_Retired.push_back( std::move( m_Atlas ) ); // outlive any in-flight frame that still cites it
        m_Atlas     = std::move( atlas );
        m_AtlasSize = dim;

        // Re-address every icon's layers into the new page. An icon's colour runs were pushed
        // consecutively, so finding its first cell is enough to walk them in order.
        const float inv = 1.0f / static_cast<float>( dim );
        for ( const auto& [path, icon] : m_Icons )
        {
            size_t cell = 0;
            while ( cell < m_Bitmaps.size() && m_Bitmaps[cell].Owner != path )
                ++cell;
            for ( IconLayer& layer : icon->Layers )
            {
                if ( cell >= m_Bitmaps.size() )
                    break;
                const uint32_t ox = static_cast<uint32_t>( cell % perRow ) * stride;
                const uint32_t oy = static_cast<uint32_t>( cell / perRow ) * stride;
                layer.U0          = static_cast<float>( ox ) * inv;
                layer.V0          = static_cast<float>( oy ) * inv;
                layer.U1          = static_cast<float>( ox + Vector::kIconCellDim ) * inv;
                layer.V1          = static_cast<float>( oy + Vector::kIconCellDim ) * inv;
                ++cell;
            }
        }
        return true;
    }

    const std::vector<std::string>& IconService::AvailableIcons()
    {
        EnsurePreloaded();
        return m_Available;
    }

    void IconService::Clear()
    {
        // `m_HandleToPath.clear()` stood here — see FontService::Clear for the defect shape both
        // services carried: a payload cache that also owned the only handle -> path binding, so
        // clearing the payloads destroyed the identity.
        m_Icons.clear();
        m_Bitmaps.clear();
        m_Available.clear();
        m_Atlas.reset();
        m_Retired.clear();
        m_AtlasSize = 0;
        m_Scanned   = false;
    }

    void IconService::EnsurePreloaded()
    {
        if ( m_Scanned )
            return;
        m_Scanned = true;

        // BOTH halves of the content world (loose files + the mounted .dpak), through the one shared
        // enumeration — same defect and same fix as FontService::EnsurePreloaded: the disk-only scan
        // registered nothing in a packaged game.
        for ( const auto* root : IconScanRoots() )
            for ( const auto& p : Common::Utils::FileSystem::ListFilesRecursive( *root ) )
                if ( p.extension() == ".svg" )
                    RegisterIcon( p.generic_string() );
        std::sort( m_Available.begin(), m_Available.end() );
        m_Available.erase( std::unique( m_Available.begin(), m_Available.end() ), m_Available.end() );
    }
} // namespace Desert::Runtime
