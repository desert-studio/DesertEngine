#pragma once

// A STATIC MESH'S SIGNED DISTANCE FIELD (UE FDistanceFieldVolumeData, DistanceFieldAtlas.h; built by
// FMeshUtilities::GenerateSignedDistanceFieldVolumeData, MeshDistanceFieldUtilities.cpp). The mesh's own volume,
// in its own local space, stored the way UE stores it: a SPARSE set of 8^3 bricks (7^3 unique samples + one
// shared border sample, so hardware trilinear filtering never reads across a brick seam) behind an indirection
// grid, for three mips. A brick is kept only where the quantized distance is not saturated - the narrow band of
// the surface, BandSizeInVoxels voxels deep on each side; everything else reads as "at least the band away".
//
// It is DERIVED DATA, cached in its own DDC bucket beside the mesh's render data (UE caches it under its own key,
// DISTANCEFIELD_DERIVEDDATA_VER, so changing a distance-field setting never rebuilds the render data and the
// other way round). The key is the render data's key plus the build settings and this builder's version: any
// change to the geometry the GPU draws re-derives the field, and bumping kMeshDistanceFieldBuilderVersion
// re-derives every field on next load (the "recook" - nothing on disk is migrated, the cache simply misses).
//
// The consumers are the GPU scene (Graphic/DistanceField: the brick atlas, the global distance field clipmaps)
// and through them every software trace - GI, reflections, distance-field shadows - via the one sampling
// interface in Shaders/Common/MeshDistanceField.glslh.

#include <Common/Content/DerivedDataCache.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/Math/AABB.hpp>
#include <Engine/Geometry/MeshTypes.hpp>

#include <glm/glm.hpp>

