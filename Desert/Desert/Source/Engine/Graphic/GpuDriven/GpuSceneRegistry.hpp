#pragma once

#include <Engine/Graphic/GpuDriven/GpuScene.hpp>
#include <Engine/Graphic/GeometryPool.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Desert
{
    class Mesh;
}

namespace Desert::Graphic
{
    // A primitive of the scene as the renderer's queues name it: the entity that owns the draw and a slot inside
    // it (a model with several skinned meshes has one primitive per mesh). The key under which GpuSceneRegistry
    // keeps the primitive's GpuScene row across frames.
    struct MotionKey
    {
        uint32_t Entity = 0; // entt id incl. version
        uint32_t Slot   = 0;

        bool operator==( const MotionKey& ) const = default;
    };

    // A Slot with this bit set is not a submission index but a STABLE PART of the entity the draw named itself
    // (MotionRecord::Part; a fracture piece is its node + 1): parts move independently and appear or vanish
    // from one frame to the next, so numbering them by submission order would hand one piece another's
    // previous transform. The ranges cannot meet: no entity submits 2^30 primitives.
    inline constexpr uint32_t kMotionPartSlotBit = 1u << 31;
    // A Slot with this bit set names the n-th instanced field (ISM / foliage component) of the entity: ONE
    // primitive whose GpuScene instances are the field's instances.
    inline constexpr uint32_t kMotionFieldSlotBit = 1u << 30;

    struct MotionKeyHash
    {
        [[nodiscard]] std::size_t operator()( const MotionKey& key ) const noexcept
        {
            return std::hash<uint64_t>{}( ( static_cast<uint64_t>( key.Entity ) << 32u ) | key.Slot );
        }
    };

    // One queued mesh record as the registry sees it: the owning entity, the world it is drawn with this frame
    // and, for a skinned record, this frame's bone palette (empty for a rigid one).
    struct MotionRecord
    {
        uint32_t Entity = 0;
        // 0: the entity's primitives are numbered in submission order. Non-zero: a stable part of the entity (a
        // fracture piece: node + 1), keyed { Entity, kMotionPartSlotBit | Part } whatever else the entity drew.
        // Rigid only.
        uint32_t                   Part = 0;
        glm::mat4                  World{ 1.0f };
        std::span<const glm::mat4> Bones{};
        // The mesh drawn (its bounds go into the row; null for geometry with no mesh, the landscape tiles) and
        // GpuDriven::PrimitiveFlags.
        const Mesh* Mesh  = nullptr;
        uint32_t    Flags = 0;
        // StaticMeshRenderData::ForcedLOD / LODBias (MeshRenderer::ComputeLOD's inputs); -1 / 0 for the rest.
        int32_t ForcedLod = -1;
        int32_t LodBias   = 0;
        // What a reader of the record's sections reads besides its mesh, world and flags (its section materials,
        // hidden submeshes, ray-tracing visibility, the mesh's residency), as one word the producer makes
        // (MeshRenderer::SyncGpuScene). A primitive's records' words changing is a Content change of the GpuScene
        // stream (GpuScene::MarkContent); the rows do not hold it.
        uint64_t Content = 0;
    };

    // One queued instanced field (ISM / foliage): its instances' WORLD transforms. Records of one entity that
    // carry the same transform set (a field split across material slots) are one primitive.
    struct MotionFieldRecord
    {
        uint32_t                   Entity = 0;
        const Mesh*                Mesh   = nullptr;
        std::span<const glm::mat4> Transforms{};
        float                      CullDistanceStart = 0.0f;
        float                      CullDistanceEnd   = 0.0f;
        uint32_t                   Flags             = 0;
        // WindBoundsPad( InstancedMeshRenderData::Wind ): how far the wind moves a vertex horizontally (cm).
        float BoundsPad = 0.0f;
        uint64_t Content = 0; // MotionRecord::Content
        // The wind itself, written to the row every frame it changes (GpuScene::SetWind): the binned instanced
        // vertex stage reads it there.
        GpuDriven::PrimitiveWind Wind{};
        // The co-owned, immutable vector Transforms views (InstancedMeshRenderData::Transforms: the component's
        // runtime snapshot, replaced and never mutated when the authored transforms change). The registry keeps
        // the one its instance rows were last written or verified from; the same object next frame is the same
        // contents (it cannot be freed and its address reused while held), so the per-instance comparison is
        // skipped (UE: instance data is re-uploaded on UpdateInstances, not compared every frame). Null: compared.
        std::shared_ptr<const std::vector<glm::mat4>> Snapshot;
    };

