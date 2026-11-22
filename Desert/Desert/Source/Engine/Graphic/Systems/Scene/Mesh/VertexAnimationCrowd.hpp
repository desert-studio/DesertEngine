#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
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
     * The vertex animation (one per skinned mesh x clip, Assets/VertexAnimationDerivedData.hpp): DERIVED DATA, not
     * a runtime bake (WP-FAR-12B; UE AnimToTexture) - the package cook derives it, the editor may derive a miss
     * into the DDC, a packaged game only loads it. It holds the mesh's LOWEST level's vertices only, in the skin
     * cache's stream layout over that range, so a VAT record's GpuScene row names one baked frame (biased base
     * address + the range's vertex count) as its skin-cache streams and every surface cell, velocity and caster
     * fetches it unchanged: the far figure is the lowest skeletal level, frame for frame, and the hand-off shows
     * no pop beyond the clip's sampling. Loaded on the first ask, kept for the process.
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
        // THE FAR FIGURE'S MINIMUM SCREEN SIZE (WP-FAR-12D; UE City Sample crowds / impostors keep a far figure
        // readable down to its cull distance instead of letting it rasterise to a sub-pixel dot): a VAT record
        // whose TRUE projected size (the FOV-aware bounding-sphere diameter as a fraction of the view's height,
        // LODDistanceScale removed - the clamp is about pixels, not about the level picked) falls below
        // kMinScreenSize is drawn grown about its feet to that size, in every pass of the frame (one world per
        // row: its cull bounds, shadow and velocity agree). Capped at kMaxMinScreenScale: past the distance the
        // cap reaches, the figure shrinks again with distance, as the far draw fades out toward its cull. The
        // clamp applies to VAT and skeletal records alike (WP-FAR-10D: a quality level that keeps a figure
        // skeletal must not make it vanish). Figures above the size are drawn at their own size - silhouettes
        // up close and in a scope's narrow FOV (which raises the true size) are the authored ones.
        static constexpr float kMinScreenSize     = 0.012f;
        static constexpr float kMaxMinScreenScale = 6.0f;

        VertexAnimationCrowd();
        ~VertexAnimationCrowd();
        VertexAnimationCrowd( const VertexAnimationCrowd& )            = delete;
        VertexAnimationCrowd& operator=( const VertexAnimationCrowd& ) = delete;

        // A new frame's records follow (SelectSkinnedLods): nothing is in use yet.
        void BeginFrame();
        // One frame as a row's skin-cache streams: the (range-biased) address and the baked level's vertex count.
        struct Frame
        {
            uint64_t Address     = 0;
            uint32_t VertexCount = 0;
        };
        // The baked frame of @p clip at @p seconds for @p mesh's lowest level (loaded from the DDC on the first
        // ask), marked in use this frame; Address 0 when the pair has no vertex animation (logged once).
        [[nodiscard]] Frame FrameAddress( const SkinnedMesh& mesh, const Animation::AnimationClip& clip,
                                          float seconds );
        // SyncGpuScene, in the view's graph: the bakes in use that never went up are uploaded in it.
        void UploadInGraph( RDG::Builder& graph );
        // The view's readers: every bake in use this frame, StorageRead where the skin cache's streams are read.
        void DeclareReads( RDG::Builder& graph, std::vector<RDG::BufferRef>& reads );
        // `[Crowd] skeletal N, vat M, draws D, grown to min screen size G (max xS), anim CPU X ms`: on a change of
        // the counts, and every 60 frames.
        void Report( uint32_t skeletal, uint32_t vat, uint32_t draws, uint32_t grown, float maxScale );

    private:
        struct Bake
        {
            uint32_t           Frames      = 0;
            uint32_t           FirstVertex = 0; // the baked level's range in the mesh's vertex order
            uint32_t           VertexCount = 0;
            uint64_t           SlotBytes   = 0; // one frame: VertexCount * 15 floats
            std::string        Data;            // the entry's slots, until uploaded
            std::unique_ptr<RayTracing::SkinCacheStreams> Buffer;
            bool                                          Uploaded = false;
            bool                                          Refused  = false;
            bool                                          InUse    = false;
            const RDG::Builder*                           Graph    = nullptr;
            std::optional<RDG::BufferRef>                 Ref;
        };
        [[nodiscard]] static Common::BoolResultStr LoadClip( Bake& bake, const SkinnedMesh& mesh,
                                                             const Animation::AnimationClip& clip );
        [[nodiscard]] RDG::BufferRef               RefIn( RDG::Builder& graph, Bake& bake );

        std::map<std::pair<const SkinnedMesh*, const Animation::AnimationClip*>, Bake> m_Bakes;
        uint64_t                                                                       m_ReportedCounts    = ~0ull;
        uint32_t                                                                       m_FramesSinceReport = 0;
    };
} // namespace Desert::Graphic::System
