#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Settings/CapabilityCatalog.hpp>
#include <Common/Settings/Scalability.hpp>
#include <Engine/Graphic/ViewMemory.hpp>

#include <glm/glm.hpp>

#include <cstdint>

// TAA1 — THE PER-FRAME VIEW (UE: FViewInfo built from FSceneViewState). What every pass of one view reads for one
// frame: matrices, their previous-frame copies, the sub-pixel jitter and the two extents of the resolution split.
//
// Why it exists. Before TAA1 each temporal consumer kept its own previous matrix (SSRRenderer::m_PrevViewProj,
// GIResolveRenderer::m_PrevViewProj, VolumetricCloudRenderer::m_PrevViewProj) and wrote it at its own point of the
// frame. Three copies of one value drift: a renderer that skips a frame (GI off for one frame, clouds culled)
// reprojects through a matrix two frames old, and a renderer shared by two views (the editor viewport and a
// preview) reprojects one view's pixels through the other view's camera. TAA would be a fourth copy and the
// velocity pass a fifth. Now: ONE previous-frame source per view (SceneViewState), ONE immutable product per frame
// (ViewFrame), and the per-renderer copies are deleted with no bridge (REMAINDER-TAA1-C0.md, step 3).
//
// The camera stays the lens and the pose (ECS::CameraData / Core::Camera): it never sees jitter, history or a
// previous matrix, because one camera can feed several views and each view has its own history.
namespace Desert::Graphic
{
    // ---- The resolution split (SCAL1 axis 2) ------------------------------------------------------------
    //
    // ViewExtent is ViewMemory.hpp's (the extent a view's targets are built at); not redeclared.
    // Every view renders its scene at RenderExtent and presents at OutputExtent. One split for the three modes of
    // Common::Scalability::ScaleMode, so the scene passes never ask which mode is on:
    //   * Upscale     (percent < 100): RenderExtent < OutputExtent; the ITemporalUpscaler (TAAU) reconstructs;
    //   * Native      (percent == 100): equal extents; TAA, if on, resolves in place;
    //   * Supersample (percent > 100): RenderExtent > OutputExtent; SSAA — the fixed downsample filter after the
    //     temporal pass (or in its place when the method is not temporal).
    // Everything before the temporal / downsample pass (GBuffer, lighting, SSR, GI, clouds, fog, translucency) is
    // at RenderExtent; everything after it (bloom, exposure, tonemap, post AA, UI, editor overlays) at
    // OutputExtent.
    struct ResolutionSplit
    {
        ViewExtent                     Render;
        ViewExtent                     Output;
        int                            RenderScalePercent = 100;
        Common::Scalability::ScaleMode Mode               = Common::Scalability::ScaleMode::Native;

        bool operator==( const ResolutionSplit& ) const = default;
    };

    // PURE. Render = round-to-nearest(Output * percent / 100) per axis, never below 1 (a 1-pixel-high preview at
    // 50 % is still a view). Mode follows the percent exactly as SCAL1's Resolve classified it, so the split and
    // ResolvedQuality::Scale can never disagree. @p output empty -> empty Render (a minimised viewport draws
    // nothing; SceneViewState treats the resize back as a history reset).
    // Error (both extents and the percent named) when a Render side would exceed kMaxViewExtentSide (ViewMemory):
    // SSAA at 200 % of a 10K output cannot be allocated, and silently clamping the percent would be a fallback.
    [[nodiscard]] Common::ResultStr<ResolutionSplit> MakeResolutionSplit( ViewExtent output,
                                                                          int        renderScalePercent );

    // ---- The temporal method of this frame -------------------------------------------------------------
    //
    // What resolves the frame's samples over time, derived from the two SCAL1 axes after
    // ResolveAntiAliasingForPath. Vendor upscalers are not named here: they arrive as further ITemporalUpscaler
    // implementations behind Common::Scalability::Upscaler, and SelectTemporalMethod refuses a value with no
    // implementation (it is never offered by CapabilityCatalog until one exists, so reaching it means the catalog
    // and the build disagree).
    enum class TemporalMethod : uint8_t
    {
        None, // FXAA / SMAA / MSAA / None at Native or Supersample: no jitter, no history, no temporal pass
        TAA,  // AntiAliasingMethod::TAA at Native or Supersample: same-extent temporal resolve
        TAAU, // Upscaler::TAAU below 100 %: the temporal resolve writes OutputExtent
    };

