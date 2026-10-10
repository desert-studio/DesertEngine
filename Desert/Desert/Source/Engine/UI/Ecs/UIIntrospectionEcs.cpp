#include <Engine/ECS/Components.hpp>
#include <Engine/UI/Ecs/EcsUITree.hpp>
#include <Engine/UI/Ecs/UICanvasLayoutEcs.hpp>
#include <Engine/UI/Ecs/UICanvasRendererEcs.hpp>
#include <Engine/UI/Ecs/UIIntrospectionEcs.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

// The ECS half of UI introspection. CaptureFrame's registry overload wraps the scene in an EcsUITree and
// looks each canvas's cell up in the view; the element-cost probe and the per-frame sink stay here whole
// because the cost is measured by DRAWING the canvas twice, and the canvas renderer still walks the
// registry (UI-FW2 moves it onto the tree, and this file shrinks to the lookups).
namespace Desert::UI
{
    Common::BoolResultStr CaptureFrame( const UIViewContext& view, entt::registry& reg,
                                        const std::vector<entt::entity>&     canvases,
                                        const Graphic::Render2D::DrawList2D& dl, const Rect& viewportPx,
                                        UIFrameProbe& out )
    {
        // EACH CANVAS WITH ITS OWN CELL of this view, never a neighbour's — and a canvas this view has not
        // drawn has none, which is EnumerateCanvas's authoring mode rather than "its first screen".
        std::vector<UICanvasWalk> walks;
        walks.reserve( canvases.size() );
        for ( const entt::entity canvas : canvases )
            walks.push_back( UICanvasWalk{ ToNode( canvas ), view.FindCanvasState( ToNode( canvas ) ) } );
        return CaptureFrame( EcsUITree( reg ), walks, dl, viewportPx, out );
    }

    void UIFrameProbeSink::SetArmed( bool armed )
    {
        if ( m_Armed == armed )
            return;
        m_Armed = armed;
        if ( !armed )
        {
            m_Frame.Reset();
            m_CostRequest = entt::null;
            m_CostSubject = entt::null;
            m_Cost        = UIElementCost{};
        }
    }

    void UIFrameProbeSink::Capture( const UIViewContext& view, entt::registry& reg,
                                    const std::vector<entt::entity>&     canvases,
                                    const Graphic::Render2D::DrawList2D& dl, const Rect& viewportPx )
    {
        if ( !m_Armed )
            return;
        // The refusal is kept in the frame (Valid stays false, Refusal names the canvas problem) rather
        // than logged from here: this runs once per frame, and a refusal at frame rate buries the log —
        // the same reason EditorUIPass reports its own canvas refusal only when it changes.
        (void)CaptureFrame( view, reg, canvases, dl, viewportPx, m_Frame );

        // The element measurement is answered HERE and only when it was asked for: it costs two extra
        // walks of the canvas, which is why it is a request rather than a column of the table.
        if ( m_CostRequest != entt::null )
        {
            m_CostSubject = m_CostRequest;
            m_CostRequest = entt::null;
            // WHICH CANVAS the element belongs to is DERIVED from the element, not from the frame's list:
            // CanvasOf walks its ancestors, so it is exact. Asking the frame would have to pick one of N.
            m_Cost = ProbeElementCost( view, reg, CanvasOf( reg, m_CostSubject ), m_CostSubject, viewportPx );
        }
    }

