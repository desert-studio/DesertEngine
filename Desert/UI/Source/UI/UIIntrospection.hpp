#pragma once

#include <Render2DCore/DrawList2D.hpp>
#include <UI/UICanvasContext.hpp>
#include <UI/UICanvasLayout.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/ResultStr.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// What the UI did this frame, in numbers a test can read.
//
// WHY THIS EXISTS AND WHY IT IS NOT A PANEL. Until now the only way to find out why a UI element is not
// where it should be, or why a canvas costs thirty draw calls instead of one, was to read
// UICanvasRenderer2D.cpp. The batch count itself already existed and was already exact —
// DrawList2D::GetCommands() is the very list the GPU backend iterates — but the only code in the engine
// that ever LOOKED at it was two test suites. Half of this task was therefore already done and invisible.
//
// So the data source comes first and the panel reads it, not the other way round. Every number below is
// derived from an artefact that already decides the frame:
//
//   * the batches, their break reasons and the geometry counts come from the DrawList2D the backend
//     draws — not from a parallel tally kept alongside it, which could drift;
//   * the elements, their rects and their skip reasons come from UI::EnumerateCanvas, the same walk the
//     editor's click-select and selection marquee are built on.
//
// NOTHING HERE RUNS UNLESS A HOST ASKS. Capture() is a call, not a hook: a host that never calls it pays
// nothing at all, and the editor calls it only while the UI Debugger panel is open. That is asserted by
// Desert/Tests/Engine/UIIntrospection rather than promised here.
namespace Desert::UI
{
    // --- Why one draw call became two -----------------------------------------------------------------
    //
    // DrawList2D extends the command it is holding when the new primitive agrees with it on texture, text
    // mode and clip rect, and when neither is a glass rect. Every batch after the first therefore exists
    // because exactly one of those tests failed, and naming which one is the whole point of this file:
    // "37 batches" is not actionable, "31 of them opened because the texture changed" is.
    //
    // The order below IS the order DrawList2D::CurrentCommand asks the questions in, so a batch is
    // attributed to the FIRST reason that fired — the same one that actually opened it.
    enum class BatchBreak : std::uint8_t
    {
        None,      // this command was merged into the previous one (never stored for a real batch)
        First,     // the first batch of the frame; nothing to merge with
        GlassPrev, // the previous batch is a glass rect and glass is never extended
        GlassSelf, // this batch is a glass rect; it carries its own rect/radius/blur in push constants
        Text,      // solid/image geometry met SDF glyphs, or the reverse: a different pipeline
        Texture,   // a different bound texture: a different descriptor set
        Material,  // a different UI-domain material: its own pipeline and its own parameter row
        ClipRect   // a different scissor
    };

    inline constexpr std::size_t kBatchBreakCount = 8;

    [[nodiscard]] const char* BatchBreakName( BatchBreak reason );

    // Why @p cur could not be merged into @p prev. Pure, and it is the mirror image of
    // DrawList2D::CurrentCommand's merge test — asserted against that function rather than described:
    // the suite requires that no two ADJACENT commands of a real draw list ever classify as None, which
    // is what turns a future change to the batch key into a red test instead of a stale panel column.
    [[nodiscard]] BatchBreak ClassifyBatchBreak( const Graphic::Render2D::DrawCommand& prev,
                                                 const Graphic::Render2D::DrawCommand& cur );

    // One recorded batch, flattened for display and for assertions.
    struct UIBatchInfo
    {
        std::uint32_t Index       = 0;
        BatchBreak    Break       = BatchBreak::None;
        const void*   Texture     = nullptr; // opaque Image2D id; null = the backend's 1x1 white
        const void*   Material    = nullptr; // opaque UIMaterialCache::Entry id; null = not a material fill
        bool          Text        = false;
        bool          Glass       = false;
        glm::vec4     ClipRect    = { 0.0f, 0.0f, 0.0f, 0.0f };
        std::uint32_t IndexCount  = 0;
        std::uint32_t IndexOffset = 0;
    };

    // The frame's cost, read off the draw list the backend consumes.
    struct UIFrameStats2D
    {
        std::uint32_t Batches = 0; // commands recorded
        // Commands the backend actually submits. Render2D::Flush skips IndexCount == 0, so in principle
        // this is a different number from Batches — MEASURED, it never is, because no DrawList2D
        // primitive opens a command without appending geometry (AddRing, the one that can emit nothing,
        // refuses at `segments < 3` before opening one). Both are kept because they answer two different
        // questions and because the day that stops being true, the suite says so.
        std::uint32_t DrawCalls    = 0;
        std::uint32_t EmptyBatches = 0;
        std::uint32_t Vertices     = 0;
        std::uint32_t Indices      = 0;
        std::uint32_t Triangles    = 0;

