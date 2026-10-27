#pragma once

// A STATIC MESH'S CARDS (UE FCardRepresentationData / FMeshCardsBuildData, MeshCardBuild.h; built by
// FMeshUtilities::GenerateCardRepresentationData, MeshCardRepresentationUtilities.cpp). A few oriented boxes in
// mesh space, each facing one of the six axis directions, that together cover the mesh's surface: the surface
// cache (Graphic/GlobalIllumination/SurfaceCache.hpp) rasterises the mesh from each card's direction into its
// atlas pages and lights those texels, so a traced ray that hits the mesh reads its light in one fetch.
//
// DERIVED DATA, in its own DDC bucket (UE CARDREPRESENTATION_DERIVEDDATA_VER), keyed on the render data's key
// plus the build settings and this builder's version, built on the distance field's worker queue in the same
// job (StaticMeshAsset::ResolveDependencies) from the same triangles.
//
// WHERE THIS DIFFERS FROM UE (owner 10-15, "плотнее карточки на тонких мешах, чем у UE"):
//   * UE voxelises the mesh box with ONE cubic voxel (>= 10 cm, at most 64 along the longest side), so a 20 m
//     kerb 15 cm high gets 31 cm voxels: its faces are half a voxel, their surfels half covered, and the card
//     covering the kerb's side is a 31 cm strip standing over a 15 cm face. Here the voxel is PER AXIS: each axis
//     gets its own size (>= kMinVoxelSize, at least kMinVoxelsAcrossThinAxis voxels across it, at most MaxVoxels
//     along it), so the thin axis is resolved as finely as the long ones need.
//   * UE's cluster thresholds are in voxel units of that one voxel; here they stay in voxel units, so a card is
//     accepted by the same density/coverage rule, measured on the finer lattice.
//   * As UE, the lattice is capped at MaxVoxels (64) along the longest side and halved while its surfels exceed
//     TargetSurfels; UE re-casts every surfel's visibility rays on each halving, here the budget counts the
//     lattice's candidates and the hemisphere rays are cast once, on the chosen lattice (BISTRO-LOOK p5: a
//     grown 256 cap re-cast per halving cost 20-30 s per Bistro mesh).
//   * UE takes a near plane's surfels as ONE cluster over the whole face of the box, dropped when sparser than
//     MinDensity of that face; a large mesh is sparse in its own box (a cross of lanes in a square). Here a slab
//     too sparse for one card is cut into its connected regions (MeshCardBuild.cpp ValidRegions).

