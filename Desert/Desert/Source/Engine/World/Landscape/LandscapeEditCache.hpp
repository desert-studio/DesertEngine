#pragma once
// Ported from UE 5.8 Engine/Source/Editor/LandscapeEditor/Private/LandscapeEdModeTools.h:283-745
// (TLandscapeEditCache) and Engine/Source/Runtime/Landscape/Public/LandscapeEdit.h:361-480 (FHeightmapAccessor),
// adapted: one height cache instead of a template over accessor and data type (weightmaps have no data here yet);
// the cache is a dense array over its bounding box instead of a TMap keyed by FIntPoint; the accessor reads and
// writes our per-tile blobs, where UE's FLandscapeEditDataInterface addresses one landscape-wide vertex grid;
// interpolated GetData, original-data snapshots, edit-layer visibility and foliage snapping are not ported (no
// edit layers, no foliage, undo is not this task); a region with no tile behind it is REFUSED, where UE caches
// zeros for it.

#include <Engine/World/Landscape/LandscapeData.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace Desert::World::Landscape
{
    /**
     * @file
     * @brief UE's TLandscapeEditCache for heights: a tool reads the samples under the brush across tile borders,
     *        changes them, and writes them back to every tile that stores them.
     *
     * COORDINATES. Everything here is on the root's GLOBAL sample lattice, as the brush's weights are
     * (LandscapeBrush.hpp): global index = tile · QuadsPerTile + local index. Rectangles are INCLUSIVE on both
     * ends, X1..X2 × Z1..Z2, because UE's cache API is and a tool ported from UE computes its bounds that way.
     *
     * SEAMS. A sample on a tile edge is stored in two tiles (four at a corner). The cache holds it ONCE, and
     * SetCachedData writes that one value into every stored copy, so the copies cannot disagree after a write.
     *
     * UPDATE PATH. Writing goes through LandscapeTileData::WriteRegion, which records dirty rectangles for every
     * LandscapeDirtyConsumer — the GPU heightmap upload and the Jolt heightfield both pick the edit up from
     * there on their next update; nothing else has to be called. ChangedTiles() is UE's ChangedComponents: which
     * tiles a stroke touched, for the tool that wants to know (undo, stats), not a second update path.
     */

    /// What a lookup knows about one tile coordinate.
    enum class LandscapeTileState : uint8_t
    {
        /// The landscape has no tile here: its edge. A seam sample whose other side is Absent is stored once.
        Absent,
        /// A tile exists here but its heights are not in memory. Writing a sample it shares would leave its
        /// copy stale, so any write touching it is refused, as UE refuses a stroke over an unloaded component.
        Unloaded,
        /// The heights are in memory and writable.
        Present,
    };

    struct LandscapeTileSlot
    {
        LandscapeTileState State = LandscapeTileState::Absent;
        /// Non-null exactly when State is Present.
        LandscapeTileData* Data = nullptr;
    };

    /// Answers what is at tile (tileX, tileZ) of the cache's root. The cache never owns a tile.
    using LandscapeTileLookup = std::function<LandscapeTileSlot( int32_t tileX, int32_t tileZ )>;

    /// One tile a write reached, and the union of the LOCAL sample rectangles written to it (half-open).
    struct LandscapeChangedTile
    {
        int32_t       TileX = 0;
        int32_t       TileZ = 0;
        LandscapeRect Samples;
    };

    class LandscapeHeightCache
    {
    public:
        LandscapeHeightCache( const LandscapeRoot& root, LandscapeTileLookup lookup );

        /**
         * @brief UE's CacheData: extends the cached region to the bounding box of itself and X1..X2 × Z1..Z2.
         *
         * A sample already cached keeps the cache's value (UE reads the new strips only), so what SetCachedData
         * stored survives the extension even if the tile changed behind the cache since.
         * Refuses — caching nothing — an inverted rectangle, or one with a sample that no Present tile stores
         * (off the landscape, or only in an Unloaded tile), naming the sample.
         */
        Common::BoolResultStr CacheData( int32_t x1, int32_t z1, int32_t x2, int32_t z2 );

        /// UE's GetCachedData: row-major, X fastest, (x2 - x1 + 1) · (z2 - z1 + 1) values. Refuses a rectangle
        /// that leaves the cached region — UE hands back zeros there, which is a height, not an answer.
        Common::ResultStr<std::vector<uint16_t>> GetCachedData( int32_t x1, int32_t z1, int32_t x2,
                                                                int32_t z2 ) const;

        /**
         * @brief UE's SetCachedData with bUpdateData: stores @p values in the cache and writes them into every
         *        tile that stores each sample.
         *
         * Refuses, writing NOTHING anywhere, when: the rectangle is inverted or leaves the cached region; the
         * value count does not match; a sample is stored by no Present tile; or any tile that stores one of the
         * samples is Unloaded. The refusal names the tile or the numbers.
         */
        Common::BoolResultStr SetCachedData( int32_t x1, int32_t z1, int32_t x2, int32_t z2,
                                             std::span<const uint16_t> values );

        /// Every tile SetCachedData has written to since construction or the last TakeChangedTiles, one entry
        /// per tile, in first-written order.
        const std::vector<LandscapeChangedTile>& ChangedTiles() const
        {
            return m_Changed;
        }
        std::vector<LandscapeChangedTile> TakeChangedTiles();

        bool IsValid() const
        {
            return m_Valid;
        }

    private:
        struct TileSpan
        {
            int32_t       TileX = 0;
            int32_t       TileZ = 0;
            LandscapeRect Local;
        };

        /// Every tile that stores a sample of X1..X2 × Z1..Z2, with the local half-open rectangle it stores.
        std::vector<TileSpan> TilesOf( int32_t x1, int32_t z1, int32_t x2, int32_t z2 ) const;
        Common::BoolResultStr ReadInto( int32_t x1, int32_t z1, int32_t x2, int32_t z2, int32_t cx1, int32_t cz1,
                                        int32_t cx2, std::vector<uint16_t>& cache ) const;
        bool                  Covers( int32_t x1, int32_t z1, int32_t x2, int32_t z2 ) const;
        void                  RecordChanged( int32_t tileX, int32_t tileZ, const LandscapeRect& local );

        LandscapeRoot       m_Root;
        LandscapeTileLookup m_Lookup;

        bool                  m_Valid = false;
        int32_t               m_X1    = 0;
        int32_t               m_Z1    = 0;
        int32_t               m_X2    = -1;
        int32_t               m_Z2    = -1;
        std::vector<uint16_t> m_Data;

        std::vector<LandscapeChangedTile> m_Changed;
    };
} // namespace Desert::World::Landscape