    // THE FRAME VALUES OF A FIELD WHOSE PRODUCER DID NOT CHANGE (FPS-SCENE-EVENTS4): what a MotionFieldRecord
    // carries besides its identity (entity, transforms, snapshot, content), in the same queue position. When the
    // producer reported no change since the proxies' last full field sync (GpuSceneProxies::KeepsFields), the
    // view hands these instead of full records: no snapshot handle is copied, no slot derived, no proxy looked up.
    struct MotionFieldFrame
    {
        const Mesh*              Mesh              = nullptr; // its geometry binding is compared, not its address
        uint32_t                 Flags             = 0;
        float                    BoundsPad         = 0.0f;
        float                    CullDistanceStart = 0.0f;
        float                    CullDistanceEnd   = 0.0f;
        GpuDriven::PrimitiveWind Wind{};
    };

    // One section of the RETAINED static scene (Render::StaticSceneSection) as the registry keys it: by the
    // persistent primitive slot MeshECSSystem gave its entity (UE: FPrimitiveSceneInfo::PackedIndex), never by
    // submission order. Every record of one slot is one entity with one world: one primitive, its sections'
    // Content summed. Record.Part is unused (0).
    struct StaticPrimitiveRecord
    {
        uint32_t     Primitive = 0;
        MotionRecord Record;
    };
} // namespace Desert::Graphic

// GPU-DRIVEN phase GPUSCENE: THE SCENE'S PRIMITIVES BY RENDERER KEY. Owns the scene's GpuScene (Scene::
// GetGpuScene) and keeps, per MotionKey, the primitive's stable GpuScene index while the scene's views keep
// queueing it. The previous world is the row's PrevLocalToWorld, the previous palette is kept here per primitive,
// and every view of a frame reads the same rows.
//
// Lifetime of a primitive: added the first frame a view queues it (Prev == World: no object motion, as a newly
// visible object had before), kept while each synced frame queues it, removed at the first BeginFrame after a
// synced frame that did not queue it (it reappears later with no stale previous world). A dynamic primitive is
// told by events (GpuSceneProxies -> AddPrimitive / UpdatePrimitive / RemovePrimitive), a static one by
// SyncStatic's list.
namespace Desert::Graphic::GpuDriven
{
    // Where GpuSceneProxies::Sync / SyncStatic put each record: rigid and skinned records -> their primitive (the
    // push block's PrimitiveIndex) and its first instance (the TLAS custom index / GI instance row); fields ->
    // their first instance (instance i of the field = FirstInstance + i).
    struct GpuSceneRecordRows
    {
        std::vector<PrimitiveIndex> RigidPrimitives;
        std::vector<InstanceIndex>  RigidInstances;
        std::vector<PrimitiveIndex> SkinnedPrimitives;
        std::vector<InstanceIndex>  SkinnedInstances;
        std::vector<InstanceIndex>  FieldInstances;
        std::vector<PrimitiveIndex> FieldPrimitives;  // each field's primitive, beside FieldInstances
        std::vector<PrimitiveIndex> StaticPrimitives; // SyncStatic: per record, its slot's primitive
        std::vector<InstanceIndex>  StaticInstances;
    };

    // A row's identity in the scene material table (GpuScene::AddMaterialRow): the template whose layout the row
    // has (its schema), the instance whose overrides it carries (null = the template's own values) and the
    // per-primitive bits that change the row's values (kMaterialRowNoSunShadows). Owner: the primitive whose values
    // the row carries when no material instance holds them (a Shader Override draw: its entity and part, packed by
    // MeshRenderer), 0 otherwise. The values themselves are not the key: a changed parameter re-uploads the same
    // row, and an address reused by a new instance re-uploads too.
    struct MaterialRowKey
    {
        const void* Material = nullptr;
        const void* Instance = nullptr;
        uint32_t    Variant  = 0;
        uint64_t    Owner    = 0;
        bool        operator==( const MaterialRowKey& ) const = default;
    };
    // The component's Receive Shadows off zeroes the row's ReceiveSunShadows (MeshRenderer::SyncGpuScene).
    inline constexpr uint32_t kMaterialRowNoSunShadows = 1u;

