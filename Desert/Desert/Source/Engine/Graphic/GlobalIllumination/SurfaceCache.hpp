#pragma once

#include <Common/Settings/Scalability.hpp>
#include <Engine/Assets/MeshCardRepresentation.hpp>
#include <Engine/Core/Formats/ImageFormat.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// THE SURFACE CACHE — the interface (GI-CACHE-IFACE; owner 10-15 "surface cache — БЕРЁМ"). Port of UE Lumen's
// Lumen Scene: LumenSceneData.h (FLumenCard, FLumenMeshCards), LumenSurfaceCache.cpp (atlases), MeshCardBuild /
// MeshCardRepresentation.h (FMeshCardsBuildData), LumenCardCommon.ush and LumenSurfaceCacheSampling.ush (GPU
// side).
//
// WHAT IT IS. Every mesh carries a few CARDS: oriented boxes in mesh space, each facing one of the six axis
// directions, that together cover its surface. Each card owns pages of the surface-cache ATLASES, rasterised from
// its direction (the card capture: depth, albedo, opacity, normal, emissive), then LIT over frames: direct light
// of every light with its shadow, plus radiosity (the card's own hemisphere probes gathering the previous frame's
// final lighting = multi-bounce). A traced ray that hits geometry reads the card texels at the hit instead of
// running the material — the hit's colour is one atlas fetch. One surface cache serves both tracers (Graphic/
// GlobalIllumination/GITracing.hpp): the software one (global SDF, Mac and every GPU without ray tracing — the
// path most players run) and the hardware one (ray query, unless hit lighting is allowed).
//
// WHERE THIS DIFFERS FROM UE (GI-ALL-LIGHTS «ПЕРЕСМОТР 10-15», for the owner):
//   * A hit OUTSIDE card coverage is not black. UE returns an invalid FSurfaceCacheSample (bValid = false) and
//     the hit contributes no light; thin and concave meshes then leak darkness. Here the shared hit function
//     (Common/GITracing.glslh, GIHitRadiance) lights such a hit IN PLACE (hit lighting): the lights of the world
//     light grid with a shadow ray, shading the mesh's MeshCardsPayload::FallbackAlbedo, plus the previous
//     frame's radiance cache as its indirect term — never zero.
//   * A card's coverage is a BITMASK per direction (MeshCardsPayload::CardLookup, up to kMaxCardsPerMesh cards),
//     not UE's per-direction card range, so a mesh may carry more than one card per direction where a thin part
//     needs it (denser cards on thin meshes, owner 10-15).
//   * Positions are plain float (centimetres, world units). UE splits each card origin into a high/low pair for
//     Large World Coordinates (DFHackToFloat, "LUMEN_LWC_TODO"); this engine's worlds are float-positioned
//     everywhere else, so a split here would buy no precision the rest of the frame keeps.
namespace Desert::Graphic::SurfaceCache
{
    // ---- Page geometry (LumenSurfaceCacheSampling.ush:80-86, Lumen::PhysicalPageSize et al.) ------------------
    // A physical page is 128² texels; its VIRTUAL size is 127 because neighbouring pages of one card share a
    // one-texel border, so bilinear sampling never reads across a card edge into a stranger's texels.
    inline constexpr uint32_t kPhysicalPageSize  = 128;
    inline constexpr uint32_t kVirtualPageSize   = kPhysicalPageSize - 1;
    inline constexpr uint32_t kMinCardResolution = 8;
    inline constexpr uint32_t kMinResLevel       = 3;  // log2( kMinCardResolution )
    inline constexpr uint32_t kMaxResLevel       = 11; // 2048 texels per card side
    // log2( kPhysicalPageSize ): a card below it shares one physical page with others (sub-allocation).
    inline constexpr uint32_t kSubAllocationResLevel = 7;
    static_assert( ( 1u << kMinResLevel ) == kMinCardResolution );
    static_assert( ( 1u << kSubAllocationResLevel ) == kPhysicalPageSize );

    // The card directions, the per-mesh card limit and the built cards are the mesh asset's derived data
    // (Assets/MeshCardRepresentation.hpp, the card build): one home, named here for the GPU side.
    using Assets::CardBuildData;
    using Assets::CardDirection;
    using Assets::kCardDirections;
    using Assets::kMaxCardsPerMesh;
    using Assets::MeshCardsBuildData;

