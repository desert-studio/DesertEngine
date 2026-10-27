#pragma once

#include <Engine/Graphic/GpuDriven/GpuScene.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
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
    };
} // namespace Desert::Graphic

// GPU-DRIVEN phase GPUSCENE: THE SCENE'S PRIMITIVES BY RENDERER KEY. Owns the scene's GpuScene (Scene::
// GetGpuScene) and keeps, per MotionKey, the primitive's stable GpuScene index while the scene's views keep
// queueing it. The previous world is the row's PrevLocalToWorld, the previous palette is kept here per primitive, and every view of a frame
// reads the same rows.
//
// Lifetime of a primitive: added the first frame a view queues it (Prev == World: no object motion, as a newly
// visible object had before), kept while each synced frame queues it, removed at the first BeginFrame after a
// synced frame that did not queue it (it reappears later with no stale previous world).
namespace Desert::Graphic::GpuDriven
{
    // Where Sync put each record: rigid and skinned records -> their primitive (the push block's
    // PrimitiveIndex) and its first instance (the TLAS custom index / GI instance row); fields -> their first
    // instance (instance i of the field = FirstInstance + i).
    struct GpuSceneRecordRows
    {
        std::vector<PrimitiveIndex> RigidPrimitives;
        std::vector<InstanceIndex>  RigidInstances;
        std::vector<PrimitiveIndex> SkinnedPrimitives;
        std::vector<InstanceIndex>  SkinnedInstances;
        std::vector<InstanceIndex>  FieldInstances;
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

        // One view's queues of this frame. Numbering (unchanged from the per-view rows it replaces):
        //  - Per entity, the rigid primitives in submission order; the position IS MotionKey::Slot. Rigid records
        //    of one entity with an IDENTICAL world are one primitive (a mesh split across material slots).
        //  - Every skinned record is a primitive of its own (the entity's next slot); its row names this frame's
        //    palette and the previous one (the primitive's palette of the last synced frame, or this one when it
        //    was not drawn then or its bone count changed).
        //  - A rigid record with a Part is keyed by that part.
        //  - A field is keyed by its position among the entity's distinct transform sets.
        // A primitive an earlier view of the frame already synced keeps its row (the views agree on the frame).
        // The frame's palettes reach GpuScene::SetBonePalettes. @p out is cleared first.
        void Sync( std::span<const MotionRecord> rigid, std::span<const MotionRecord> skinned,
                   std::span<const MotionFieldRecord> fields, GpuSceneRecordRows& out );

        // The scene material table row of @p key holding @p row: added the first time a frame asks, its slots
        // re-uploaded when the values differ from the ones it holds (a parameter changed), re-added at the new
        // stride when the template's layout changed (hot reload), removed by the first BeginFrame after a frame
        // that did not ask (the primitives' rule; every view of a frame may ask). Returns the index the draw names
        // as MaterialIndex / GpuDrawSlot::MaterialRow. An empty row (a template without parameters declares no
        // Materials block) takes no row: 0, which its shader never reads.
        [[nodiscard]] uint32_t SyncMaterialRow( const MaterialRowKey& key, std::span<const glm::vec4> row );

    private:
        static constexpr uint64_t kNoFrame = ~0ull;
        struct MaterialEntry
        {
            uint32_t               Row       = 0;
            std::vector<glm::vec4> Values;               // what the table holds at Row
            uint64_t               Frame     = kNoFrame; // the last frame that asked for it
            uint64_t               PrevFrame = kNoFrame; // the one before (DiscardFrame restores it)
        };

        struct Entry
        {
            PrimitiveIndex         Primitive  = kInvalidIndex;
            uint64_t               Frame      = kNoFrame; // the last frame that queued it
            uint64_t               PrevFrame  = kNoFrame; // the one before (DiscardFrame restores it)
            const Mesh*            Mesh       = nullptr;
            uint32_t               Flags      = 0;
            uint64_t               BonesFrame = kNoFrame; // the frame Bones / BoneOffset were written for
            uint64_t               PrevBonesFrame = kNoFrame; // the frame PrevBones were written for
            uint32_t               BoneOffset = 0;
            uint32_t               PrevOffset = 0;
            std::vector<glm::mat4> Bones;
            std::vector<glm::mat4> PrevBones; // the palette before Bones (DiscardFrame swaps it back)
        };

        Entry& Touch( MotionKey key, const MotionRecord& record, bool& fresh );
        void   Queued( Entry& entry ) const;
        void   DiscardFrame();

        GpuScene                                            m_Scene;
        std::unordered_map<MotionKey, Entry, MotionKeyHash> m_Entries;
        std::unordered_map<MaterialRowKey, MaterialEntry, MaterialRowKeyHash> m_MaterialRows;
        std::vector<glm::mat4>                              m_FramePalettes;
        uint64_t                                            m_Frame         = kNoFrame;
        uint64_t                                            m_PreviousFrame = kNoFrame;
        uint64_t                                            m_FrameBeforePrevious = kNoFrame;
        bool                                                m_Started       = false;
        bool                                                m_Committed     = false; // CommitFrame( m_Frame )
    };
} // namespace Desert::Graphic::GpuDriven