    // What a material row's values were derived from (UE: a cached mesh draw command stays valid until its
    // material changes): the template's row revision (DataDrivenMaterial::GetParamRowRevision) and the instance's
    // parameter revision (MaterialInstance::GetParameterRevision, 0 = no instance). Both are process-unique, so a
    // key whose address was reused never matches. Template 0 = no revision: the row is always compared.
    struct MaterialRowRevision
    {
        uint64_t Template                                       = 0;
        uint64_t Instance                                       = 0;
        bool     operator==( const MaterialRowRevision& ) const = default;
    };

    struct MaterialRowKeyHash
    {
        [[nodiscard]] std::size_t operator()( const MaterialRowKey& key ) const noexcept
        {
            const std::size_t material = std::hash<const void*>{}( key.Material );
            const std::size_t instance = std::hash<const void*>{}( key.Instance );
            const std::size_t owner    = std::hash<uint64_t>{}( key.Owner );
            return material ^ ( instance * 0x9E3779B97F4A7C15ull ) ^ ( static_cast<std::size_t>( key.Variant ) << 1u ) ^
                   ( owner * 0xC2B2AE3D27D4EB4Full );
        }
    };

    class GpuSceneRegistry
    {
    public:
        [[nodiscard]] GpuScene& Scene()
        {
            return m_Scene;
        }
        [[nodiscard]] const GpuScene& Scene() const
        {
            return m_Scene;
        }

        // Once per absolute frame (every view calls it; the later calls of one frame do nothing): when no view
        // committed the last synced frame (CommitFrame: its graph never executed - FrameFault, a failed sync),
        // that frame is discarded first, as if it never happened (GpuScene::DiscardFrame, the palettes and the
        // primitives it added); then drops the primitives the last kept frame did not queue, then
        // GpuScene::BeginFrame.
        void BeginFrame( uint64_t frame );
        // A view's graph of @p frame executed (SceneRenderer, beside SceneViewState::EndFrame): the frame is kept.
        void CommitFrame( uint64_t frame );

        // THE DYNAMIC PRIMITIVES BY EVENT (FPS-SCENE-EVENTS; UE FScene::AddPrimitive / UpdatePrimitiveTransform /
        // RemovePrimitive queued by the producers, applied as they come): a producer (one view's GpuSceneProxies)
        // holds a LIST of primitives and tells the registry only what changed - nothing is walked or compared for
        // a primitive that did not change. A primitive is shared by every list that added its key (the views
        // agree on the frame: the first list of a frame to write it wins, a later list's write of the same frame
        // is ignored) and removed once no list holds it.
        // Lifetime as before: a list no committed frame kept (EndList) is dropped by the next BeginFrame with its
        // primitives (the view stopped syncing); a removal takes effect at the next BeginFrame of a kept frame (a
        // discarded frame's removals are cancelled, its additions undone).
        //
        // The list a producer syncs this frame: @p list itself when the registry holds it, else a new one (0, or
        // an id this registry does not hold: dropped, or another registry's - ids are process-unique). @p fresh:
        // every primitive the producer held is void (a new list, or a discarded frame rolled the list back) and it
        // adds each one afresh - an Add of a key the list already holds is a plain rewrite, and the members it
        // does not add again leave at EndList.
        [[nodiscard]] uint64_t BeginList( uint64_t list, bool& fresh ) noexcept;
        // Whether BeginList( @p list ) this frame would keep every primitive its producer holds (the registry
        // holds the list and no discarded frame rolled it back): fresh == false. Call after BeginFrame.
        [[nodiscard]] bool ListCurrent( uint64_t list ) const noexcept;
        // The list's sync of this frame is complete: the frame's palettes reach GpuScene::SetBonePalettes; on a
        // re-added list, the primitives it held and did not add again leave it.
        void EndList( uint64_t list ) noexcept;
        // A primitive enters the list (@p key: GpuSceneProxies' numbering). Prev == World for a new one.
        [[nodiscard]] PrimitiveIndex AddPrimitive( uint64_t list, MotionKey key,
                                                   const MotionRecord& record ) noexcept;
        [[nodiscard]] PrimitiveIndex AddField( uint64_t list, MotionKey key,
                                               const MotionFieldRecord& field ) noexcept;
        // Its world, mesh, geometry, flags or LOD inputs changed (only what differs from the row is rewritten).
        void UpdatePrimitive( uint64_t list, PrimitiveIndex primitive, const MotionRecord& record ) noexcept;
        // A field's mesh, instances, flags, bounds pad or cull distances changed.
        void UpdateField( uint64_t list, PrimitiveIndex primitive, const MotionFieldRecord& field ) noexcept;
        // What its records draw besides mesh, world and flags changed (MotionRecord::Content):
        // GpuScene::MarkContent.
        void UpdateContent( PrimitiveIndex primitive ) noexcept;
        void UpdateWind( PrimitiveIndex primitive, const PrimitiveWind& wind ) noexcept;
        // A skinned primitive's palette of this frame (every frame it is drawn; the first list of a frame wins):
        // its row names it and the previous one (the primitive's palette of the last synced frame, or this one
        // when it was not drawn then or its bone count changed).
        void UpdateBones( PrimitiveIndex primitive, std::span<const glm::mat4> bones ) noexcept;
        // The primitive leaves the list (at the next BeginFrame of a kept frame).
        void RemovePrimitive( uint64_t list, PrimitiveIndex primitive ) noexcept;