    // TAA QUALITY. Read by the TAA/TAAU resolve shader as the TAA_QUALITY shader variant (one pipeline per level):
    // each level is a different history-rejection filter, never a different jitter length, so a quality change
    // does not reset the history.
    //   Low    — 5-tap plus neighbourhood min/max clamp, bilinear history fetch;
    //   Medium — 3x3 YCoCg variance clip, bilinear history fetch;
    //   High   — 3x3 YCoCg variance clip, 5-tap Catmull-Rom history fetch (UE r.TemporalAA.Quality 2).
    // Its value comes from the SCAL1 parameter TemporalAAQuality (AntiAliasing group) — a row SCAL1 does not have
    // yet; REMAINDER-TAA1-C0.md lists it for the lead. The AntiAliasingSamples value the SCAL1 table gives TAA
    // levels has no reader under TAA and must not be read as a quality.
    enum class TemporalAAQuality : uint8_t
    {
        Low = 0,
        Medium,
        High,
    };

    // PURE. TAA iff the path's effective method is TAA and the split is Native/Supersample; TAAU iff the split is
    // Upscale and the upscaler is TAAU; None for a non-temporal method at Native/Supersample. An Upscale split
    // with Upscaler::None, or any vendor upscaler (FSR / DLSS / XeSS / MetalFX: no implementation in this build)
    // is an error naming the value — SCAL1's Resolve never produces either, so it is a contract break, not a
    // fallback.
    [[nodiscard]] Common::ResultStr<TemporalMethod>
    SelectTemporalMethod( const Common::Scalability::PathAntiAliasing& path, const ResolutionSplit& split,
                          Common::Scalability::Upscaler upscaler );

    // ---- Sub-pixel jitter (UE: Halton(2,3) sample offsets) ---------------------------------------------
    //
    // PURE. Sequence length: 8 at Native/Supersample (UE r.TemporalAASamples), and at Upscale 8 x (output pixels /
    // render pixels) rounded up, capped at 64 (UE: TAAU keeps the samples-per-OUTPUT-pixel constant). 0 for
    // TemporalMethod::None — no jitter at all, so a non-temporal frame is bit-identical to the pre-TAA1 frame.
    [[nodiscard]] uint32_t TemporalJitterSequenceLength( TemporalMethod method, const ResolutionSplit& split );

    // PURE. Halton(base 2, base 3) at 1-based index (index % length) + 1, minus 0.5: an offset in RENDER pixels in
    // [-0.5, 0.5) per axis. +x right, +y down (Vulkan framebuffer and NDC agree on y-down). Length 0 -> (0, 0).
    [[nodiscard]] glm::vec2 TemporalJitterPixels( uint32_t index, uint32_t length );

    // PURE. The same offset in NDC: 2 * pixels / RenderExtent. Empty extent -> (0, 0).
    [[nodiscard]] glm::vec2 JitterPixelsToNdc( glm::vec2 pixels, ViewExtent render );

    // PURE. Translate(ndc.x, ndc.y, 0) * projection: shifts every clip-space point by ndc * w, i.e. the whole
    // image by exactly @p ndc in NDC, for a perspective (w = -z) and an orthographic (w = 1) projection alike.
    // Adding the offset to projection[2][0..1] instead is right for perspective only and moves an ortho view not
    // at all.
    [[nodiscard]] glm::mat4 ApplyJitter( const glm::mat4& projection, glm::vec2 ndc );

    // ---- Why a history may not be read -----------------------------------------------------------------
    enum class HistoryResetReason : uint8_t
    {
        None = 0,             // history valid: previous matrices and history textures belong to the last frame
        FirstFrame,           // the view has never rendered
        CameraCut,            // ViewInputs::CameraCut, or the view now follows a different camera
        Resize,               // OutputExtent or RenderExtent changed (history textures are recreated)
        TemporalMethodChange, // TemporalMethod changed (incl. to/from None): history shape or meaning changed
        PassFault,            // RDG-FAULT1: the pass that writes the history was removed (InvalidateHistory)
    };

    // ---- THE PER-FRAME VIEW ----------------------------------------------------------------------------
    //
    // Immutable once SceneViewState::BeginFrame returned it. Matrices are right-handed, reversed-Z
    // (Core/Projection), world units centimetres.
    //
    // WHICH MATRIX A PASS USES (the rule a reviewer checks):
    //   * rasterising scene geometry into the depth/GBuffer/forward targets: JitteredViewProjection;
    //   * reconstructing a world position from that depth (lighting, SSAO, SSR, GI, fog, clouds' depth test):
    //     InvJitteredViewProjection — the depth was written jittered;
    //   * reprojecting into the previous frame (velocity, TAA, SSR/GI/cloud temporal reuse): world position ->
    //     PrevViewProjection (UNJITTERED), compared with the current UNJITTERED ViewProjection. Velocity therefore
    //     carries motion only, never the jitter, and a still camera over a still scene has velocity exactly 0;
    //   * anything drawn after the temporal pass at OutputExtent (editor grid, gizmos, collider lines, UI in 3D):
    //     ViewProjection — unjittered, or the overlay shimmers by the jitter;
    //   * sky / clouds ray directions: InvViewProjection of the unjittered matrices plus JitterNdc applied to the
    //     pixel's NDC, so the sky jitters with the geometry it is resolved together with.
    struct ViewFrame
    {
        uint64_t FrameIndex = 0; // this view's frame count (SceneViewState), not the engine's