#include <Common/Core/ResultStr.hpp>
#include <Engine/Assets/MeshDistanceField.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Assets
{
    // The six directions a card faces, UE AxisAlignedDirectionIndex order: axis = index / 2, sign = index % 2
    // (0 = towards negative).
    enum class CardDirection : uint32_t
    {
        NegativeX = 0,
        PositiveX,
        NegativeY,
        PositiveY,
        NegativeZ,
        PositiveZ,
        Count
    };
    inline constexpr uint32_t kCardDirections = static_cast<uint32_t>( CardDirection::Count );

    // UE MeshCardGen::MaxCardsPerMesh; also the width of the per-direction coverage mask on the GPU.
    inline constexpr uint32_t kMaxCardsPerMesh = 32;

    // One card (UE FLumenCardBuildData): an axis-aligned box of mesh space, centimetres; it faces Direction, so
    // its depth runs along that axis from the face Direction points to.
    struct CardBuildData
    {
        glm::vec3     Center{ 0.0f };
        glm::vec3     Extent{ 0.0f }; // half size along the mesh axes
        CardDirection Direction = CardDirection::NegativeX;

        bool operator==( const CardBuildData& ) const = default;
    };

    struct MeshCardsBuildData
    {
        glm::vec3                  BoundsMin{ 0.0f };
        glm::vec3                  BoundsMax{ 0.0f };
        std::vector<CardBuildData> Cards;                  // at most kMaxCardsPerMesh
        bool                       MostlyTwoSided = false; // foliage: lit from both sides (UE bMostlyTwoSided)

        bool operator==( const MeshCardsBuildData& ) const = default;
    };

    // The project-wide build settings (UE r.MeshCardRepresentation.*), inputs of the DDC key.
    struct MeshCardsBuildSettings
    {
        float    TargetVoxelSize         = 10.0f; // cm, UE TargetVoxelSize
        float    MinVoxelSize            = 1.0f;  // cm: the finest a thin axis is cut
        uint32_t MinVoxelsAcrossThinAxis = 4;     // a thin axis gets at least this many voxels (UE: one voxel)
        // UE InitSurfelScene: the lattice's cap per axis (the voxel follows from the bounds), halved while the
        // lattice's surfels exceed TargetSurfels; the visibility rays are cast once, on the chosen lattice.
        uint32_t MaxVoxels               = 64;    // per axis, UE MaxVoxels
        uint32_t TargetSurfels           = 10000; // UE TargetNumSufels
        float    MinDensity              = 0.2f;  // UE r.MeshCardRepresentation.MinDensity
        float    NormalThreshold         = 0.25f; // UE r.MeshCardRepresentation.NormalTreshold
        uint32_t MaxCards                = kMaxCardsPerMesh;
    };

    // The builder (MeshCardBuild.cpp). Deterministic for the same input and settings. Takes the distance
    // field's input: LOD0 triangles in mesh space with each one's material sidedness. It runs in the distance
    // field's job (EnqueueMeshDistanceField) and takes that job's cancel token (UE FCardRepresentationAsyncQueue
    // shares the distance field queue's shutdown): polled per lattice row of every direction and per cluster
    // pass, a cancelled build returns kMeshCardsCancelled - an error, so the DDC writes nothing for it.
    [[nodiscard]] Common::ResultStr<MeshCardsBuildData>
    BuildMeshCards( const MeshDistanceFieldBuildInput& input, const MeshCardsBuildSettings& settings,
                    const MeshDistanceFieldCancel& cancel = {} );
    // The error a cancelled card build returns (and a DDC flight that followed a cancelled leader).
    inline constexpr std::string_view kMeshCardsCancelled = "cards: build cancelled";

    // THE DDC ENTRY: "DESTCARD", a version, the bounds, the flag, the cards.
    inline constexpr uint32_t                           kMeshCardsFormatVersion = 1;
    [[nodiscard]] std::string                           EncodeMeshCards( const MeshCardsBuildData& data );
    [[nodiscard]] Common::ResultStr<MeshCardsBuildData> DecodeMeshCards( std::string_view bytes );

    inline constexpr Common::DDC::Deriver kMeshCardsDeriver{
         "MeshCards", ".mcard", { 0x2F86C1D94B0E73A5ULL, 0x9D3E5A71C6B2F048ULL } };
    // 1: UE's surfel clustering on a per-axis voxel lattice; 2: a slab too sparse for one card is cut into its
    // connected regions (MeshCardBuild.cpp ValidRegions); 3: UE's fixed lattice cap, visibility rays cast once
    inline constexpr uint32_t kMeshCardsBuilderVersion = 3;

    // Render-data key + settings + builder version + every submesh's sidedness -> this bucket's key.
    [[nodiscard]] uint64_t MeshCardsDerivedDataKey( uint64_t renderDataKey, const MeshCardsBuildSettings& settings,
                                                    const std::vector<uint8_t>& submeshTwoSided );

    // Where a builder is registered (the editor, at startup) a miss builds and Puts; a packaged game registers
    // none, and a miss there is an error naming the asset and the key.
    using MeshCardsBuilder = std::function<Common::ResultStr<std::string>(
         const MeshDistanceFieldBuildInput& input, const MeshDistanceFieldCancel& cancel )>;
    void                                         SetMeshCardsBuilder( MeshCardsBuilder builder );
    [[nodiscard]] Common::ResultStr<std::string> BuildMeshCardsBytes( const MeshDistanceFieldBuildInput& input,
                                                                      const MeshDistanceFieldCancel&     cancel );

    // @p input is called only on a miss of the cards or of their coverage (as LoadMeshDistanceField's): a
    // displaced mesh's input is its displaced surface, built only when a bucket needs it. A cancelled build
    // (@p cancel) returns an error containing kMeshCardsCancelled and measures no coverage.
    [[nodiscard]] Common::ResultStr<std::shared_ptr<const MeshCardsBuildData>>
    LoadMeshCards( const std::filesystem::path& asset, uint64_t renderDataKey,
                   const std::vector<uint8_t>&                                            submeshTwoSided,
                   const std::function<Common::ResultStr<MeshDistanceFieldBuildInput>()>& input,
                   const MeshDistanceFieldCancel&                                         cancel );

    // THE COVERAGE (not in UE): the share of the mesh's surface, by area, that its cards hold the way the runtime
    // lookup reads them (Common/SurfaceCacheSampling.glslh) - a surface point is HELD when a card facing one of
    // the axis directions its normal leans to (|component| >= 0.1, the lookup's axis weight) contains it and
    // nothing of the mesh lies between it and the card plane. Only the surface seen from outside the mesh along
    // one of those axis directions is counted (a pocket no axis reaches from outside is a hole UE's cards cannot
    // cover either). Deterministic area-weighted samples, ray-cast against the mesh (Geometry/TriangleBvh.hpp);
    // two-sided triangles count once per side.
    inline constexpr uint32_t kMeshCardsCoverageSamples = 4096;
    struct MeshCardsCoverage
    {
        float Held = 1.0f; // the share of the outside-seen surface the cards hold
        // The share of the one-sided surface seen from outside ONLY from behind: winding that faces into the mesh,
        // which the card build (as UE's) takes for the inside of the geometry and never makes a card for.
        float BackOnly = 0.0f;
    };
    [[nodiscard]] MeshCardsCoverage MeasureMeshCardsCoverage( const MeshDistanceFieldBuildInput& input,
                                                              const MeshCardsBuildData&          cards,
                                                              uint32_t samples = kMeshCardsCoverageSamples );
    // Derived from the cards and the same triangles, in its own DDC bucket keyed on the cards' key and this
    // measure's version: a mesh pays for it once per card build, as for the cards.
    inline constexpr Common::DDC::Deriver kMeshCardsCoverageDeriver{
         "MeshCardsCoverage", ".mccov", { 0x6B1E2F0C93D84A57ULL, 0xA4C7155E2D9B03F1ULL } };
    // 1: area-weighted samples, held when a facing card contains the point and sees it; 2: + the back-only share
    inline constexpr uint32_t kMeshCardsCoverageVersion = 2;

    // THE COVERAGE REPORT. Every mesh whose cards are loaded or built where a builder is registered (the editor,
    // the cook) is recorded with its coverage; one below kMeshCardsLowCoverage is logged as a warning naming it,
    // and MeshCardsCoverageSummary() is the report over the process: the meshes, their mean coverage and the low
    // ones, lowest first (the surface cache prints it when the scene settles).
    inline constexpr float kMeshCardsLowCoverage = 0.75f;
    struct MeshCardsCoverageEntry
    {
        std::string Asset;
        float       Coverage = 0.0f;
        float       BackOnly = 0.0f;
        uint32_t    Cards    = 0;
        glm::vec3   Size{ 0.0f }; // the cards' box, cm
    };
    struct MeshCardsCoverageReport
    {
        uint32_t                            Meshes       = 0;
        float                               MeanCoverage = 0.0f; // by mesh
        std::vector<MeshCardsCoverageEntry> Low;                 // below kMeshCardsLowCoverage, lowest first
    };
    void RecordMeshCardsCoverage( const std::filesystem::path& asset, const MeshCardsCoverage& coverage,
                                  const MeshCardsBuildData& cards );
    [[nodiscard]] MeshCardsCoverageReport MeshCardsCoverageSummary();
    // The report as one line: "N mesh(es), mean X %, K below Y %: a 41 %, b 52 %, ..." (the first @p listed).
    [[nodiscard]] std::string FormatMeshCardsCoverage( const MeshCardsCoverageReport& report, size_t listed );

    // Where a mesh's cards land when its derived-data job finishes (as MeshDistanceFieldSlot): empty until then,
    // and for good when the build failed (logged naming the mesh). Read from any thread; written once.
    class MeshCardsSlot
    {
    public:
        [[nodiscard]] std::shared_ptr<const MeshCardsBuildData> Get() const;
        void Publish( std::shared_ptr<const MeshCardsBuildData> cards );

    private:
        mutable std::mutex                        m_Mutex;
        std::shared_ptr<const MeshCardsBuildData> m_Cards;
    };
} // namespace Desert::Assets