        // THE RETAINED STATIC SCENE's records of one view (CPU-SCENE-DELTA 2; UE: FScene keeps the primitive by
        // its PackedIndex, AddPrimitive / UpdatePrimitiveTransform / RemovePrimitive on change only). Keyed by
        // StaticPrimitiveRecord::Primitive in a dense array (no hash, no per-entity scratch, no world compare to
        // find the primitive). @p token names the list's contents (MeshRenderer: a new one per change of the
        // list, of any record's world, mesh, flags or content; process-unique, 0 = none). Lifetime as a list's: a
        // slot no synced view of a frame queued is removed by the next BeginFrame; a slot given to another entity
        // is removed and added afresh (no previous world of the old entity). Writes @p out.StaticPrimitives /
        // StaticInstances, one per record. Call after BeginFrame, before or after the dynamic lists.
        void SyncStatic( std::span<const StaticPrimitiveRecord> records, uint64_t token, GpuSceneRecordRows& out );
        // The cheap frame of an unchanged static scene: true when the last SyncStatic was of @p token and no
        // primitive of it was removed, discarded or rolled back since - then its primitives are queued for this
        // frame and nothing else is read or written, O(1): one stamp for the whole list (the rows the caller
        // kept from that SyncStatic still name them). false: call SyncStatic.
        [[nodiscard]] bool KeepStatic( uint64_t token );

        // The scene material table row of @p key holding @p row: added the first time a frame asks, its slots
        // re-uploaded when the values differ from the ones it holds (a parameter changed), re-added at the new
        // stride when the template's layout changed (hot reload), removed by the first BeginFrame after a frame
        // that did not ask (the primitives' rule; every view of a frame may ask). Returns the index the draw names
        // as MaterialIndex / GpuDrawSlot::MaterialRow. An empty row (a template without parameters declares no
        // Materials block) takes no row: 0, which its shader never reads.
        // @p revision: what @p row was derived from (stored with it for ReuseMaterialRow; {} = none).
        [[nodiscard]] uint32_t SyncMaterialRow( const MaterialRowKey& key, std::span<const glm::vec4> row,
                                                MaterialRowRevision revision = {} );
        // The row of @p key when it holds values derived from @p revision (a non-zero Template): asked for this
        // frame as SyncMaterialRow would, with neither the row built nor its values compared. nullopt: the key
        // has no row yet, or its values were derived from something else - build the row, SyncMaterialRow it.
        [[nodiscard]] std::optional<uint32_t> ReuseMaterialRow( const MaterialRowKey& key,
                                                                MaterialRowRevision   revision );

    private:
        static constexpr uint64_t kNoFrame = ~0ull;
        struct MaterialEntry
        {
            uint32_t               Row       = 0;
            std::vector<glm::vec4> Values;               // what the table holds at Row
            uint64_t               Frame     = kNoFrame; // the last frame that asked for it
            uint64_t               PrevFrame = kNoFrame; // the one before (DiscardFrame restores it)
            MaterialRowRevision    Revision;             // what Values were derived from ({} = unknown)
        };
        void AskedThisFrame( MaterialEntry& entry );

