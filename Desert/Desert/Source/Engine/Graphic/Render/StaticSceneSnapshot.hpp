#pragma once

#include <Common/Core/Math/AABB.hpp>
#include <Engine/Graphic/Materials/MaterialInstance.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <span>
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
        // THE PARENT MATERIAL OF EVERY OWNER'S SLOTS, taken when the chunk is built (UE: a proxy's material
        // relevance is computed at AddPrimitive / a render-state recreate, not per frame): owner o's run is
        // OwnerParents[OwnerParentsFirst[o], OwnerParentsFirst[o + 1]), one entry per slot (null: no instance).
        // A slot instance's parent is fixed at its construction and a material invalidation rebuilds every chunk
        // (MeshECSSystem::RecordStaticScene), so the per-view replay (the texture demand of
        // SceneRenderer::SubmitStaticScene) reads them instead of asking every slot of every section each frame.
        std::vector<const Material*> OwnerParents;
        std::vector<uint32_t>        OwnerParentsFirst; // Owners.size() + 1 entries once built
        [[nodiscard]] std::span<const Material* const> ParentsOf( const uint32_t owner ) const noexcept
        {
            if ( owner + 1u >= OwnerParentsFirst.size() )
                return {};
            return std::span<const Material* const>( OwnerParents ).subspan(
                 OwnerParentsFirst[owner], OwnerParentsFirst[owner + 1u] - OwnerParentsFirst[owner] );
        }
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

    // ONE VIEW'S ADMISSION OF THE RETAINED STATIC SCENE, KEPT ACROSS FRAMES (UE FScene: per-frame work only over
    // the primitives a change named, PrimitivesNeedingStaticMeshUpdate; the cached draw state lives on). Owned by
    // the view's MeshRenderer, written by SceneRenderer::SubmitStaticScene on the view's replay thread only.
    //
    // A chunk is immutable once published, so a chunk this view walked is named by its pointer (held: no other
    // chunk can take the address while it is cached). Its admission is a function of the chunk, the view
    // (camera position and projection - the LOD rule's view, rotation-free - the target height and the Modeling
    // panel's LOD preview) and its meshes' residency and render revisions. The events:
    //  - a new chunk pointer (MeshECSSystem rebuilt it: a moved, added, removed or re-materialled entity);
    //  - a different View (the camera moved; every chunk is re-walked, as UE's per-view relevance is);
    //  - a section skipped as not resident yet (Pending: the chunk is re-walked until its meshes landed);
    //  - a mesh upload (Mesh::EnsureGpuResources bumps the render revision; residency and the cached bounds only
    //    change with it). Uploads follow Mesh::GetStaleEpoch: a mesh marked stale is prepared at the next fence
    //    (DrawStaticSceneCommand::Prepare), before the view records. So while the epoch holds over two recorded
    //    frames that both checked the admitted revisions, no upload is pending; until then the admitted entries'
    //    revisions are compared (one load per entry, no walk).
    // Texture demand is per frame (TextureStreamingDemand::TakeFrame), so a steady frame notes it again - from
    // Demand, one note per distinct parent material at the largest size any admitted section asked for it.
    struct StaticSceneViewAdmission
    {
        struct View
        {
            glm::vec3 Position{ 0.0f };
            glm::mat4 Projection{ 1.0f };
            uint32_t  Height                          = 0; // the view's target height (texture demand)
            uint32_t  PreviewEntity                   = 0; // the LOD preview's entity (PreviewLevel < 0: none)
            int       PreviewLevel                    = -1;
            bool      HasCamera                       = false; // no camera: no draw-distance cull, no demand
            bool      operator==( const View& ) const = default;
        };
        struct MaterialDemand
        {
            const Material* Parent = nullptr;
            float           Pixels = 0.0f;
        };
        struct Chunk
        {
            std::shared_ptr<const StaticSceneChunk> Source;          // the chunk walked (held)
            std::vector<StaticSceneAdmission>       Admitted;        // its admitted sections, Chunk = its index
            std::vector<MaterialDemand>             Demand;          // sorted by Parent, one entry per parent
            bool                                    Pending = false; // a section waited for its geometry
        };
        View                        Current;
        bool                        Valid = false; // Current names the view the chunks were walked for
        std::vector<Chunk>          Chunks;        // index == StaticSceneSnapshot::Chunks index
        std::vector<MaterialDemand> Demand;        // every chunk's merged, rebuilt when any chunk is re-walked
        uint64_t                    StaleEpoch  = ~0ull; // Mesh::GetStaleEpoch at the last recorded frame
        uint32_t                    EpochChecks = 0;     // frames at StaleEpoch whose revisions were compared
    };

    // THE RETAINED STATIC SCENE, one immutable version of it: what DrawStaticSceneCommand hands every view. A
    // frame in which nothing static changed hands the SAME snapshot again (one shared_ptr copy for the whole
    // scene, nothing rebuilt); a change makes a new snapshot sharing every untouched chunk.
    struct StaticSceneSnapshot
    {
        std::vector<std::shared_ptr<const StaticSceneChunk>> Chunks;
        uint64_t                                             Revision = 0; // bumped per published change
        // The Mesh::GetStaleEpoch its sections' meshes were last prepared at (DrawStaticSceneCommand::Prepare):
        // no mesh marked stale since then = nothing to upload, the section walk is skipped. Game thread only (the
        // fence's Prepare; no replay reads it); reset by the publisher (MeshECSSystem) for every new version.
        mutable uint64_t PreparedStaleEpoch = ~0ull;
    };
} // namespace Desert::Graphic::Render
