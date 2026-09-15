#pragma once

#include <cstdint>

namespace Desert::Engine::RendererSlotBudget
{
    /**
     * @brief Who is asking for a renderer slot, and therefore how much it may take.
     *
     * WHY THERE IS A RULE AT ALL. A Graphic::SceneRenderer that finds every slot taken does NOT fail. It
     * records into slot 0 and shares the main view's per-frame state (RendererSlotPool.hpp next door says
     * so in its own header), which reads as "my preview moved when I moved the scene camera" and has cost
     * this project days. There are six slots (Engine::kMaxRendererSlots) and no seventh, so somebody has
     * to yield — and the only question worth writing down is WHO.
     *
     * WHY IT IS ITS OWN HEADER. Several callers need the same answer: the Details panel, deciding whether
     * to build the live preview a click just asked for; ThumbnailService, deciding whether to build the
     * renderer its capture queue needs; and the UI render-texture producer, deciding whether the element
     * an author put on a canvas may have a world behind it. The first two were written a screen apart
     * with the arithmetic spelled out separately — `live >= max` in one, `live + 1 >= max` in the other —
     * which is two expressions of one policy that nothing forces to agree. That is the defect shape this
     * repository keeps paying for.
     *
     * WHY IT LIVES IN THE ENGINE AND NOT IN THE EDITOR, corrected Ю16. It was
     * `Editor/Widgets/PreviewSlotBudget.hpp`, and the name and the address were both wrong the moment a
     * SHIPPED thing needed the rule: RuntimeLayer walks the same canvases the editor does
     * (UI::RenderCanvas2D), so a UI element that hosts a world asks this question inside the packaged
     * game, where nothing in `Editor/` is linked at all. A policy that answers only in the editor is not
     * a policy — it is a policy plus a silent second copy in the process that ships.
     *
     * WHY IT NAMES NO HEADER OF ITS OWN. `max` is a parameter, so this file never needs
     * Engine::kMaxRendererSlots and must not include the frame manager to say a number it does not use.
     * That is what keeps the suite below buildable against `Common` alone.
     *
     * WHY IT IS HEADER-ONLY AND CONSTEXPR. RendererSlotPool was split out of SceneRenderer for exactly
     * this reason: a rule about a lease nobody can observe from a frame has to be drivable by a test
     * without a device. Reaching a sustained six-of-six by hand needs several scene views open at once,
     * which the editor's control channel cannot do (they live behind a menu), so a frame CANNOT prove
     * this and a test can. Desert/Tests/Engine/RendererSlotBudget is that test.
     */
    enum class Demand
    {
        /// A surface the person opened and is looking at: a scene view, a material document, the Details
        /// preview under the row they just clicked, a render-texture element the author placed on a
        /// canvas. It may take the LAST slot — refusing it would be refusing the thing that was asked
        /// for, in favour of something that was not.
        UserSurface,

        /// Work nobody asked for by name: an asset thumbnail whose only consumer is a 64 px square that
        /// already draws a placeholder. It may not take the last slot, because the next thing the person
        /// opens has to have somewhere to go — and because the picture it produces is precisely what that
        /// person sees while they cannot have the live one.
        Background
    };

    /**
     * @brief May @p who claim a slot, given @p live already in use out of @p max?
     *
     * Total and pure. `live` is Graphic::SceneRenderer::GetLiveRendererCount(), which IS the slot mask's
     * population count, so there is no second counter to disagree with it.
     */
    [[nodiscard]] constexpr bool MayClaim( Demand who, uint32_t live, uint32_t max ) noexcept
    {
        // The whole policy, in one line: background work keeps one slot in reserve, a user surface does
        // not. Written as a reservation rather than as two comparisons so that the two answers cannot
        // drift into disagreeing about the same situation.
        const uint32_t reserved = ( who == Demand::Background ) ? 1u : 0u;
        return live + reserved < max;
    }
} // namespace Desert::Engine::RendererSlotBudget
