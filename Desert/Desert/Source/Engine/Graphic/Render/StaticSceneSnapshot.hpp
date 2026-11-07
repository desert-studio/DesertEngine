#pragma once

#include <Common/Core/Math/AABB.hpp>
#include <Engine/Graphic/Materials/MaterialInstance.hpp>

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace Desert
{
    class Mesh;
}

namespace Desert::Graphic::Render
{
    // ONE LIT SECTION GROUP OF A STATIC MESH ENTITY, AS THE RETAINED STATIC SCENE HOLDS IT (UE: a static mesh
    // proxy's section, built once by AddPrimitive and rebuilt by UpdatePrimitiveTransform / a render-state
    // recreate, never per frame). The fields of DrawStaticMeshCommand, plus what the replay would otherwise
    // recompute per frame: the mesh's local bounds (with the render revision they were taken at) and the
    // index of the binding that keeps the section's slots alive (StaticSceneChunk::Owners).
    struct StaticSceneSection
    {
        Desert::Mesh*      Mesh      = nullptr;
        glm::mat4          Transform = glm::mat4( 1.0f );
        Common::Math::AABB LocalBounds{};
        uint64_t           BoundsRevision  = 0; // Mesh::GetRenderRevision when LocalBounds was taken
        uint64_t           HiddenSubmeshes = 0; // bit i = submesh i hidden
        uint32_t           Entity          = 0;
        uint32_t           SectionSlot     = 0;
        uint32_t           Owner           = 0; // into StaticSceneChunk::Owners
        int                ForcedLOD       = -1;
        int                LODBias         = 0;
        int                TranslucencySortPriority = 0;
        float              MaxDrawDistance          = 0.0f;
        bool               Outlined                 = false;
        bool               CastShadows              = true;
        bool               ReceiveShadows           = true;
        bool               CastContactShadow        = true;
        // The entity's PERSISTENT PRIMITIVE SLOT (MeshECSSystem; UE FPrimitiveSceneInfo::PackedIndex): the key
        // GpuSceneRegistry keeps the section's primitive under across frames (GpuSceneRegistry::SyncStatic).
        // Every section of one slot is one entity and shares its world. Set by the chunk rebuild.
        uint32_t Primitive = 0;
    };

    // A fixed run of persistent primitive slots (MeshECSSystem: slot / kStaticSceneChunkSlots). IMMUTABLE ONCE
    // PUBLISHED: a change rebuilds only the chunks whose slots changed, into a chunk no snapshot in flight
    // names, so a replay reading the previous snapshot on the render task never sees a write.
    struct StaticSceneChunk
    {
        std::vector<StaticSceneSection>     Sections;
        std::vector<MaterialSlotBindingPtr> Owners; // one per entity with sections in this chunk
    };

    inline constexpr uint32_t kStaticSceneChunkSlots = 64;

    // One section a view ADMITTED from the snapshot (SceneRenderer::SubmitStaticScene: ready, within its draw
    // distance), with the view-dependent inputs its queue entry is filled from: the LOD the view forces (the
    // Modeling panel's preview) and the mesh's render revision (an in-place edit, a re-upload). An equal list
    // over the same chunks queues equal entries, so MeshRenderer::SubmitStaticScene keeps them as they are.
    struct StaticSceneAdmission
    {
        uint32_t Chunk                                           = 0; // into StaticSceneSnapshot::Chunks
        uint32_t Section                                         = 0; // into StaticSceneChunk::Sections
        int      ForcedLOD                                       = -1;
        uint64_t MeshRevision                                    = 0;
        bool     operator==( const StaticSceneAdmission& ) const = default;
    };

    // THE RETAINED STATIC SCENE, one immutable version of it: what DrawStaticSceneCommand hands every view. A
    // frame in which nothing static changed hands the SAME snapshot again (one shared_ptr copy for the whole
    // scene, nothing rebuilt); a change makes a new snapshot sharing every untouched chunk.
    struct StaticSceneSnapshot
    {
        std::vector<std::shared_ptr<const StaticSceneChunk>> Chunks;
        uint64_t                                             Revision = 0; // bumped per published change
    };
} // namespace Desert::Graphic::Render
