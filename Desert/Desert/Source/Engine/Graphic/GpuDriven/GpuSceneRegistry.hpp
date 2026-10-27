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
// queueing it. Replaces the per-view MotionHistory and BuildObjectMotionRows (both deleted): the previous world
// is the row's PrevLocalToWorld, the previous palette is kept here per primitive, and every view of a frame
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

        // Once per absolute frame (every view calls it; the later calls of one frame do nothing): drops the
        // primitives the last synced frame did not queue, then GpuScene::BeginFrame.
        void BeginFrame( uint64_t frame );

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

    private:
        struct Entry
        {
            PrimitiveIndex         Primitive  = kInvalidIndex;
            uint64_t               Frame      = 0; // the last frame that queued it
            const Mesh*            Mesh       = nullptr;
            uint32_t               Flags      = 0;
            uint64_t               BonesFrame = 0; // the frame Bones / BoneOffset were written for
            uint32_t               BoneOffset = 0;
            uint32_t               PrevOffset = 0;
            std::vector<glm::mat4> Bones;
        };

        Entry& Touch( MotionKey key, const MotionRecord& record, bool& fresh );

        GpuScene                                            m_Scene;
        std::unordered_map<MotionKey, Entry, MotionKeyHash> m_Entries;
        std::vector<glm::mat4>                              m_FramePalettes;
        uint64_t                                            m_Frame         = 0;
        uint64_t                                            m_PreviousFrame = 0;
        bool                                                m_Started       = false;
    };
} // namespace Desert::Graphic::GpuDriven