        struct Entry
        {
            PrimitiveIndex         Primitive  = kInvalidIndex;
            uint64_t               Frame      = kNoFrame; // the last frame that queued it
            uint64_t               PrevFrame  = kNoFrame; // the one before (DiscardFrame restores it)
            const Mesh*            Mesh       = nullptr;
            // Where the mesh's geometry sat when the row was written (MeshGeometry::MeshBinding): the bins carry
            // its pool base, so a re-upload into a new allocation (DynamicMesh::Update) rebins like a mesh change.
            MeshGeometryBinding    Geometry;
            uint32_t               Flags      = 0;
            // The row's LOD and cull inputs as last written: a change rewrites the row like a mesh change.
            int32_t                ForcedLod = -1;
            int32_t                LodBias   = 0;
            float                  BoundsPad = 0.0f;
            float                  CullStart = 0.0f;
            float                  CullEnd   = 0.0f;
            // THE PALETTE LIVES IN THE FRAME BUFFERS, NOT HERE (UE: FSkeletalMeshObjectGPUSkin's double-buffered
            // bone buffer). BoneOffset/BoneCount name this primitive's palette inside the frame palette of
            // BonesFrame; the frame after reads its history straight out of m_PreviousPalettes there.
            uint64_t BonesFrame     = kNoFrame; // the frame BoneOffset / BoneCount were written for
            uint64_t PrevBonesFrame = kNoFrame; // the frame before it (DiscardFrame restores it)
            uint32_t BoneOffset     = 0;
            uint32_t BoneCount      = 0;
            uint32_t PrevOffset     = 0;        // where the history copy sits in the frame palette
            uint32_t PrevBoneOffset = 0;        // BoneOffset as of PrevBonesFrame (DiscardFrame restores it)
            uint32_t PrevBoneCount  = 0;
            // The primitive's records' Content words as of the last SyncStatic that queued it (an order-free sum
            // of their mixes: a merged record of the primitive adds its own), and the one SyncStatic gathers now.
            uint64_t Content      = 0;
            uint64_t FrameContent = 0;
            uint64_t ContentSync  = 0; // the Sync (m_SyncSerial) FrameContent is being summed in
            // A dynamic (list) primitive: its key, how many lists hold it, and the frame / list that last wrote
            // it (a later list of the same frame does not overwrite: the views agree on the frame).
            MotionKey Key{};
            uint32_t  Refs         = 0;
            uint64_t  WrittenFrame = kNoFrame;
            uint64_t  WrittenList  = 0;
        };

        // One producer's primitives (BeginList .. EndList). Members change only by Add / Remove events.
        struct SceneList
        {
            uint64_t                           Frame     = kNoFrame; // the last frame that kept it (EndList)
            uint64_t                           PrevFrame = kNoFrame; // the one before (DiscardFrame restores it)
            std::unordered_set<PrimitiveIndex> Members;
            std::vector<PrimitiveIndex>        Added; // this frame's additions (DiscardFrame undoes them)
            std::vector<PrimitiveIndex> Removed; // this frame's removals (applied by the next kept BeginFrame)
            // Re-added from scratch this frame (a discarded frame rolled it back: BeginList said fresh): the
            // members its producer adds again; the others leave at EndList.
            bool                        Resync = false;
            std::vector<PrimitiveIndex> Seen;
        };
        SceneList* ListOf( uint64_t list ) noexcept;
        Entry*     DynamicOf( PrimitiveIndex primitive ) noexcept;
        // May @p list write @p entry this frame (no earlier list of the frame did); stamps it when so.
        bool Writable( Entry& entry, uint64_t list ) noexcept;
        // The key's entry (created: @p inserted, stamped as written by @p list); its primitive (none yet when
        // new).
        PrimitiveIndex Join( uint64_t list, MotionKey key, bool& inserted, Entry*& held ) noexcept;
        // The entry joins @p owner's members (one more list holds it) unless it is one already.
        void Enlist( SceneList& owner, Entry& entry, bool inserted ) noexcept;
        void Leave( PrimitiveIndex primitive ) noexcept; // one list less; removed at none
        void WriteField( Entry& entry, bool inserted, const MotionFieldRecord& field ) noexcept;

