#pragma once

// The engine side of UI/UIIntrospection.hpp: the `entt::registry&` overloads every engine and editor caller uses.
// Each wraps the registry in an EcsUITree and asks the IUITree overload of the framework (DesertUI), so there is
// one walk, not one per storage. The framework itself never names entt (Desert/Tests/Engine/UIFrameworkBoundary).
#include <UI/UIIntrospection.hpp>

#include <Engine/UI/Ecs/EcsUITree.hpp>

namespace Desert::UI
{
    // Capture one frame of @p view: the elements of every canvas in @p canvases (through EnumerateCanvas,
    // honouring each canvas's OWN cell of @p view — its screen and its bindings) and the batches @p dl
    // ended up with, read ONCE for the frame. Refuses, into out.Refusal, when a canvas is not drawable in
    // @p reg — an empty successful probe would read as "this canvas costs nothing", which is the silent
    // wrong answer this project forbids.
    //
    // The registry overload (Engine/UI/Ecs/UIIntrospectionEcs.cpp) finds each canvas's cell in @p view and
    // asks the tree overload below.
    NO_DISCARD Common::BoolResultStr CaptureFrame( const UIViewContext& view, entt::registry& reg,
                                                   const std::vector<entt::entity>&     canvases,
                                                   const Graphic::Render2D::DrawList2D& dl, const Rect& viewportPx,
                                                   UIFrameProbe& out );

    // Measure @p element inside @p canvas. Mutates nothing that outlives the call: the walk runs against a
    // COPY of @p ctx with a zero timestep and no input, so hover eases, tween clocks and the hot election
    // are not disturbed, and the element's Visibility is restored before returning.
    [[nodiscard]] UIElementCost ProbeElementCost( const UIViewContext& view, entt::registry& reg,
                                                  entt::entity canvas, entt::entity element,
                                                  const Rect& viewportPx );

    // A host's slot for the probe, and the ONE place "is anybody looking?" is asked.
    //
    // WHY THE GATE IS A TYPE AND NOT AN `if` AT THE CALL SITE. The requirement is that a closed panel
    // costs nothing, and the difference between a promise and a fact is whether a test can read it. With
    // the branch inside Capture(), the suite can disarm a sink, walk a real canvas past it and assert that
    // Captures() never moved and that the frame's vectors never allocated — which is the claim, stated as
    // an assertion. A branch written at each call site would have to be re-proved at each call site.
    class UIFrameProbeSink
    {
    public:
        [[nodiscard]] bool IsArmed() const
        {
            return m_Armed;
        }

        // Disarming DROPS the captured frame. A panel that closes and reopens must not show numbers from
        // a frame that has since been redrawn — a stale reading is worse than no reading, because it
        // still looks like evidence.
        void SetArmed( bool armed );

        // Returns immediately, having touched nothing, while disarmed.
        void Capture( const UIViewContext& view, entt::registry& reg, const std::vector<entt::entity>& canvases,
                      const Graphic::Render2D::DrawList2D& dl, const Rect& viewportPx );

        // Ask for one element to be measured (see UIElementCost). THE MEASUREMENT IS NOT TAKEN HERE: it
        // needs the view's UIViewContext, which only the pass that draws the canvas holds, so the
        // request is answered on that pass's next frame. One frame of latency, and the alternative —
        // copying the whole context out every frame so a panel could walk with it — would charge every
        // armed frame for something asked once.
        void RequestElementCost( entt::entity e )
        {
            m_CostRequest = e;
        }
        [[nodiscard]] const UIElementCost& ElementCost() const
        {
            return m_Cost;
        }
        // Which element ElementCost() describes; entt::null when nothing has been measured.
        [[nodiscard]] entt::entity CostSubject() const
        {
            return m_CostSubject;
        }

        [[nodiscard]] const UIFrameProbe& Frame() const
        {
            return m_Frame;
        }
        // Captures that did work. The disarmed-costs-nothing assertion is written against this.
        [[nodiscard]] std::uint64_t Captures() const
        {
            return m_Frame.Captures;
        }

    private:
        bool          m_Armed = false;
        UIFrameProbe  m_Frame;
        entt::entity  m_CostRequest = entt::null;
        entt::entity  m_CostSubject = entt::null;
        UIElementCost m_Cost;
    };
} // namespace Desert::UI
