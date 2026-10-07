#pragma once

#include <Engine/Graphic/View/SceneViewState.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace Desert::Graphic
{
    // One queued mesh record as the row builder sees it: the owning entity, the world it is drawn with this frame
    // and, for a skinned record, this frame's bone palette (empty for a rigid one).
    struct MotionRecord
    {
        uint32_t                   Entity = 0;
        glm::mat4                  World{ 1.0f };
        std::span<const glm::mat4> Bones;
    };

    // The CPU half of MeshRenderer::BuildObjectMotions (no device): what the view's ObjectMotions / ObjectBones
    // buffers hold this frame, and the row each record draws with.
    struct ObjectMotionRows
    {
        std::vector<GpuObjectMotion> Rows;       // ONE per drawn primitive (UE GPUScene primitive data)
        std::vector<glm::mat4>       Palettes;   // both frames' palettes of every skinned primitive, end to end
        std::vector<uint32_t>        RecordRows; // per record, rigid records first then skinned: its index in Rows

        void Clear()
        {
            Rows.clear();
            Palettes.clear();
            RecordRows.clear();
        }
    };

    // Numbers this frame's primitives of one view and records each one's world (and palette) in @p motion.
    //  - Per entity, the rows it owns in submission order; a row's index in that list IS its MotionKey::Slot, so
    //    the same submission gives the same keys every frame.
    //  - Rigid records (static, generic, slot-material) of one entity with an IDENTICAL world are one primitive
    //    (a mesh split across material slots) and share a row; another world is the entity's next slot.
    //  - Every skinned record takes its own slot; its row names this frame's palette (BoneOffset) and the previous
    //    one (PrevBoneOffset, MotionHistory::PreviousBones) in Palettes.
    //  - A primitive the view did not draw last frame gets PrevWorld == World (and its own palette as previous):
    //    camera motion only.
    // @p out is cleared first (capacity kept).
    void BuildObjectMotionRows( MotionHistory& motion, std::span<const MotionRecord> rigid,
                                std::span<const MotionRecord> skinned, ObjectMotionRows& out );
} // namespace Desert::Graphic
