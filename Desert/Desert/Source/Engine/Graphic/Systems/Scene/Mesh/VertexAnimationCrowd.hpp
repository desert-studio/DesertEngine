#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace Desert
{
    class SkinnedMesh;
}
namespace Desert::Animation
{
    class AnimationClip;
}
namespace Desert::Graphic::RayTracing
{
    class SkinCacheStreams;
}

namespace Desert::Graphic::System
{
    /**
     * @brief THE FAR CROWD (WP-FAR-12; UE 5.5 AnimToTexture / City Sample crowds): beyond skeletal range a figure
     *        plays its clip from a BAKED VERTEX ANIMATION instead of a skeleton - no pose evaluated, no palette
     *        uploaded, no skin dispatch.
     *
     * The bake (one per skinned mesh x clip, made on the first demand and kept for the process): the clip sampled
     * at kBakeFramesPerSecond over its length, each sample's skinning matrices applied on the CPU to the mesh's
     * LOWEST level's vertices exactly as the skin pass applies them (SkinCache.shader: posed position,
     * unnormalised normal / tangent / bitangent), every frame stored in the GPU skin cache's stream layout
     * (positions, previous positions = the frame before, tangent frame; RayTracingSkinCache.hpp), so a VAT
     * record's GpuScene row names one baked frame as its skin-cache streams and every surface cell, velocity and
     * caster fetches it unchanged: the far figure is the lowest skeletal level, frame for frame, and the hand-off
     * shows no pop beyond the clip's sampling.
     *
     * The hand-off (MeshRenderer::SelectSkinnedLods): a record already at its lowest level whose FOV-aware screen
     * size (the WP-FAR-10 metric) falls below kVatScreenSize goes to its VAT; it comes back above
     * kSkeletalScreenSize (hysteresis), the Animator resuming from the clock the animation ran meanwhile
     * (SkinnedLodFeedback::VatClock).
     */
    class VertexAnimationCrowd
    {
    public:
        static constexpr float kVatScreenSize       = 0.03f;
        static constexpr float kSkeletalScreenSize  = 0.04f;
        static constexpr float kBakeFramesPerSecond = 30.0f;

        VertexAnimationCrowd();
        ~VertexAnimationCrowd();
        VertexAnimationCrowd( const VertexAnimationCrowd& )            = delete;
        VertexAnimationCrowd& operator=( const VertexAnimationCrowd& ) = delete;

        // A new frame's records follow (SelectSkinnedLods): nothing is in use yet.
        void BeginFrame();
        // The device address of the baked frame of @p clip at @p seconds for @p mesh's lowest level (baked on the
        // first ask), marked in use this frame; 0 when the pair cannot be baked (logged once).
        [[nodiscard]] uint64_t FrameAddress( const SkinnedMesh& mesh, const Animation::AnimationClip& clip,
                                             float seconds );
        // SyncGpuScene, in the view's graph: the bakes in use that never went up are uploaded in it.
        void UploadInGraph( RDG::Builder& graph );
        // The view's readers: every bake in use this frame, StorageRead where the skin cache's streams are read.
        void DeclareReads( RDG::Builder& graph, std::vector<RDG::BufferRef>& reads );
        // `[Crowd] skeletal N, vat M, draws D, anim CPU X ms`: on a change of the counts, and every 60 frames.
        void Report( uint32_t skeletal, uint32_t vat, uint32_t draws );

    private:
        struct Bake
        {
            uint32_t           Frames      = 0;
            uint32_t           VertexCount = 0; // the mesh's whole vertex count: the stream layout's V
            uint64_t           SlotBytes   = 0; // one frame: V * 15 floats
            std::vector<float> Data;            // until uploaded
            std::unique_ptr<RayTracing::SkinCacheStreams> Buffer;
            bool                                          Uploaded = false;
            bool                                          Refused  = false;
            bool                                          InUse    = false;
            const RDG::Builder*                           Graph    = nullptr;
            std::optional<RDG::BufferRef>                 Ref;
        };
        [[nodiscard]] static Common::BoolResultStr BakeClip( Bake& bake, const SkinnedMesh& mesh,
                                                             const Animation::AnimationClip& clip );
        [[nodiscard]] RDG::BufferRef               RefIn( RDG::Builder& graph, Bake& bake );

        std::map<std::pair<const SkinnedMesh*, const Animation::AnimationClip*>, Bake> m_Bakes;
        uint64_t                                                                       m_ReportedCounts    = ~0ull;
        uint32_t                                                                       m_FramesSinceReport = 0;
    };
} // namespace Desert::Graphic::System