        // The row part of a write: what differs from @p record (@p inserted: no row yet) is rewritten.
        void WriteRow( Entry& entry, bool inserted, const MotionRecord& record );
        // SyncStatic's write of a slot the current frame has not queued yet: the row, its Content and the stamp.
        void Write( Entry& entry, bool inserted, const MotionRecord& record );
        // The first record of the primitive this Sync (it was fresh: no earlier view of the frame synced it)
        // starts its FrameContent; every other record of it in the same Sync adds to it (AddContent). The end of
        // Sync publishes a Content change for each primitive whose sum moved (not for one added now).
        void BeginContent( Entry& entry, bool inserted, uint64_t content );
        void AddContent( Entry& entry, uint64_t content ) const;
        void PublishContent();
        void   Queued( Entry& entry ) const;
        void   DiscardFrame();

        GpuScene                                            m_Scene;
        std::unordered_map<MotionKey, Entry, MotionKeyHash> m_Entries;
        std::unordered_map<MaterialRowKey, MaterialEntry, MaterialRowKeyHash> m_MaterialRows;
        // The dynamic primitives' entries by primitive (the events name a primitive, not a key: no hash) and the
        // producers' lists by id.
        std::vector<Entry*>                     m_Dynamic;
        std::unordered_map<uint64_t, SceneList> m_Lists;
        bool                                    m_PalettesChanged = false; // since the last EndList
        // The retained static scene's primitives by persistent slot (SyncStatic). Live: the slot holds a row.
        // THE FRAME DOES NOT WALK THE SLOTS (UE FScene: only the added / updated / removed primitives are
        // visited). The current list (m_StaticTokenSlots, its members' List == m_StaticListSerial) is queued as
        // a whole by one stamp, m_StaticListFrame (KeepStatic writes nothing per slot); a member's Held.Frame is
        // brought up to that stamp (FoldListFrame) only when it leaves the list. A live slot outside the list is
        // an orphan (m_StaticOrphans): BeginFrame visits only the orphans, and the list's members only when the
        // frame that ended did not queue the list.
        struct StaticSlot
        {
            Entry    Held;
            uint64_t List   = 0; // the list (m_StaticListSerial) that last named the slot
            uint32_t Entity = 0;
            bool     Live   = false;
            bool     Orphan = false; // listed in m_StaticOrphans
        };
        void                    RemoveStatic( StaticSlot& slot );
        void                    FoldListFrame( uint32_t index );
        void                    OrphanStatic( uint32_t index );
        void                    RetireStatic();
        std::vector<StaticSlot> m_StaticSlots;
        std::vector<uint32_t>   m_StaticTokenSlots; // the distinct slots of the last SyncStatic's list
        uint64_t                m_StaticToken = 0;  // that list's token; 0 once any of its rows changed hands
        uint64_t                m_StaticListSerial    = 0;        // names the current list (one per SyncStatic)
        uint64_t                m_StaticListFrame     = kNoFrame; // the last frame that queued the current list
        uint64_t                m_StaticListPrevFrame = kNoFrame; // the one before (DiscardFrame restores it)
        std::vector<uint32_t>   m_StaticOrphans;  // live slots outside the current list (may lapse)
        std::vector<uint32_t>   m_StaticWritten;  // slots whose Held.Frame this frame set (DiscardFrame's undo)
        // The palettes of the frame being synced and of the frame before it, swapped at BeginFrame (and back at
        // DiscardFrame): the previous frame's bones are read from where they were written, never kept twice.
        std::vector<glm::mat4> m_FramePalettes;
        std::vector<glm::mat4> m_PreviousPalettes;
        uint64_t                                            m_SyncSerial = 0;
        std::vector<std::pair<Entry*, bool>>                m_ContentTouched; // entry, added this Sync
        uint64_t                                            m_Frame         = kNoFrame;
        uint64_t                                            m_PreviousFrame = kNoFrame;
        uint64_t                                            m_FrameBeforePrevious = kNoFrame;
        bool                                                m_Started       = false;
        bool                                                m_Committed     = false; // CommitFrame( m_Frame )
        // THE RETIRE WALK RUNS ONLY WHEN SOMETHING LEFT (UE FScene: RemovedPrimitives is a list, not a sweep).
        // How many m_MaterialRows the frame being synced stamped with m_Frame (each counted on its first stamp of
        // the frame). BeginFrame walks the map only when its count is below its size - an unchanged scene retires
        // nothing and reads no entry. Exact from BeginFrame on; a DiscardFrame restores stamps of an older frame,
        // so the next BeginFrame walks all (m_CountsExact). The dynamic primitives need no walk: they leave by
        // event (RemovePrimitive) or with their list.
        size_t m_MaterialsAsked = 0;
        bool   m_CountsExact    = false;
    };

