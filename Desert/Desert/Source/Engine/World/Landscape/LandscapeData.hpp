#pragma once

#include <Common/Core/ResultStr.hpp>

#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Desert::World::Landscape
{
    /**
     * @file
     * @brief The landscape's height data: one CPU array of 16-bit samples per tile, and nothing else.
     *
     * THIS IS THE SINGLE SOURCE OF TRUTH FOR TERRAIN HEIGHT (landscape analysis, decision A2). The GPU R16
     * texture, the physics height field and the raycast all read FROM a tile; none of them is ever read
     * back into one. That is why the file knows nothing about rendering, entities or the editor — those
     * are consumers, and a consumer that could write here would make two sources.
     *
     * WHY THE UE ENCODING AND NOT FLOATS. A uint16 sample is half the memory of a float, is exactly the
     * texel the GPU samples (no conversion on upload, no drift between what physics and rendering see),
     * and is the format every heightmap tool on the market imports and exports. Taking UE's constants
     * rather than inventing our own means a 16-bit heightmap exported from Unreal round-trips through
     * this engine with no rescale.
     *
     * WHAT IS DELIBERATELY NOT HERE. The tile's placement in the world (origin, grid spacing, vertical
     * scale) belongs to the landscape ROOT that owns the tiles, exactly as UE keeps it on the actor
     * transform rather than in the heightmap — so it is a parameter of the sampling functions, not a
     * member, and it is not in the blob. Paint layers (weightmaps) are a separate task.
     */

    // ── The encoding ──────────────────────────────────────────────────────────────────────────────────
    //
    // UE:Engine/Source/Runtime/Landscape/Public/LandscapeDataAccess.h:13-14
    //     #define LANDSCAPE_ZSCALE (1.0f/128.0f)      #define LANDSCAPE_INV_ZSCALE 128.0f
    // UE:Engine/Source/Runtime/Landscape/Public/LandscapeDataAccess.h:26-27
    //     MaxValue = 65535;  MidValue = 32768.f;
    // UE:Engine/Source/Runtime/Landscape/Public/LandscapeDataAccess.h:29-37
    //     GetLocalHeight(h)  = (h - MidValue) * LANDSCAPE_ZSCALE
    //     GetTexHeight(z)    = uint16(RoundToInt(Clamp(z * LANDSCAPE_INV_ZSCALE + MidValue, 0, MaxValue)))
    //
    // "Local" height is in the landscape's own space; the actor's Z scale multiplies it into the world.
    // UE's world unit is the centimetre and so is ours, so the conversion to centimetres is the Z scale
    // itself and nothing else: heightCm = (sample - 32768) / 128 * ZScale. With UE's default Z scale of
    // 100 that is a range of -256 m .. +255.99 m in steps of 100/128 = 0.78125 cm.

    inline constexpr uint16_t kLandscapeMaxSample = 65535u;
    inline constexpr uint16_t kLandscapeMidSample = 32768u;

    /// Local units per sample step. UE:LandscapeDataAccess.h:13 (LANDSCAPE_ZSCALE).
    inline constexpr float kLandscapeLocalPerStep = 1.0f / 128.0f;

    /// Samples per local unit. UE:LandscapeDataAccess.h:14 (LANDSCAPE_INV_ZSCALE).
    inline constexpr float kLandscapeStepsPerLocal = 128.0f;

    /// The vertical scale a new landscape gets: centimetres per local unit. UE's default actor scale is
    /// (100, 100, 100); the Z component of it is this number.
    inline constexpr float kLandscapeDefaultZScale = 100.0f;

    /// The horizontal spacing a new landscape gets: centimetres between two neighbouring samples. UE's
    /// default actor X/Y scale of 100 on a one-unit quad grid is one metre.
    inline constexpr float kLandscapeDefaultSpacingCm = 100.0f;

    /// Height in the landscape's local space. UE:LandscapeDataAccess.h:29-32 (GetLocalHeight).
    /// Defined by Shaders/Common/LandscapeHeight.glslh — the text the terrain shader decodes its R16 copy
    /// with — so it is not constexpr: the formula has one home, and it is the one the GPU compiles.
    float LandscapeLocalHeight( uint16_t sample );

    /**
     * @brief The sample for a local height. UE:LandscapeDataAccess.h:34-37 (GetTexHeight).
     *
     * Clamps THEN rounds half-up, in that order, as UE does. The clamp is spelled as UE's FMath::Clamp
     * spells it — `x < lo ? lo : (x < hi ? x : hi)` — rather than std::clamp, because the two differ on
     * NaN: std::clamp hands NaN through to the integer cast, which is undefined behaviour; UE's form maps
     * NaN to the maximum, which is defined and is what a heightmap exported from UE would contain.
     */
    uint16_t LandscapeSampleFromLocal( float localHeight );

    /// World-space height offset in centimetres of a sample, for a landscape of vertical scale @p zScale.
    /// Same home as LandscapeLocalHeight.
    float LandscapeHeightCm( uint16_t sample, float zScale );

    /// The sample for a height offset in centimetres. @p zScale must be positive; the caller that owns
    /// the scale (the landscape root) is the one that validates it, see ValidateLandscapeFrame.
    uint16_t LandscapeSampleFromHeightCm( float heightCm, float zScale );

    // ── Placement ─────────────────────────────────────────────────────────────────────────────────────

    /**
     * @brief Where one tile's sample grid sits in the world. Y is up; samples lie on the XZ plane.
     *
     * Sample (0, 0) is at (OriginX, BaseY + height, OriginZ); sample (x, z) is SpacingCm * (x, z) from it.
     * Supplied by the owner of the tile rather than stored in it — see the file comment.
     */
    struct LandscapeFrame
    {
        float OriginX   = 0.0f;
        float OriginZ   = 0.0f;
        float BaseY     = 0.0f;
        float SpacingCm = kLandscapeDefaultSpacingCm;
        float ZScale    = kLandscapeDefaultZScale;
    };

    /// Refuses a frame the sampling maths cannot honour (non-positive or non-finite spacing or scale),
    /// naming the offending number.
    Common::BoolResultStr ValidateLandscapeFrame( const LandscapeFrame& frame );

    // ── Dirty rectangles ──────────────────────────────────────────────────────────────────────────────

    /**
     * @brief A half-open rectangle of SAMPLE indices: [X0, X1) × [Z0, Z1).
     *
     * Half-open so that an empty rectangle is expressible (X0 == X1) and two rectangles that share an edge
     * do not share a sample.
     *
     * A HEIGHT rectangle, and exactly that: the samples whose value changed. Anything DERIVED from heights
     * through a neighbourhood — normals, which read one sample either side — changes one sample further
     * out, and it is the deriving consumer's job to widen by its own stencil. Widening here would make the
     * rectangle wrong for the consumer that needs it exact: undo, which snapshots precisely what changed.
     */
    struct LandscapeRect
    {
        uint32_t X0 = 0u;
        uint32_t Z0 = 0u;
        uint32_t X1 = 0u;
        uint32_t Z1 = 0u;

        [[nodiscard]] uint32_t Width() const
        {
            return X1 - X0;
        }
        [[nodiscard]] uint32_t Depth() const
        {
            return Z1 - Z0;
        }
        [[nodiscard]] bool Empty() const
        {
            return X1 <= X0 || Z1 <= Z0;
        }
        [[nodiscard]] uint64_t Area() const
        {
            return Empty() ? 0u : static_cast<uint64_t>( Width() ) * Depth();
        }

        friend bool operator==( const LandscapeRect&, const LandscapeRect& ) = default;
    };

    // ── The tile ──────────────────────────────────────────────────────────────────────────────────────

    /// Smallest tile: one quad needs two samples a side.
    inline constexpr uint32_t kLandscapeMinTileSamples = 2u;

    /// Largest tile a side: 8192 quads, 128 MiB of samples — far above any tile a landscape is cut into.
    /// It exists because the dimensions come from a file: without it, a corrupt header asks for a 16 GiB
    /// allocation before the payload length check can refuse it.
    inline constexpr uint32_t kLandscapeMaxTileSamples = 8193u;

    /// How many Edit Layers the current format can carry. ZERO, by owner decision O4: Edit Layers are
    /// round two. The blob already has the count field (see kLandscapeTileContainerVersion), so a tile
    /// written today is a valid layered tile with no layers, and round two adds records rather than a
    /// migration. A count above this is refused on read, never ignored.
    inline constexpr uint32_t kLandscapeMaxEditLayers = 0u;

    /**
     * @brief Who reads a tile's dirty rectangles. Each consumer has its OWN list.
     *
     * Taking is destructive ("I now have everything up to here"), so two consumers sharing one list would
     * each see only the edits the other had not taken yet: the GPU upload and the physics heightfield would
     * silently diverge after the first stroke that landed between their two frames.
     */
    enum class LandscapeDirtyConsumer : uint8_t
    {
        Gpu,     ///< LandscapeECSSystem: the R16 heightmap copy.
        Physics, ///< LandscapeCollision: the Jolt heightfield.
    };
    inline constexpr size_t kLandscapeDirtyConsumerCount = 2u;

    class LandscapeTileData
    {
    public:
        /// An EMPTY tile: zero samples, no dirty rectangles. It exists because a ResultStr must be able to
        /// hand back something on a failed unwrap, and it is inert rather than plausible: every sample
        /// query answers nullopt, every region is refused, and encoding it is a verified caller defect.
        /// A real tile only ever comes from Create, FromSamples or DecodeLandscapeTile.
        LandscapeTileData() = default;

        /// A flat tile at height zero (every sample kLandscapeMidSample). Refuses dimensions outside
        /// [kLandscapeMinTileSamples, kLandscapeMaxTileSamples], naming them.
        static Common::ResultStr<LandscapeTileData> Create( uint32_t samplesX, uint32_t samplesZ );

        /// A tile from existing samples, row-major with X fastest. Refuses a size mismatch, naming both
        /// numbers.
        static Common::ResultStr<LandscapeTileData> FromSamples( uint32_t samplesX, uint32_t samplesZ,
                                                                 std::vector<uint16_t> samples );

        [[nodiscard]] uint32_t SamplesX() const
        {
            return m_SamplesX;
        }
        [[nodiscard]] uint32_t SamplesZ() const
        {
            return m_SamplesZ;
        }

        /// Row-major, X fastest, exactly SamplesX * SamplesZ entries — the layout an R16 upload of the whole
        /// tile takes with no row padding.
        [[nodiscard]] const std::vector<uint16_t>& Samples() const
        {
            return m_Samples;
        }

        /// The whole tile as a rectangle.
        [[nodiscard]] LandscapeRect Bounds() const
        {
            return { 0u, 0u, m_SamplesX, m_SamplesZ };
        }

        /// One sample. Out of range is a caller defect and is asserted, not clamped: a clamp here would
        /// hand back the edge height for a point off the tile, which is the silent wrong answer.
        [[nodiscard]] uint16_t Sample( uint32_t x, uint32_t z ) const;

        /// Writes one sample and marks it dirty. Writing the value already there changes nothing and
        /// dirties nothing — undo and upload are both about change, not about touch.
        void SetSample( uint32_t x, uint32_t z, uint16_t value );

        /// Copies @p rect out, row-major. Refuses a rectangle that is empty or leaves the tile.
        [[nodiscard]] Common::ResultStr<std::vector<uint16_t>> ReadRegion( const LandscapeRect& rect ) const;

        /// Writes @p values (row-major, rect.Area() entries) into @p rect and marks it dirty. Refuses an
        /// empty rectangle, one that leaves the tile, or a value count that does not match — naming the
        /// numbers — and in every refusal writes nothing.
        ///
        /// Marks the rectangle dirty only if a value actually changed, and then the WHOLE rectangle rather
        /// than the tight box of changed samples. The caller named the region it edited, and undo wants to
        /// restore that region as a unit; the tight box would be a second answer to the same question.
        Common::BoolResultStr WriteRegion( const LandscapeRect& rect, std::span<const uint16_t> values );

        /**
         * @brief Every region changed since @p consumer last called TakeDirtyRects, as disjoint-or-merged
         * rectangles. Every consumer is told about every change; see LandscapeDirtyConsumer.
         *
         * A NEW TILE IS WHOLLY DIRTY — from Create, FromSamples and Decode alike. The GPU copy of a tile
         * that has just come into existence holds nothing, so "what the GPU does not have yet" is the whole
         * tile; stating it that way gives the uploader exactly one path, dirty rectangles, rather than a
         * first-upload special case beside it.
         *
         * Overlapping or edge-touching rectangles are merged into their bounding box as they arrive, so a
         * brush stroke of a hundred dabs is one rectangle, not a hundred. Two far-apart edits stay two.
         */
        [[nodiscard]] const std::vector<LandscapeRect>& DirtyRects( LandscapeDirtyConsumer consumer ) const
        {
            return m_Dirty[static_cast<size_t>( consumer )];
        }

        /// Hands @p consumer its dirty list and clears it; the other consumers' lists are untouched.
        std::vector<LandscapeRect> TakeDirtyRects( LandscapeDirtyConsumer consumer );

    private:
        LandscapeTileData( uint32_t samplesX, uint32_t samplesZ, std::vector<uint16_t> samples );

        void MarkDirty( LandscapeRect rect );

        uint32_t                   m_SamplesX = 0u;
        uint32_t                   m_SamplesZ = 0u;
        std::vector<uint16_t>      m_Samples;
        std::array<std::vector<LandscapeRect>, kLandscapeDirtyConsumerCount> m_Dirty;
    };

    // ── Sampling ──────────────────────────────────────────────────────────────────────────────────────

    /**
     * @brief World height (cm) at world (x, z), bilinear between the four surrounding samples.
     *
     * BILINEAR because that is what the GPU's R16 fetch in the tessellation stage returns, and the CPU
     * answer must agree with the surface on screen; a triangle split would disagree with it by up to a
     * quarter of the cell's twist.
     *
     * The tile covers [OriginX, OriginX + (SamplesX-1)·Spacing] × the same in Z, BOTH edges inclusive:
     * the last row of one tile is the first row of the next (neighbouring tiles share their edge samples,
     * as UE's components do), so a point on
     * the seam belongs to both and must be answered by both. Outside that — nullopt, never the edge value.
     */
    std::optional<float> SampleLandscapeHeight( const LandscapeTileData& tile, const LandscapeFrame& frame,
                                                float worldX, float worldZ );

    /**
     * @brief The four tiles that share an edge with a tile, each null when it is not loaded (or is the
     * landscape's own edge). West = tile (x - 1, z), East = (x + 1, z), South = (x, z - 1), North = (x, z + 1).
     *
     * A present neighbour must have the tile's own sample counts — it belongs to the same root, and
     * CheckTileMatchesRoot holds both to QuadsPerTile + 1. A mismatch is a caller defect and verified.
     */
    struct LandscapeTileNeighbours
    {
        const LandscapeTileData* West  = nullptr;
        const LandscapeTileData* East  = nullptr;
        const LandscapeTileData* South = nullptr;
        const LandscapeTileData* North = nullptr;
    };

    /// Bit i set = neighbour i present, in the order West, East, South, North. The GPU receives exactly this
    /// number (TerrainInstance.Params2.w) and decodes it into the hasLow / hasHigh of LandscapeHeight.glslh.
    uint32_t LandscapeNeighbourMask( const LandscapeTileNeighbours& neighbours );

    /**
     * @brief The tile's samples with a one-sample ring from its neighbours: (SamplesX + 2) x (SamplesZ + 2),
     * row-major, X fastest; entry (x + 1, z + 1) is sample (x, z) for x in [-1, SamplesX], z likewise.
     *
     * The ring holds the row BEYOND the shared edge — the neighbour's second row, because its first is this
     * tile's last. It exists for one reader: the gradient at a border sample, which with it is a central
     * difference identical on both sides of the seam (LandscapeGradientLow/High). Where a neighbour is
     * absent, and at the four corners (no gradient reads them), the ring repeats the nearest own sample; the
     * neighbour mask says those entries are not to be differenced. This is the GPU heightmap's layout.
     */
    std::vector<uint16_t> LandscapeBorderedSamples( const LandscapeTileData&       tile,
                                                    const LandscapeTileNeighbours& neighbours );

    /**
     * @brief Unit surface normal (Y up) at world (x, z), or nullopt off the tile.
     *
     * The gradient is taken by central differences AT THE SAMPLES and then interpolated bilinearly, so the
     * normal is continuous across cells where the bilinear surface's own derivative is not. On a border
     * sample the difference reaches into the neighbour's ring row (LandscapeBorderedSamples), so two tiles
     * give the same bits at a point of their shared edge; it is one-sided only where the neighbour is
     * absent. The GPU evaluates the same functions of LandscapeHeight.glslh on the same samples.
     */
    std::optional<glm::vec3> SampleLandscapeNormal( const LandscapeTileData& tile, const LandscapeFrame& frame,
                                                    const LandscapeTileNeighbours& neighbours, float worldX,
                                                    float worldZ );

    // ── The blob ──────────────────────────────────────────────────────────────────────────────────────

    /// The four bytes every tile blob starts with: Desert Landscape Height Tile.
    inline constexpr char kLandscapeTileMagic[4] = { 'D', 'L', 'H', 'T' };

    /// The container layout's version. Bumped when a field moves.
    ///
    /// 1 — header: magic, version, samplesX, samplesZ, edit-layer count (must be 0, see
    ///     kLandscapeMaxEditLayers), payload byte length (u64); then the samples as little-endian uint16,
    ///     row-major, X fastest; then a trailer: CRC-32C of EVERYTHING before it.
    ///
    /// The checksum covers the header too, and sits at the end so it can: a flipped bit in a dimension
    /// that keeps the product (4 x 3 read as 3 x 4) passes every length check and decodes as a valid tile
    /// with its rows sheared — a terrain, not an error. Only a checksum over the header can refuse it.
    inline constexpr uint32_t kLandscapeTileContainerVersion = 1u;

    /// Byte lengths of the v1 header and trailer. Exposed so the round-trip test can assert the total
    /// size: a header that grew without this constant moving would pass a test that meant nothing.
    inline constexpr size_t kLandscapeTileHeaderSize  = 28u;
    inline constexpr size_t kLandscapeTileTrailerSize = 4u;

    /// Serialises a tile's samples. An empty (default-constructed) tile is a caller defect and verified. The
    /// result is exactly header + 2·SamplesX·SamplesZ + trailer bytes. Dirty state is not part of the blob — it
    /// describes the GPU copy, not the terrain.
    std::vector<unsigned char> EncodeLandscapeTile( const LandscapeTileData& tile );

    /**
     * @brief Parses a blob back into a tile, or says why it could not.
     *
     * REFUSES RATHER THAN GUESSES, and each refusal names the number that was wrong: magic, an unknown
     * version, dimensions out of range, a non-zero edit-layer count, a payload length that disagrees with
     * the dimensions, a truncated or over-long blob, a checksum mismatch. A heightmap decoded from wrong
     * bytes is not an error on screen, it is a mountain — the hardest shape of defect to trace back.
     */
    Common::ResultStr<LandscapeTileData> DecodeLandscapeTile( std::span<const unsigned char> bytes );
} // namespace Desert::World::Landscape
