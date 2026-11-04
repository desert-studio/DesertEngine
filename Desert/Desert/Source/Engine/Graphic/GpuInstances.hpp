#pragma once

// GPU-GENERATED INSTANCES (GRASS-GPU): an instanced mesh whose transforms a compute pass writes this frame and
// whose instance count the GPU states in an indexed-indirect argument record per section. The producer (the
// landscape grass, GrassRenderer) owns the buffers and registers them in the frame graph; the mesh renderer draws
// them through the instanced cells exactly as an ISM, reading the count with vkCmdDrawIndexedIndirect.

#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <cstdint>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace Desert::Graphic
{
    // The frame-graph names of one producer's two buffers, valid from the producer's compute node on (it is added
    // before the cascades and the geometry, so every draw node that reads them declares the reads after it).
    struct GpuInstanceRefs
    {
        RDG::BufferRef Instances; // mat4 per instance, read by the vertex stage as InstanceTransforms
        RDG::BufferRef Args;      // GpuInstanceArgs header + one indexed-indirect record per submesh
        // GPU-COST-22 (UE GPU Scene: a view's culled list is ids into InstanceSceneData, InstanceIdBuffer): when
        // valid, one uint per drawn slot - the row of Instances that slot draws - read by the vertex stage as
        // GpuInstanceIds (cells in the kSurfaceInstanceIdsDefine permutation); the records' firstInstance and the
        // instance index count slots of this list. Invalid: the slot IS the row (Instances drawn directly).
        RDG::BufferRef Ids{};
    };

    // The argument buffer: a 16-byte header (bucket count, total capacity, submesh count, last drawn count), one
    // VkDrawIndexedIndirectCommand (indexCount, instanceCount, firstIndex, vertexOffset, firstInstance) per
    // submesh of the mesh at the drawn level, then per BUCKET four words (count the generator appended, first
    // instance, capacity, 0). A bucket is a range of Instances: a mesh has one, drawn by every section; a grass
    // blade strip one per level, drawn by the submesh of that level (GRASS-BLADE). Programs/Grass/
    // GrassCull.shader writes each record's instanceCount and firstInstance from its bucket.
    // A blade strip's records come in SETS, one per strip level (GPU-COST-20, UE's shadow LOD bias): record
    // k x levels + s draws bucket s with strip level max(s, k). Set 0 is the camera's (level s for bucket s); a
    // cascade draws the set its texel size asks for (MeshRenderer::BuildGpuInstancedShadowDraws), so a blade is
    // never finer in a shadow than in the view and never finer than the cascade can resolve.
    inline constexpr uint64_t        kGpuInstanceArgsHeaderBytes = 16;
    inline constexpr uint64_t        kGpuInstanceArgsRecordBytes = 20;
    [[nodiscard]] constexpr uint64_t GpuInstanceArgsOffset( uint32_t submesh )
    {
        return kGpuInstanceArgsHeaderBytes + kGpuInstanceArgsRecordBytes * submesh;
    }

    // RENDER-JOB-CPU-3, THE COMPACTED BINS (UE FInstanceCullingContext + FMeshDrawCommand merging: one indirect
    // argument stream per pipeline bin, ONE DrawIndexedIndirectCount per bin, the per-draw data fetched by the
    // draw's index instead of bound per section). A bin is one (pass, phase, cell template, mesh) of the
    // GPU-instanced queue. Programs/Grass/GrassDrawCompact.shader walks the frame's entries - per entry a run of
    // one producer list's records (its Args, a phase's or a cascade's copy) - and copies each record to its slot
    // of the bin's run of Commands, the address of the entry's draw row to the same slot of DrawRows, and adds
    // the run to the bin's count in Counts (fixed slots in the per-variety path's order, empty records kept:
    // GpuDrawCompactEntry). MeshRenderer draws a bin with one
    // Renderer::RenderMeshIndirectCount (maxDrawCount = the bin's Capacity, stride kGpuDrawBinCommandBytes); its
    // vertex stage reads the draw's row through the push field DrawRows (DrawPushFields::DrawRows: the address of
    // the bin's first slot of DrawRows) at [gl_DrawIDARB] (Common/GpuInstanceDrawRow.glslh).
    inline constexpr uint64_t kGpuDrawBinCommandBytes =
         kGpuInstanceArgsRecordBytes;                    // VkDrawIndexedIndirectCommand
    inline constexpr uint64_t kGpuDrawBinRowBytes   = 8; // a GpuInstanceDrawRow address
    inline constexpr uint64_t kGpuDrawBinCountBytes = 4; // a bin's drawn count

    // The lists of one producer a draw row describes: the camera's (CameraIds, both phases), the full list
    // (FullIds: the cascades without a list of their own, RT), the cascades' own (CascadeIds, list k's records at
    // k x CascadeArgBytes of CascadeArgs). A row differs between them only by Ids.
    enum class GpuInstanceDrawList : uint32_t
    {
        Camera,
        Full,
        Cascade,
        Count
    };
    inline constexpr uint32_t kGpuInstanceDrawLists = static_cast<uint32_t>( GpuInstanceDrawList::Count );

    // ONE DRAW ROW (std430, 80 bytes; Common/GpuInstanceDrawRow.glslh GpuInstanceDrawRow): what the per-variety
    // path bound or pushed per section (MeshRendererGpuInstanced.cpp bindRecord's slots b17 / b18 / b13 and the
    // mesh visibility rows, addBuckets' MaterialIndex / Wind / PrimitiveFlags), by device address or by value.
    // Written by the producer when a word moves (a buffer re-created, the material row re-claimed, the wind
    // changed), per frame-in-flight copy, never per draw.
    struct GpuInstanceDrawRow
    {
        uint64_t  Instances      = 0; // mat4 per instance (InstanceTransforms)
        uint64_t  Ids            = 0; // the list's ids into Instances (GpuInstanceIds); 0: the slot is the row
        uint64_t  BladeShape     = 0; // the variety's GrassBladeShape; 0: not a blade
        uint64_t  MeshVisibility = 0; // GrassMeshVisibilityGpu rows; 0: not drawn through the visibility buffer
        uint32_t  MaterialRow    = 0; // the scene material table's row (MeshRenderer::FindMaterialRow)
        uint32_t  PrimitiveFlags = 0;
        uint32_t  Reserved0      = 0;
        uint32_t  Reserved1      = 0;
        glm::vec4 WindA{ 0.0f }; // PackViewInstanceWind( Wind, PrevInstanceOffset ).A
        glm::vec4 WindB{ 0.0f }; // .B
    };
    static_assert( sizeof( GpuInstanceDrawRow ) == 80, "GpuInstanceDrawRow mirrors the std430 GLSL struct" );

    // One gather entry (std430, 32 bytes; Programs/Grass/GrassDrawCompact.shader CompactEntry): Count records
    // from Records at Stride bytes apart (a strip's level records: kGpuInstanceArgsRecordBytes; a mesh's LOD
    // groups: a group's bytes), copied to slots FirstSlot .. FirstSlot + Count - 1 of bin Bin, and Count added to
    // the bin's drawn count. The CPU lays the entries of a bin out end to end in the per-variety path's draw
    // order, so a bin draws its records in that order (the picture stays the same); an empty record is an empty
    // draw of the GPU's front end.
    struct GpuDrawCompactEntry
    {
        uint64_t Records      = 0; // device address of the run's first VkDrawIndexedIndirectCommand
        uint64_t DrawRow      = 0; // device address of the GpuInstanceDrawRow its draws read
        uint32_t Count        = 0;
        uint32_t Stride       = 0;
        uint32_t Bin          = 0;
        uint32_t FirstSlot    = 0;
    };
    static_assert( sizeof( GpuDrawCompactEntry ) == 32, "GpuDrawCompactEntry mirrors the std430 GLSL struct" );

    // The frame graph names of one frame's compaction output (valid from its pass on): Commands holds
    // kGpuDrawBinCommandBytes per slot, DrawRows kGpuDrawBinRowBytes per slot, Counts kGpuDrawBinCountBytes per
    // bin (zeroed by the pass before it appends).
    struct GpuDrawBinRefs
    {
        RDG::BufferRef Commands;
        RDG::BufferRef DrawRows;
        RDG::BufferRef Counts;
    };

    // What a cascade picks a grass blade strip's shadow record set and its caster density by (GPU-COST-20; UE
    // r.Shadow LOD by the shadow view's resolution, grass thinned by the same rule as the camera's). Written by the
    // producer per frame (GrassRenderer::PrepareVariety); Levels 0: not a blade, the cascades draw set 0.
    struct GpuInstanceBladeShadowLod
    {
        float    Height      = 0.0f; // the tallest blade, cm (the shape's Height x the placement's largest scale)
        uint32_t Segments    = 1;    // the strip's level-0 segments; each level halves them down to one
        uint32_t Levels      = 0;    // the strip's levels = its record sets
        // where the generator's (F / d)^2 thinning starts, cm through the lens: F = min(StartCullDistance S, the
        // screen-width floor's D) (GrassFieldShare.glslh GrassThinShare); 0: none
        float    StartCull   = 0.0f;
        float    PixelSpread = 0.0f; // the main view's pixel size per cm of distance (0: orthographic, no thinning)
        // the eye the generator measured its distances from (GrassRenderer FieldEye: the camera snapped to the
        // variety's tile grid, CSM-SCROLL-3): a still section's cascade thins its blades from it too, so its static
        // layer does not change while the eye stays in a tile
        glm::vec3 ThinOrigin = glm::vec3( 0.0f );
    };
} // namespace Desert::Graphic