    // ONE VIEW'S PRODUCER OF DYNAMIC PRIMITIVES (UE FPrimitiveSceneProxy set): the view's queued rigid, skinned
    // and field records of this frame, diffed against the proxies it keeps per MotionKey across frames, reach the
    // registry as events only - an added, changed or removed primitive; an unchanged one costs no registry call.
    // Numbering (unchanged from the per-frame Sync it replaces):
    //  - Per entity, the rigid primitives in submission order; the position IS MotionKey::Slot. Rigid records of
    //    one entity with an IDENTICAL world are one primitive (a mesh split across material slots).
    //  - Every skinned record is a primitive of its own (the entity's next slot); its palette goes up every frame.
    //  - A rigid record with a Part is keyed by that part.
    //  - A field is keyed by its position among the entity's distinct transform sets.
    // A record's lookups are memoised by its queue position (the queues repeat their order frame to frame): the
    // entity scratch and the proxy of last sync's record at the same position are reused when they name the same
    // entity and key - no hash for a steady queue.
    class GpuSceneProxies
    {
    public:
        // @p out is cleared first; one row per record, in order, as the registry's rows of each primitive.
        // THE FIELDS BY PRODUCER EVENT (FPS-SCENE-EVENTS4; UE: an ISM proxy is updated on UpdateInstances / a
        // render-state dirty mark, never re-derived per frame): with @p keptFields set, @p fields is ignored and
        // @p fieldFrames (one per field of the last full field sync, in its order) carries this frame's values
        // only - the field proxies are kept as last synced, their wind updated, their rows re-read. The caller
        // asked KeepsFields for exactly these frames after the registry's BeginFrame and before this call.
        // false: @p keptFields was set while KeepsFields would have refused (a contract break; the fields' rows
        // in @p out are missing) - the caller reports it and syncs full records next frame.
        // THE RIGID RECORDS BY PRODUCER EVENT (FPS-SCENE-EVENTS5; UE FScene::UpdatePrimitiveTransform /
        // AddPrimitive come from component events, an unchanged primitive is not visited): with @p keptRigid set,
        // @p rigid is ignored (pass it empty) and the rigid proxies of the last full rigid sync are kept as they
        // stand - no world packed, no proxy looked up, no record compared; their rows are re-read in that sync's
        // order. The caller asked KeepsRigid for exactly @p keptRigidCount records after the registry's
        // BeginFrame. false: a kept part (fields or rigid) was set while its Keeps* would have refused (a contract
        // break; its rows in @p out are missing) - the caller reports it and syncs full records next frame.
        [[nodiscard]] bool Sync( GpuSceneRegistry& registry, std::span<const MotionRecord> rigid, bool keptRigid,
                                 size_t keptRigidCount, std::span<const MotionRecord> skinned,
                                 std::span<const MotionFieldRecord> fields, bool keptFields,
                                 std::span<const MotionFieldFrame> fieldFrames, GpuSceneRecordRows& out ) noexcept;
        // Whether Sync may keep the rigid records. The caller vouches that the @p count rigid records it would
        // queue equal, position for position, the ones of the last full rigid Sync (entity, part, world, mesh,
        // flags, LOD inputs, content); this checks what it cannot see: the registry still holds this list
        // unrolled, the last Sync synced full rigid records (not dropped by a Reset), as many as then, and each
        // held proxy's mesh geometry binding (a re-upload / pool move of the same mesh). O(records), no hash.
        [[nodiscard]] bool KeepsRigid( const GpuSceneRegistry& registry, size_t count ) const noexcept;
        // Whether Sync may keep the fields with @p frames in place of full records. The caller vouches that the
        // fields' producer reported no change since the last Sync (same entities, meshes, material slots, hidden
        // masks, transform snapshots, flags inputs, in the same order); this checks what it cannot see: the
        // registry still holds this list unrolled, the last Sync synced full field records (not dropped by a
        // Reset), as many fields as then, and per proxy's first field the geometry binding, flags, bounds pad,
        // cull distances and an immutable snapshot (a field without one is compared by the registry every
        // sync, so it never takes this path). O(fields), no hash, no handle copy.
        [[nodiscard]] bool KeepsFields( const GpuSceneRegistry&           registry,
                                        std::span<const MotionFieldFrame> frames ) const noexcept;