        // Pose and lens of this frame. View / Projection are the camera's, unmodified.
        glm::mat4 View{ 1.0f };
        glm::mat4 InvView{ 1.0f };
        glm::mat4 Projection{ 1.0f }; // unjittered
        glm::mat4 InvProjection{ 1.0f };
        glm::mat4 JitteredProjection{ 1.0f }; // ApplyJitter( Projection, JitterNdc )
        glm::mat4 ViewProjection{ 1.0f };     // Projection * View
        glm::mat4 InvViewProjection{ 1.0f };
        glm::mat4 JitteredViewProjection{ 1.0f }; // JitteredProjection * View
        glm::mat4 InvJitteredViewProjection{ 1.0f };
        glm::vec3 CameraPosition{ 0.0f }; // world, cm
        float     NearPlane = 0.0f;       // cm, as the camera gave it (the projection holds it reversed)
        float     FarPlane  = 0.0f;

        // The previous frame of THIS view. Equal to the current values when HistoryReset != None (a reset view
        // reprojects onto itself: velocity of a static pixel is 0, not garbage from another camera).
        glm::mat4 PrevView{ 1.0f };
        glm::mat4 PrevProjection{ 1.0f };     // unjittered
        glm::mat4 PrevViewProjection{ 1.0f }; // unjittered
        glm::mat4 PrevInvViewProjection{ 1.0f };
        glm::mat4 PrevJitteredViewProjection{ 1.0f };
        glm::vec3 PrevCameraPosition{ 0.0f };

        // Jitter of this frame and of the previous one (TAA removes the previous jitter when it samples history).
        uint32_t  JitterIndex          = 0; // position in the sequence, < JitterSequenceLength (0 when length 0)
        uint32_t  JitterSequenceLength = 0; // TemporalJitterSequenceLength( Method, Split )
        glm::vec2 JitterPixels{ 0.0f };     // render pixels, [-0.5, 0.5)
        glm::vec2 JitterNdc{ 0.0f };
        glm::vec2 PrevJitterNdc{ 0.0f };

        ResolutionSplit   Split;
        TemporalMethod    Method  = TemporalMethod::None;
        TemporalAAQuality Quality = TemporalAAQuality::Medium;

        // Texture LOD bias every material sampler adds while Split.Mode is Upscale: log2(Render.Width /
        // Output.Width) (negative), so textures are sampled at the detail of the OUTPUT pixel the upscaler
        // reconstructs (UE: the screen-percentage mip bias). 0 at Native and Supersample.
        float MaterialMipBias = 0.0f;

        // Time of this frame and of the view's previous frame, seconds. Readers: foliage/grass wind evaluated at
        // both times for the previous vertex position; particles integrate their previous position over Delta.
        double TimeSeconds     = 0.0;
        double PrevTimeSeconds = 0.0;
        float  DeltaSeconds    = 0.0f; // TimeSeconds - PrevTimeSeconds; 0 on a reset

        // None: the history textures and every Prev* above belong to the previous frame of this view. Otherwise
        // the temporal pass writes the current frame with no history blend (UE: bCameraCut).
        HistoryResetReason HistoryReset = HistoryResetReason::FirstFrame;

        [[nodiscard]] bool HistoryValid() const
        {
            return HistoryReset == HistoryResetReason::None;
        }
    };

    // PURE. The frame of a camera that is NOT a SceneViewState view — a shadow cascade's light camera, an editor
    // preview drawing one overlay with no temporal history: no jitter (the jittered matrices are the unjittered
    // ones), no previous frame (every Prev* equals the current value, HistoryReset FirstFrame), MaterialMipBias 0.
    // It exists so the camera block has ONE writer (ShaderProtocols::MakeCameraUB) whatever drives the camera.
    [[nodiscard]] ViewFrame MakeStillViewFrame( const glm::mat4& view, const glm::mat4& projection,
                                                const glm::vec3& cameraPosition, double timeSeconds );
} // namespace Desert::Graphic
