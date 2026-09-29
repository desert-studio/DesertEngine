#pragma once
// Editor-only (UE: LandscapeEditor's heightmap file formats): it decodes a SOURCE PNG through stb_image, and the
// runtime reads cooked landscape data only (Tests/Engine/RuntimeSourceDecoders holds that line).

#include <Engine/World/Landscape/LandscapeEditCache.hpp>
#include <Engine/World/Landscape/LandscapeGenerator.hpp>
#include <Engine/World/Landscape/LandscapeSculpt.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace Desert::World::Landscape
{
    /**
     * @file
     * @brief UE's heightmap Import / Export (Manage mode): a landscape's heights as one 16-bit greyscale PNG or
     *        one RAW (.r16 / .raw, little-endian uint16) file, and back.
     *
     * THE VALUES ARE THE STORED SAMPLES, UNCHANGED. A pixel is the uint16 a tile holds, in UE's encoding
     * (kLandscapeMidSample = 32768 is height zero, LandscapeHeightCm applies the Z scale) — exactly what UE
     * writes, so a heightmap exported from UE imports here at the same heights with the same Z scale, and an
     * export → import round trip is bit for bit. Nothing is normalised, stretched or re-quantised.
     *
     * THE LAYOUT. Pixel (x, y) is global sample (X1 + x, Z1 + y) of the exported rectangle: image X is world X,
     * image rows run along +Z, as UE maps image Y onto its landscape Y. A landscape of TilesX x TilesZ tiles of
     * Q quads is (TilesX · Q + 1) x (TilesZ · Q + 1) pixels — UE's "overall resolution".
     *
     * THE SIZE MUST MATCH. UE's importer offers the resolutions a file can take and refuses the mismatch
     * ("The heightmap file's resolution does not match the requested resolution"); resampling is a separate,
     * explicit choice in UE. Here a mismatch is refused, naming the file's size and the landscape's, and the
     * sizes a new landscape could have instead — never silently cropped, padded or scaled.
     *
     * WHAT IS REFUSED THAT UE ONLY WARNS ABOUT: an 8-bit PNG and a colour PNG. UE imports both "with lower
     * quality"; a heightmap tool that quietly turns 256 levels into a terrain is the silent fallback this
     * project does not ship, so the refusal names the bit depth / channel count instead.
     *
     * WHAT IS NOT PORTED: UE's RAW `.json` sidecar (width/height/bpp beside the file). A RAW file has no header;
     * importing into an existing landscape the size is the landscape's, and a NEW landscape from RAW infers a
     * square side from the byte count exactly as UE does without the sidecar (LandscapeFileFormatRaw.cpp,
     * GetRawResolution). A non-square RAW is refused with its byte count; use PNG, which carries its size.
     */

    enum class LandscapeHeightmapFormat : uint8_t
    {
        /// 16-bit greyscale PNG.
        Png16,
        /// Headerless little-endian uint16, row-major (.r16 / .raw).
        Raw16,
    };

    /// Width x height in samples.
    struct LandscapeHeightmapSize
    {
        uint32_t Width  = 0u;
        uint32_t Height = 0u;
    };

    struct LandscapeHeightmap
    {
        uint32_t Width  = 0u;
        uint32_t Height = 0u;
        /// Row-major, X fastest: Samples[y · Width + x].
        std::vector<uint16_t> Samples;
    };

    /// By extension, case-insensitive: .png → Png16, .r16 / .raw → Raw16. Refuses any other, naming it.
    Common::ResultStr<LandscapeHeightmapFormat> LandscapeHeightmapFormatOf( const std::filesystem::path& path );

    /// A valid 16-bit greyscale PNG (colour type 0, bit depth 16, zlib through stb's deflate, Sub filter).
    /// Refuses an empty map or one whose sample count is not Width · Height.
    Common::ResultStr<std::vector<unsigned char>> EncodeLandscapeHeightmapPng( const LandscapeHeightmap& map );

    /// Decodes a PNG; refuses anything that is not 16-bit single-channel greyscale, naming what it is.
    Common::ResultStr<LandscapeHeightmap> DecodeLandscapeHeightmapPng( std::span<const unsigned char> bytes );

    /// Little-endian uint16, row-major. Refuses an empty map or a sample count that is not Width · Height.
    Common::ResultStr<std::vector<unsigned char>> EncodeLandscapeHeightmapRaw( const LandscapeHeightmap& map );

    /// With @p expected, the byte count must be Width · Height · 2 exactly; without it, the side is inferred as
    /// UE infers it: a square whose area is bytes / 2. Refuses an odd byte count, a non-square file with no
    /// expected size, and a mismatch, naming the numbers.
    Common::ResultStr<LandscapeHeightmap>
    DecodeLandscapeHeightmapRaw( std::span<const unsigned char>               bytes,
                                 const std::optional<LandscapeHeightmapSize>& expected );

    /// Writes @p map in the format its extension names, atomically. Refuses an unknown extension or a failed
    /// write, naming the path.
    Common::BoolResultStr WriteLandscapeHeightmapFile( const std::filesystem::path& path,
                                                       const LandscapeHeightmap&    map );

    /// Reads a heightmap file in the format its extension names. With @p expected, a file of any other size is
    /// refused naming the path, the file's size and the expected one (for PNG as for RAW). A missing file is a
    /// refusal naming the path.
    Common::ResultStr<LandscapeHeightmap>
    ReadLandscapeHeightmapFile( const std::filesystem::path&                 path,
                                const std::optional<LandscapeHeightmapSize>& expected );

    /// The global sample rectangle of tiles (@p tileX1, @p tileZ1) .. (@p tileX2, @p tileZ2), both inclusive, with
    /// the shared edge rows: (tileX2 - tileX1 + 1) · Q + 1 samples wide.
    LandscapeSampleBounds LandscapeTileRangeSamples( const LandscapeRoot& root, int32_t tileX1, int32_t tileZ1,
                                                     int32_t tileX2, int32_t tileZ2 );

    /// The heights of @p rect as a heightmap (UE's Export: the whole landscape, or the selected tiles' bounding
    /// rectangle). Refuses, as the edit cache does, a rectangle with a sample no loaded tile stores, naming it.
    Common::ResultStr<LandscapeHeightmap> ReadLandscapeHeightmap( const LandscapeRoot&         root,
                                                                  LandscapeTileLookup          lookup,
                                                                  const LandscapeSampleBounds& rect );

    /**
     * @brief UE's Import into an existing landscape: replaces the heights of @p rect with @p map in one write, and
     *        returns the before/after record the caller turns into ONE undo step (WriteLandscapeHeights replays
     * it).
     *
     * Refuses — writing nothing — a map whose size is not @p rect's (naming both), or a rectangle the edit cache
     * refuses (an unloaded tile, a sample off the landscape). Seam samples are written into every tile that
     * stores them, so the tiles cannot disagree on an edge after an import.
     *
     * The map goes into edit layer @p layer.Layer (UE: Import writes the layer being edited), as that layer's
     * heights relative to mid; the tiles become the stack's merge. The record holds that layer's heights, so
     * WriteLandscapeHeights with the same @p layer replays it. A locked layer or a tile without edit layers is
     * refused, writing nothing.
     */
    Common::ResultStr<LandscapeStrokeRecord> ImportLandscapeHeightmap( const LandscapeRoot&            root,
                                                                       LandscapeTileLookup             lookup,
                                                                       const LandscapeSampleBounds&    rect,
                                                                       const LandscapeHeightmap&       map,
                                                                       const LandscapeEditLayerTarget& layer );

    /**
     * @brief UE's New Landscape → Import from File: a new landscape whose tile grid is the map's size.
     *
     * @p frame supplies the location, QuadsPerTile, spacing and Z scale (its TilesX / TilesZ and fill are
     * ignored: the map decides them). A map is (TilesX · Q + 1) x (TilesZ · Q + 1) samples; any other width or
     * height is refused, naming it and the two nearest sizes Q allows, as UE lists the valid resolutions.
     */
    Common::ResultStr<LandscapeGenerated> LandscapeFromHeightmap( const LandscapeGenerateSettings& frame,
                                                                  const LandscapeHeightmap&        map );
} // namespace Desert::World::Landscape