    private:
        struct Proxy
        {
            PrimitiveIndex Primitive = kInvalidIndex;
            uint64_t       Sync      = 0; // the sync that last queued it
            // What the registry's row was last told (an event fires when the record differs).
            glm::mat4           World{ 1.0f };
            const Mesh*         Mesh = nullptr;
            MeshGeometryBinding Geometry;
            uint32_t            Flags     = 0;
            int32_t             ForcedLod = -1;
            int32_t             LodBias   = 0;
            // The records' Content words as last published (an order-free sum of their mixes) and this sync's.
            uint64_t Content      = 0;
            uint64_t FrameContent = 0;
            // A field: its row inputs and the snapshot its instances were last sent from (null: compared by the
            // registry every sync, as the record carries no immutable snapshot).
            float                                         BoundsPad = 0.0f;
            float                                         CullStart = 0.0f;
            float                                         CullEnd   = 0.0f;
            PrimitiveWind                                 Wind{};
            std::shared_ptr<const std::vector<glm::mat4>> Snapshot;
        };
        struct Owned
        {
            PrimitiveIndex Primitive = kInvalidIndex;
            Proxy*         Held      = nullptr;
        };
        // Per entity, what this sync gathered of it (emptied by the sync's first record of the entity).
        struct EntityScratch
        {
            uint64_t                      Sync = 0;
            std::vector<Owned>            Primitives; // index == MotionKey::Slot
            std::vector<const glm::mat4*> FieldSets;  // index == the field's slot
        };
        // Last sync's lookups by queue position (rigid, skinned, fields end to end).
        struct Memo
        {
            uint32_t       Entity = 0;
            MotionKey      Key{};
            EntityScratch* Scratch = nullptr;
            Proxy*         Held    = nullptr;
        };
        EntityScratch& ScratchOf( uint32_t entity, size_t position ) noexcept;
        Proxy&         ProxyOf( MotionKey key, size_t position, bool& added ) noexcept;
        // The record's first sighting this sync: Content starts, an Add or Update reaches the registry.
        void Rigid( GpuSceneRegistry& registry, Proxy& proxy, MotionKey key, bool added,
                    const MotionRecord& record ) noexcept;
        void Reset() noexcept;

        std::unordered_map<MotionKey, Proxy, MotionKeyHash> m_Proxies;
        std::unordered_map<uint32_t, EntityScratch>         m_Entities;
        std::vector<Memo>                                   m_Memo;
        std::vector<std::pair<Proxy*, bool>> m_Touched; // this sync's proxies (added now), first sighting
        // The last full field sync, per field position: its proxy, its entity's scratch and whether the position
        // was its proxy's first record (the one whose row inputs the proxy holds). Node pointers of the two maps:
        // stable while the proxy / entity stays, and a kept sync names every one of them, so none leaves while
        // held. Cleared by Reset; m_FieldsHeld: the vector names the proxies as they stand.
        struct FieldHeld
        {
            Proxy*         Held    = nullptr;
            EntityScratch* Scratch = nullptr;
            bool           First   = false;
        };
        std::vector<FieldHeld> m_FieldHeld;
        bool                   m_FieldsHeld = false;
        // The last full rigid sync, per rigid position: its proxy, its entity's scratch (null for a part record,
        // which names none) and whether the position added an Owned to the scratch (the entity's next numbered
        // primitive, not another section of one already owned) - so a kept sync rebuilds each entity's Primitives
        // exactly as the full one did (the skinned records number after them). Pointer stability as FieldHeld.
        struct RigidHeld
        {
            Proxy*         Held    = nullptr;
            EntityScratch* Scratch = nullptr;
            bool           Owns    = false;
        };
        std::vector<RigidHeld> m_RigidHeld;
        bool                   m_RigidsHeld = false;
        // Proxies a kept field or rigid sync stamped (they are not in m_Touched: no Content to publish).
        size_t                               m_KeptClaimed   = 0;
        uint64_t                             m_List          = 0;
        uint64_t                             m_Sync          = 0;
        size_t                               m_EntitiesNamed = 0;
    };
} // namespace Desert::Graphic::GpuDriven