    // ---- The atlases (LumenSurfaceCacheSampling.ush:8-16; FLumenSceneData atlases) ----------------------------
    // Captured layers are written by the card capture (a raster of the mesh from the card's direction); lit layers
    // by the surface-cache lighting passes. One extent for all: SurfaceCacheBudget::AtlasSize texels per side.
    enum class Layer : uint32_t
    {
        // ---- captured
        Depth = 0, // card-space depth of the surface, 0 = card plane, 1 = far side of the card box
        Albedo,    // diffuse albedo, rgb; a = 1
        Opacity,   // coverage: 0 where the card ray finds no surface (outside coverage)
        Normal,    // octahedral world normal
        Emissive,  // material emissive, linear radiance
                  // the light the surface lets through to its far side (DesertSubsurfaceColor of a subsurface
                  // model, e.g. TwoSidedFoliage), rgb; a = its subsurface opacity (the transmission shadow's
                  // density). 0 = opaque.
        Subsurface,
        // ---- lit
        // every light with its shadow, as the diffuse radiance it leaves: albedo / PI x the front irradiance
        // (+N) + subsurface / PI x the back irradiance (-N, the light through a leaf)
        DirectLighting,
        IndirectLighting,           // radiosity: the same, of the radiance cache's irradiance at +N and -N
        FinalLighting,              // direct + indirect + emissive: what a traced hit reads
        RadiosityFramesAccumulated, // temporal count of the radiosity accumulation (UE NumFramesAccumulated)
        Count
    };
    inline constexpr std::size_t kLayerCount = static_cast<std::size_t>( Layer::Count );

    inline constexpr std::array<Core::Formats::ImageFormat, kLayerCount> kLayerFormats{
         Core::Formats::ImageFormat::R16_UNORM,  // Depth           (UE PF_G16)
         Core::Formats::ImageFormat::RGBA8F,     // Albedo          (UE PF_R8G8B8A8, BC7 when compressed)
         Core::Formats::ImageFormat::R8_UNORM,   // Opacity         (UE PF_G8)
         Core::Formats::ImageFormat::RG8_UNORM,  // Normal          (UE PF_R8G8)
         Core::Formats::ImageFormat::R11G11B10F, // Emissive        (UE PF_FloatR11G11B10)
         Core::Formats::ImageFormat::RGBA8F,     // Subsurface      (as Albedo)
         Core::Formats::ImageFormat::R11G11B10F, // DirectLighting  (UE PF_FloatR11G11B10)
         Core::Formats::ImageFormat::R11G11B10F, // IndirectLighting (UE PF_FloatR11G11B10)
         Core::Formats::ImageFormat::R11G11B10F, // FinalLighting   (UE PF_FloatR11G11B10)
         Core::Formats::ImageFormat::R8_UNORM,   // RadiosityFramesAccumulated (UE PF_G8)
    };

    // ---- The GPU records (std430; must match Common/SurfaceCache.glslh) --------------------------------------

    // One card of the scene (UE FLumenCardData, LUMEN_CARD_DATA_STRIDE = 7 float4). 96 bytes.
    struct CardPayload
    {
        // World -> card: rows of the rotation in xyz, the card origin (world, cm) in w of the three rows.
        glm::vec4 WorldToLocal[3]{ glm::vec4( 0.0f ), glm::vec4( 0.0f ), glm::vec4( 0.0f ) };
        glm::vec4 LocalExtent{ 0.0f }; // xyz = half size in card space (z = depth range); w = texel size (cm)
        // x = SizeInPages (x | y << 16), y = PageTableOffset, z = HiResSizeInPages (x | y << 16),
        // w = HiResPageTableOffset — the always-resident pages and the streamed high-resolution ones.
        glm::uvec4 Pages{ 0u };
        // x = packed: ResLevelToResLevelXYBias x (bits 0-7), y (8-15), direction (16-19), lighting channels
        // (20-23),
        //     visible (24), heightfield (25), far field (26)  — UE Packed0W, same bits;
        // y = index of the MeshCardsPayload it belongs to; z, w = 0.
        glm::uvec4 Info{ 0u };
    };
    static_assert( sizeof( CardPayload ) == 96 );