        // Pipeline changes across the batch sequence (UI2D / UIText / UIGlass). A pipeline bind is the
        // expensive state change; a texture change is a descriptor-set bind, which is cheaper.
        std::uint32_t PipelineSwitches = 0;
        std::uint32_t UniqueTextures   = 0; // distinct non-null texture ids = distinct descriptor sets
        // Distinct UI-domain materials filled with this frame. Each is a pipeline of its own, so this is
        // the number that says what materials cost the canvas — a texture change is a descriptor bind, a
        // material change is a pipeline bind AND a descriptor bind.
        std::uint32_t UniqueMaterials  = 0;
        std::uint32_t LargestBatchTris = 0;

        std::array<std::uint32_t, kBatchBreakCount> BreakCounts{};
    };

    // How the walk spent the canvas tree.
    struct UIWalkStats
    {
        std::uint32_t Visited = 0; // elements the walk reached, drawn or not
        std::uint32_t Drawn   = 0;
        std::uint32_t Skipped = 0; // == Visited - Drawn
        std::uint32_t Clipped = 0; // drawn, but every pixel cut away by a scissor or the viewport edge:
                                   // the walk does not cull, so these cost geometry and draw nothing
        std::uint32_t MaxDepth = 0;

        // Skipped elements by cause, indexed by UISkipCause. SIZED BY THE ENUM ITSELF: a typed 6 stood
        // here, and a seventh cause would have written past it rather than failing to compile.
        std::array<std::uint32_t, static_cast<std::size_t>( UISkipCause::Count )> SkipCounts{};
    };

    // Everything one captured frame of one VIEW knows about itself — every canvas the view drew, not one
    // of them. A view draws N canvases (Ю4) into ONE draw list, so a probe that described a single canvas
    // could only ever be right about the batches by accident: the list it read already held the geometry
    // of every canvas before it.
    struct UIFrameProbe
    {
        bool          Valid = false; // false until a capture succeeded; Refusal then says why
        std::string   Refusal;
        std::uint64_t Captures = 0; // captures that did work — the disarmed-costs-nothing assertion

        UIFrameStats2D             Stats;
        UIWalkStats                Walk;
        std::vector<UIBatchInfo>   Batches;
        std::vector<UIElementNode> Elements;

        Rect                ViewportPx{};
        std::vector<NodeId> Canvases; // in draw order — the frame's canvases, all of them

        // Forget the frame while keeping the allocated capacity — the same no-reallocation-per-frame
        // discipline DrawList2D::Reset follows, because this runs every frame the panel is open.
        void Reset();
    };

    // Read @p dl into @p out. Geometry counts and break reasons only; the element half comes from
    // CaptureFrame below.
    void CaptureDrawList( const Graphic::Render2D::DrawList2D& dl, UIFrameProbe& out );

    // Capture one frame: every canvas of @p canvases enumerated in @p tree with its own cell of the view
    // (EnumerateCanvas), and the batches @p dl ended up with, read ONCE for the frame. The registry overload
    // that finds each canvas's cell is Engine/UI/Ecs/UIIntrospectionEcs.hpp.
    // One canvas of a frame, with the cell of the view it was drawn under — nullptr when the view never drew
    // it, which is EnumerateCanvas's authoring mode (no screen, no binding is asked), not "its first screen".
    struct UICanvasWalk
    {
        NodeId                 Canvas = NodeId::Null;
        const UICanvasContext* Cell   = nullptr;
    };

    // The tree half of CaptureFrame: every canvas of @p canvases enumerated in @p tree with its own cell.
    NO_DISCARD Common::BoolResultStr CaptureFrame( const IUITree& tree, const std::vector<UICanvasWalk>& canvases,
                                                   const Graphic::Render2D::DrawList2D& dl, const Rect& viewportPx,
                                                   UIFrameProbe& out );

    // --- What one element costs, measured rather than modelled ----------------------------------------
    //
    // "Which batch did this element land in" cannot be read off a draw list: the list records geometry,
    // not who produced it. It can be MEASURED, and the measurement uses the real walk as its own oracle
    // instead of a second copy of it — walk the canvas twice into a scratch draw list, once as authored
    // and once with this element forced Hidden, and read the difference. Whatever the renderer does with
    // an element, including anything added to it after this was written, is in that difference.
    //
    // The second walk is why this is asked for ONE selected element and not for all of them.
    struct UIElementCost
    {
        bool        Valid = false;
        std::string Refusal;

        std::uint32_t Vertices  = 0; // geometry that disappears when this element is hidden
        std::uint32_t Indices   = 0;
        std::uint32_t Triangles = 0;

        std::uint32_t BatchesWith    = 0; // the canvas's batch count as authored
        std::uint32_t BatchesWithout = 0; // ... and with this element hidden

        // The first batch whose state or size changed — where this element's geometry sits. Only
        // meaningful when the element contributed anything (Indices > 0).
        std::uint32_t FirstBatch = 0;
        const void*   Texture    = nullptr;          // the texture bound by that batch
        BatchBreak    Break      = BatchBreak::None; // why that batch was opened

        // Hiding this element strictly reduces the batch count: it is one of the elements breaking the
        // canvas apart, and this is the number the whole panel exists to produce.
        bool OpensBatch = false;
    };

} // namespace Desert::UI