    UIElementCost ProbeElementCost( const UIViewContext& view, entt::registry& reg, entt::entity canvas,
                                    entt::entity element, const Rect& viewportPx )
    {
        UIElementCost cost;

        if ( !reg.valid( element ) || element == canvas )
        {
            cost.Refusal = "no element is selected in this canvas, so there is nothing to measure";
            return cost;
        }
        if ( !reg.has<ECS::UILayoutComponent>( element ) )
        {
            // The measurement works by hiding the element, and Visibility lives on the layout component.
            // An element without one cannot be hidden, so the difference cannot be taken — said plainly
            // rather than returned as a row of zeroes.
            cost.Refusal = "this entity has no UILayoutComponent, so it cannot be hidden and its cost "
                           "cannot be measured by difference";
            return cost;
        }

        // Both walks run against a COPY of the view's context. The real one keeps its hover eases, tween
        // clocks and hot election: a debug measurement that moved them would change the picture it is
        // measuring. DrivesSceneAnimation is off for the same reason the UI Editor preview turns it off —
        // a second walk advancing the shared UIAnim playheads runs every clip at double speed.
        const auto RunWalk = [&]( Graphic::Render2D::DrawList2D& dl ) -> bool
        {
            UIViewContext probe        = view;
            probe.DrivesSceneAnimation = false;
            dl.Reset();
            // A zero step: both walks must draw the SAME instant (the copy starts at the view's own time), or
            // the difference would include a tween's or marquee's motion between them.
            BeginUIFrame( probe, reg, viewportPx, /*frameDtSeconds=*/0.0f );
            const bool drawn = RenderCanvas2D( probe, reg, canvas, dl ).IsSuccess();
            EndUIFrame( probe, reg, dl, /*input=*/nullptr );
            return drawn;
        };

        Graphic::Render2D::DrawList2D authored;
        if ( !RunWalk( authored ) )
        {
            cost.Refusal = "the canvas refused to draw, so there is no baseline to measure against";
            return cost;
        }

        Graphic::Render2D::DrawList2D without;
        {
            // Restored on every path out, including the walk throwing: the scene must leave this function
            // exactly as it entered it.
            struct VisibilityRestore
            {
                UIVisibility& Field;
                UIVisibility  Previous;
                ~VisibilityRestore()
                {
                    Field = Previous;
                }
            };
            auto&             field = reg.get<ECS::UILayoutComponent>( element ).Data.Visibility;
            VisibilityRestore restore{ field, field };
            field = UIVisibility::Hidden;

            if ( !RunWalk( without ) )
            {
                cost.Refusal = "the canvas refused to draw with this element hidden";
                return cost;
            }
        }

        const auto& withCmds    = authored.GetCommands();
        const auto& withoutCmds = without.GetCommands();

        cost.Valid          = true;
        cost.BatchesWith    = static_cast<std::uint32_t>( withCmds.size() );
        cost.BatchesWithout = static_cast<std::uint32_t>( withoutCmds.size() );
        cost.OpensBatch     = cost.BatchesWithout < cost.BatchesWith;
        cost.Vertices =
             static_cast<std::uint32_t>( authored.GetVertices().size() -
                                         std::min( authored.GetVertices().size(), without.GetVertices().size() ) );
        cost.Indices =
             static_cast<std::uint32_t>( authored.GetIndices().size() -
                                         std::min( authored.GetIndices().size(), without.GetIndices().size() ) );
        cost.Triangles = cost.Indices / 3;

        // WHERE the geometry sat, found through the VERTICES rather than through the commands. Comparing
        // the two command streams position by position does not answer this: removing an element lets the
        // runs on either side of it merge, so command 0 changes size even though nothing of this element
        // was ever in it. The vertex buffers, on the other hand, share an exact prefix — everything drawn
        // before this element is emitted identically — so the first vertex they disagree about is this
        // element's first vertex, and the batch that references it is the batch it landed in.
        const auto&       withVerts = authored.GetVertices();
        const auto&       cutVerts  = without.GetVertices();
        const std::size_t shared    = std::min( withVerts.size(), cutVerts.size() );
        std::size_t       firstVert = shared;
        for ( std::size_t i = 0; i < shared; ++i )
        {
            if ( !( withVerts[i] == cutVerts[i] ) )
            {
                firstVert = i;
                break;
            }
        }

        if ( cost.Indices > 0 && firstVert < withVerts.size() )
        {
            const auto& indices = authored.GetIndices();
            for ( std::size_t i = 0; i < withCmds.size(); ++i )
            {
                const auto&       cmd = withCmds[i];
                const std::size_t end = std::min<std::size_t>( indices.size(), cmd.IndexOffset + cmd.IndexCount );
                bool              touches = false;
                for ( std::size_t k = cmd.IndexOffset; k < end && !touches; ++k )
                    touches = indices[k] >= firstVert;
                if ( !touches )
                    continue;
                cost.FirstBatch = static_cast<std::uint32_t>( i );
                cost.Texture    = cmd.Texture;
                cost.Break      = i == 0 ? BatchBreak::First : ClassifyBatchBreak( withCmds[i - 1], withCmds[i] );
                break;
            }
        }
        return cost;
    }
} // namespace Desert::UI