#include <array>
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
    // UE DistanceField:: constants (DistanceFieldAtlas.h:39-50). The GPU side reads the same numbers from
    // Shaders/Common/DistanceFieldShared.glslh; Graphic/DistanceField pins the two together.
    inline constexpr uint32_t kDistanceFieldObjectBorder      = 1;
    inline constexpr uint32_t kDistanceFieldUniqueBrickSize   = 7;
    inline constexpr uint32_t kDistanceFieldBrickSize         = 8;
    inline constexpr uint32_t kDistanceFieldBrickVoxels       = 8 * 8 * 8;
    inline constexpr uint32_t kDistanceFieldBandSizeInVoxels  = 4;
    inline constexpr uint32_t kDistanceFieldNumMips           = 3;
    inline constexpr uint32_t kDistanceFieldInvalidBrick      = 0xFFFFFFFFu;
    inline constexpr uint32_t kDistanceFieldMaxIndirectionDim = 1024;

    // The project-wide build settings (UE r.DistanceFields.DefaultVoxelDensity / MaxPerMeshResolution, both in
    // world units = centimetres). They are inputs of the DDC key: a different density is a different field.
    struct MeshDistanceFieldBuildSettings
    {
        float    VoxelDensity         = 0.2f; // voxels per centimetre: one voxel per 5 cm
        uint32_t MaxPerMeshResolution = 256;  // voxels along the longest axis; half of it at ResolutionScale <= 1
        float    ResolutionScale      = 1.0f; // UE FMeshBuildSettings::DistanceFieldResolutionScale
        // THE CHUNK VOXEL (centimetres). A mesh whose voxel under MaxPerMeshResolution would be larger than its
        // thinnest box extent - a 160 m pavement 29 cm high gets 130 cm voxels, so the slab has no sample inside
        // it and no zero crossing - is built as several SPATIAL VOLUMES of at most that cap each, at this voxel
        // (or the density's, when that is coarser). See MeshDistanceFieldData::Volumes.
        float ChunkVoxelSize = 8.0f;
    };

    // One mip of the sparse volume (UE FSparseDistanceFieldMip + its bulk data).
    struct SparseDistanceFieldMip
    {
        glm::uvec3 IndirectionDimensions{ 0 };
        // Encoded [0,1] -> volume-space distance: d = encoded * x + y (UE DistanceFieldToVolumeScaleBias).
        glm::vec2 DistanceFieldToVolumeScaleBias{ 0.0f };
        // Volume position [-VolumePositionExtent, VolumePositionExtent] -> virtual UV in the indirection grid.
        glm::vec3 VolumeToVirtualUVScale{ 0.0f };
        glm::vec3 VolumeToVirtualUVAdd{ 0.0f };
        // IndirectionDimensions.x*y*z entries, x fastest: the brick's index in BrickData, or the invalid index.
        std::vector<uint32_t> IndirectionTable;
        // BrickCount() bricks of kDistanceFieldBrickVoxels R8 samples, x fastest within a brick.
        std::vector<uint8_t> BrickData;

        [[nodiscard]] uint32_t BrickCount() const
        {
            return static_cast<uint32_t>( BrickData.size() / kDistanceFieldBrickVoxels );
        }
    };

    // ONE VOLUME of a mesh's field: a box of mesh-local space with its own sparse mips (UE's whole
    // FDistanceFieldVolumeData; here a mesh may have several, see MeshDistanceFieldData).
    struct MeshDistanceFieldVolume
    {
        // The box the volume space normalizes (UE LocalSpaceMeshBounds): volume = (local - centre) / max
        // extent, so the volume spans [-VolumePositionExtent, VolumePositionExtent] with the largest axis at 1.
        Common::Math::AABB LocalSpaceMeshBounds{ glm::vec3( 0.0f ), glm::vec3( 0.0f ) };
        // Mip 0 is the finest; mip kDistanceFieldNumMips-1 the coarsest (UE's always-loaded mip).
        std::array<SparseDistanceFieldMip, kDistanceFieldNumMips> Mips;

        [[nodiscard]] uint32_t  TotalBrickCount() const;
        [[nodiscard]] glm::vec3 VolumePositionExtent() const;
        [[nodiscard]] float     LocalToVolumeScale() const;
    };

    // A STATIC MESH'S FIELD: one volume over the mesh box (every mesh UE's per-mesh cap serves - UE stops here),
    // or, for a mesh too large for the cap at its thinnest extent, several volumes over disjoint boxes of its
    // space (MeshDistanceFieldBuildSettings::ChunkVoxelSize) - in UE so a 160 m kerb ring gets 130 cm voxels and
    // rays pass through it (UE leaves it to artists to split the mesh). Each volume is built from every triangle
    // within its coarsest mip's band, so the minimum over the volumes is the mesh's distance across their seams;
    // the GPU scene places each volume as its own object (own box, own bricks, own mip streaming). Empty when
    // the mesh has no triangles.
    struct MeshDistanceFieldData
    {
        std::vector<MeshDistanceFieldVolume> Volumes;

        [[nodiscard]] uint32_t TotalBrickCount() const;
    };

    // The triangles a field is built from: the mesh's LOD0 in mesh-local space, every submesh's own transform
    // applied (the draw applies it the same way - MeshRendererDebug draws rd.Transform * sm.Transform).
    struct MeshDistanceFieldBuildInput
    {
        std::vector<glm::vec3> Positions;
        std::vector<uint32_t>  Indices; // three per triangle
        // One per triangle: 1 = the triangle's material is two-sided (UE FSignedDistanceFieldBuildSectionData::
        // bTwoSided), so it bounds no volume and takes no part in the sign - the field around it is the
        // distance to its surface, never a false interior behind a leaf card.
        std::vector<uint8_t> TriangleTwoSided;
    };
    // @p submeshTwoSided: one flag per submesh, its material's resolved two-sidedness (ResolveMaterialTwoSided).
    [[nodiscard]] Common::ResultStr<MeshDistanceFieldBuildInput>
    MeshDistanceFieldInputOf( const std::vector<Vertex>& vertices, const std::vector<Index>& indices,
                              const std::vector<Submesh>& submeshes, const std::vector<uint8_t>& submeshTwoSided );

    // The builder (MeshDistanceFieldBuild.cpp). Deterministic for the same input and settings.
    [[nodiscard]] Common::ResultStr<MeshDistanceFieldData>
    BuildMeshDistanceField( const MeshDistanceFieldBuildInput&    input,
                            const MeshDistanceFieldBuildSettings& settings );

    // THE DDC ENTRY'S CONTAINER: "DESTMSDF", a version, the mip count, the volume count, then per volume its
    // bounds and per mip its parameters, indirection table and bricks. Decode refuses any other magic or version
    // by name.
    // 2: a list of volumes, each its bounds and mips
    inline constexpr uint32_t kMeshDistanceFieldFormatVersion = 2;
    [[nodiscard]] std::string EncodeMeshDistanceField( const MeshDistanceFieldData& data );
    [[nodiscard]] Common::ResultStr<MeshDistanceFieldData> DecodeMeshDistanceField( std::string_view bytes );

    // UE DISTANCEFIELD_DERIVEDDATA_VER: the bucket and its builder version (bump when the build produces
    // different bytes for the same input).
    inline constexpr Common::DDC::Deriver kMeshDistanceFieldDeriver{
         "MeshDistanceField", ".msdf", { 0x6E1D4B2A93C750F8ULL, 0xB47A0C3E5D9F2816ULL } };
    // 2: two-sided triangles do not vote; 3: behind an open one-sided sheet is inside (BehindOneSidedFace);
    // 4: the sign by UE's backface rays (98 per band sample) in place of the parity vote; 5: a mesh too large for
    // the cap at its thinnest extent is built as spatial volumes (ChunkVoxelSize)
    inline constexpr uint32_t kMeshDistanceFieldBuilderVersion = 5;

    // Render-data key (MeshAssetDerivedDataKey) + settings + builder version + every submesh's material
    // sidedness -> this bucket's key.
    [[nodiscard]] uint64_t MeshDistanceFieldDerivedDataKey( uint64_t                              renderDataKey,
                                                            const MeshDistanceFieldBuildSettings& settings,
                                                            const std::vector<uint8_t>& submeshTwoSided );

    // Where a builder is registered (the editor: SetMeshDistanceFieldBuilder at startup), a miss builds and Puts;
    // a packaged game registers none, and a miss there is an error naming the asset and the key.
    using MeshDistanceFieldBuilder =
         std::function<Common::ResultStr<std::string>( const MeshDistanceFieldBuildInput& input )>;
    void SetMeshDistanceFieldBuilder( MeshDistanceFieldBuilder builder );
    // The editor's builder: BuildMeshDistanceField with the project settings, encoded.
    [[nodiscard]] Common::ResultStr<std::string>
    BuildMeshDistanceFieldBytes( const MeshDistanceFieldBuildInput& input );

    // DDC hit, or build (single-flight per key) and Put. @p input is called only on a miss; @p built, when
    // given, is set to whether it was (a build, not a cache hit).
    [[nodiscard]] Common::ResultStr<std::shared_ptr<const MeshDistanceFieldData>> LoadMeshDistanceField(
         const std::filesystem::path& asset, uint64_t renderDataKey, const std::vector<uint8_t>& submeshTwoSided,
         const std::function<Common::ResultStr<MeshDistanceFieldBuildInput>()>& input, bool* built = nullptr );

    // WHERE A MESH'S FIELD LANDS WHEN ITS BUILD FINISHES (UE FDistanceFieldAsyncQueue: the mesh renders without
    // a field until the task completes, then the field is attached). Empty until then - and for good when the
    // build failed, which the queue logs naming the mesh. Read from any thread; written once, by the queue.
    class MeshDistanceFieldSlot
    {
    public:
        [[nodiscard]] std::shared_ptr<const MeshDistanceFieldData> Get() const;
        void Publish( std::shared_ptr<const MeshDistanceFieldData> field );

    private:
        mutable std::mutex                           m_Mutex;
        std::shared_ptr<const MeshDistanceFieldData> m_Field;
    };

    // THE ASYNCHRONOUS QUEUE (UE FDistanceFieldAsyncQueue, DistanceFieldAtlas.cpp): a field is derived (DDC read
    // or build) on the JobSystem's workers, never on the caller's thread, and published into @p slot when done.
    // At most WorkerCount()-1 (at least one) builds run at once, so one worker always stays free for the rest of
    // the engine (texture cooks, asset reads); the others wait in this queue, not in the pool's. @p work must
    // own everything it reads (it runs after the caller returned). @p asset names the mesh in the log.
    // THE WORK DOES NOT OUTLIVE ITS OWNERS: the queue holds the slot weakly, so work whose slot nobody holds
    // any more (the mesh unloaded or reloaded, its service gone) is cancelled before it starts; a cache root
    // that moves (Common::DDC::RetireRoot) drops the waiting work and waits for the running work, so no field
    // is written into a root other than the one it was started under; process exit does the same before the
    // JobSystem joins its workers.
    using MeshDistanceFieldWork =
         std::function<Common::ResultStr<std::shared_ptr<const MeshDistanceFieldData>>( bool& built )>;
    void EnqueueMeshDistanceField( std::shared_ptr<MeshDistanceFieldSlot> slot, std::string asset,
                                   MeshDistanceFieldWork work );
    // Queued + running builds (diagnostics).
    [[nodiscard]] size_t PendingMeshDistanceFields();
} // namespace Desert::Assets