    // One page of a card mapped into the atlas (UE FLumenPageTableEntry / LUMEN_CARD_PAGE_DATA_STRIDE). 80 bytes.
    struct CardPagePayload
    {
        // x = card index, y = mapped (0/1), z = 1 when the card is captured this frame (its lit layers are not its
        // own yet), w = 0
        glm::uvec4 Card{ 0u };
        glm::vec4  CardUVRect{ 0.0f };          // the page's rectangle in card UV
        glm::vec4  PhysicalAtlasUVRect{ 0.0f }; // the same rectangle in atlas UV
        glm::vec4  TexelScale{ 0.0f };          // xy = CardUVTexelScale, zw = PhysicalAtlasUVTexelScale
        // x = frame of the last direct-lighting update, y = of the last radiosity update,
        // z = radiosity temporal index, w = direct-lighting temporal index (UE Last*UpdateFrameIndex)
        glm::uvec4 Updates{ 0u };
    };
    static_assert( sizeof( CardPagePayload ) == 80 );

    // One mesh instance's cards (UE FLumenMeshCardsData, LUMEN_MESH_CARDS_DATA_STRIDE = 6 float4). 112 bytes.
    struct MeshCardsPayload
    {
        glm::vec4 WorldToLocal[3]{ glm::vec4( 0.0f ), glm::vec4( 0.0f ), glm::vec4( 0.0f ) }; // as CardPayload
        // x = first CardPayload, y = card count, z = flags (bit 0 heightfield, bit 1 mostly two-sided), w = 0
        glm::uvec4 Cards{ 0u };
        // Per CardDirection, the mask of this mesh's cards facing it: [0].xyzw = directions 0..3, [1].xy = 4..5.
        glm::uvec4 CardLookup[2]{ glm::uvec4( 0u ), glm::uvec4( 0u ) };
        // rgb = what a hit OUTSIDE card coverage is shaded with (header note): the mean base colour of the mesh's
        // materials when the instance is registered, replaced by the mean captured card albedo after its first
        // capture; a = 0.
        glm::vec4 FallbackAlbedo{ 0.0f };
    };
    static_assert( sizeof( MeshCardsPayload ) == 112 );

    // ---- The instance table (contract B3, GI-TRACER-CONTRACT; agreed with the Windows side) ------------------
    // Every instance of the Lumen scene (a static mesh part, each instance of an ISM/HISM batch) holds ONE ROW of
    // this table for as long as it lives (SurfaceCacheScene: the row is allocated on add, freed on remove, reused
    // by the next add): the number a traced hit carries to find its cards. The hardware tracer writes the row as
    // the TLAS instance custom index (GIHit::instance); the software tracer, whose global distance field has no
    // object identity, finds the rows around its hit in the object grid below. A row whose instance has no cards
    // this frame (none built, none resident: small and far meshes) holds kNoMeshCards - the hit is lit in place.
    inline constexpr uint32_t kNoMeshCards = 0xFFFFFFFFu; // Common/GITracing.glslh GI_NO_MESH_CARDS
    inline constexpr uint32_t kNoInstance  = 0xFFFFFFFFu; // Common/GITracing.glslh GI_NO_INSTANCE

    // One row (std430, Common/SurfaceCacheSampling.glslh SurfaceCacheInstance). 48 bytes.
    struct InstanceRowPayload
    {
        // x = this frame's MeshCardsPayload index (the instance's card layout) or kNoMeshCards, y = the first
        // CardPagePayload of the instance's cards, z = their page count, w = 0.
        glm::uvec4 Cards{ kNoMeshCards, 0u, 0u, 0u };
        glm::vec4  BoundsMin{ 0.0f }; // world box of the instance's cards, cm (xyz)
        glm::vec4  BoundsMax{ 0.0f };
    };
    static_assert( sizeof( InstanceRowPayload ) == 48 );

    // THE OBJECT GRID — which rows a software hit may belong to (UE: the global distance field's page object grid,
    // r.LumenScene.GlobalSDF / GlobalDistanceFieldPageObjectGridBuffer, read by Lumen's global SDF hits to sample
    // the surface cache). A camera-centred grid of kObjectGridDims³ cells of kObjectGridCellSize cm, each listing
    // the rows whose card box overlaps it. One uint array (Common/SurfaceCacheSampling.glslh surfaceCacheGrid[]):
    //   [0..2]  the grid origin (cm, float bits), [3] the cell size (cm, float bits), [4] dims, [5..7] 0;
    //   [8 .. 8 + dims³)  per cell: first entry << 8 | entry count (count clamped to 255);
    //   then the entries: rows.
    // A hit outside the grid finds no row and is lit in place.
    inline constexpr uint32_t kObjectGridDims       = 64;
    inline constexpr float    kObjectGridCellSize   = 256.0f; // cm: the grid spans 164 m around the camera
    inline constexpr uint32_t kObjectGridHeader     = 8;
    inline constexpr uint32_t kObjectGridMaxPerCell = 255;

    // ---- The budget (Scalability, GlobalIllumination group: UE sg.GlobalIlluminationQuality) -----------------
    struct SurfaceCacheBudget
    {
        uint32_t AtlasSize                     = 0; // texels per atlas side, a multiple of kPhysicalPageSize
        uint32_t CardTexelDensityPercent       = 0; // card texels per world unit, percent of the reference
        uint32_t CardMinResolution             = 0; // smallest card side, texels
        uint32_t CardMaxResolution             = 0; // largest card side, texels
        uint32_t DirectLightingUpdateFactor    = 0; // 1/N of the card texels relit per frame
        uint32_t RadiosityUpdateFactor         = 0; // 1/N of the card texels re-gathered per frame
        uint32_t RadiosityProbeSpacing         = 0; // card texels between radiosity probes
        uint32_t RadiosityHemisphereResolution = 0; // radiosity probe side, texels

        bool operator==( const SurfaceCacheBudget& ) const = default;
    };

    // PURE. The budget of the machine's quality. The rows are Scalability placeholders until GI-SURFACE-CACHE
    // gives them this reader's pass (Scalability.hpp, "GlobalIllumination: the budgets ...").
    [[nodiscard]] inline SurfaceCacheBudget
    MakeSurfaceCacheBudget( const Common::Scalability::ResolvedQuality& quality )
    {
        using P = Common::Scalability::Parameter;
        SurfaceCacheBudget budget;
        budget.AtlasSize =
             quality.As<uint32_t>( P::SurfaceCacheAtlasSize ) / kPhysicalPageSize * kPhysicalPageSize;
        budget.CardTexelDensityPercent       = quality.As<uint32_t>( P::SurfaceCacheCardTexelDensity );
        budget.CardMinResolution             = quality.As<uint32_t>( P::SurfaceCacheCardMinResolution );
        budget.CardMaxResolution             = quality.As<uint32_t>( P::SurfaceCacheCardMaxResolution );
        budget.DirectLightingUpdateFactor    = quality.As<uint32_t>( P::SurfaceCacheDirectLightingUpdateFactor );
        budget.RadiosityUpdateFactor         = quality.As<uint32_t>( P::SurfaceCacheRadiosityUpdateFactor );
        budget.RadiosityProbeSpacing         = quality.As<uint32_t>( P::SurfaceCacheRadiosityProbeSpacing );
        budget.RadiosityHemisphereResolution = quality.As<uint32_t>( P::SurfaceCacheRadiosityProbeResolution );
        return budget;
    }

    // ---- Page arithmetic (LumenSurfaceCache.cpp: FLumenSurfaceCacheAllocator) ---------------------------------

    // Physical pages per atlas side.
    [[nodiscard]] constexpr uint32_t AtlasPagesPerSide( uint32_t atlasSize )
    {
        return atlasSize / kPhysicalPageSize;
    }

    // Texels of a card side at @p resLevel, clamped to [kMinResLevel, kMaxResLevel].
    [[nodiscard]] constexpr uint32_t ResLevelResolution( uint32_t resLevel )
    {
        return 1u << std::clamp( resLevel, kMinResLevel, kMaxResLevel );
    }

    // Virtual pages along a card side of @p resolution texels: below a page a card is SUB-ALLOCATED inside one
    // shared page (0 here = no page of its own); at or above, ceil( resolution / kVirtualPageSize ).
    [[nodiscard]] constexpr uint32_t PagesAlongSide( uint32_t resolution )
    {
        return resolution < kPhysicalPageSize ? 0u : ( resolution + kVirtualPageSize - 1u ) / kVirtualPageSize;
    }
} // namespace Desert::Graphic::SurfaceCache
