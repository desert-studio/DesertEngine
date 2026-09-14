#include "UIIntrospection.hpp"

#include <Engine/UI/UICanvasRenderer2D.hpp>

#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace Desert::UI
{
    const char* BatchBreakName( BatchBreak reason )
    {
        switch ( reason )
        {
            case BatchBreak::None:
                return "merged";
            case BatchBreak::First:
                return "first batch";
            case BatchBreak::GlassPrev:
                return "after glass";
            case BatchBreak::GlassSelf:
                return "glass";
            case BatchBreak::Text:
                return "text/solid";
            case BatchBreak::Texture:
                return "texture";
            case BatchBreak::Material:
                return "material";
            case BatchBreak::ClipRect:
                return "clip rect";
        }
        return "?";
    }

    BatchBreak ClassifyBatchBreak( const Graphic::Render2D::DrawCommand& prev,
                                   const Graphic::Render2D::DrawCommand& cur )
    {
        // The same questions, in the same order, as DrawList2D::CurrentCommand. Glass first because a
        // glass rect is opened by hand and never extended in either direction, so it answers before the
        // state comparison is even reached.
        if ( cur.Glass )
            return BatchBreak::GlassSelf;
        if ( prev.Glass )
            return BatchBreak::GlassPrev;
        if ( prev.Text != cur.Text )
            return BatchBreak::Text;
        if ( prev.Texture != cur.Texture )
            return BatchBreak::Texture;
        if ( prev.Material != cur.Material )
            return BatchBreak::Material;
        if ( prev.ClipRect != cur.ClipRect )
            return BatchBreak::ClipRect;
        return BatchBreak::None;
    }

    void UIFrameProbe::Reset()
    {
        Valid = false;
        Refusal.clear();
        Stats = UIFrameStats2D{};
        Walk  = UIWalkStats{};
        Batches.clear();
        Elements.clear();
        ViewportPx = Rect{};
        Canvases.clear();
    }

    namespace
    {
        // Which of the three pipelines Render2D::Flush binds for a command. Glass wins over Text because
        // that is the order Flush tests them in.
        // WHICH PIPELINE a command binds, as an identity rather than as a small integer. It used to be
        // 0/1/2 for UI2D/UIText/UIGlass, which was exact while those were the only three; a UI material
        // brings a pipeline of ITS OWN, one per material, so two adjacent material batches are two
        // pipeline binds and a fixed enumeration cannot say so. The three built-ins keep distinct
        // addresses of their own so the comparison stays one comparison.
        const void* PipelineOf( const Graphic::Render2D::DrawCommand& cmd )
        {
            static const char kSolid = 0, kText = 0, kGlass = 0;
            if ( cmd.Glass )
                return &kGlass;
            if ( cmd.Material )
                return cmd.Material;
            return cmd.Text ? static_cast<const void*>( &kText ) : static_cast<const void*>( &kSolid );
        }
    } // namespace

    void CaptureDrawList( const Graphic::Render2D::DrawList2D& dl, UIFrameProbe& out )
    {
        const auto& commands = dl.GetCommands();

        out.Stats.Vertices  = static_cast<std::uint32_t>( dl.GetVertices().size() );
        out.Stats.Indices   = static_cast<std::uint32_t>( dl.GetIndices().size() );
        out.Stats.Triangles = out.Stats.Indices / 3;
        out.Stats.Batches   = static_cast<std::uint32_t>( commands.size() );

        std::unordered_set<const void*> textures;
        std::unordered_set<const void*> materials;
        const void*                     lastPipeline = nullptr;

        out.Batches.reserve( commands.size() );
        for ( std::size_t i = 0; i < commands.size(); ++i )
        {
            const auto& cmd = commands[i];

            UIBatchInfo info;
            info.Index       = static_cast<std::uint32_t>( i );
            info.Break       = i == 0 ? BatchBreak::First : ClassifyBatchBreak( commands[i - 1], cmd );
            info.Texture     = cmd.Texture;
            info.Material    = cmd.Material;
            info.Text        = cmd.Text;
            info.Glass       = cmd.Glass;
            info.ClipRect    = cmd.ClipRect;
            info.IndexCount  = cmd.IndexCount;
            info.IndexOffset = cmd.IndexOffset;
            out.Batches.push_back( info );

            out.Stats.BreakCounts[static_cast<std::size_t>( info.Break )]++;

            // Render2D::Flush skips a command with no indices, so a recorded batch and a submitted draw
            // are not the same thing and the panel says both.
            if ( cmd.IndexCount == 0 )
                out.Stats.EmptyBatches++;
            else
                out.Stats.DrawCalls++;

            out.Stats.LargestBatchTris = std::max( out.Stats.LargestBatchTris, cmd.IndexCount / 3 );

            if ( cmd.Texture != nullptr )
                textures.insert( cmd.Texture );
            if ( cmd.Material != nullptr )
                materials.insert( cmd.Material );

            const void* pipeline = PipelineOf( cmd );
            if ( pipeline != lastPipeline )
            {
                if ( lastPipeline != nullptr )
                    out.Stats.PipelineSwitches++;
                lastPipeline = pipeline;
            }
        }
        out.Stats.UniqueTextures  = static_cast<std::uint32_t>( textures.size() );
        out.Stats.UniqueMaterials = static_cast<std::uint32_t>( materials.size() );
    }

    Common::BoolResultStr CaptureFrame( const UIViewContext& view, entt::registry& reg,
                                        const std::vector<entt::entity>&     canvases,
                                        const Graphic::Render2D::DrawList2D& dl, const Rect& viewportPx,
                                        UIFrameProbe& out )
    {
        out.Reset();
        out.ViewportPx = viewportPx;
        out.Canvases   = canvases;

        for ( const entt::entity canvas : canvases )
        {
            // EACH CANVAS IS ENUMERATED WITH ITS OWN CELL of this view, never with a neighbour's: the two
            // skip causes EnumerateCanvas needs from a context — which screen is current, what a binding
            // says — are per (canvas x view), so handing it the wrong cell would mark this canvas's screens
            // "not current" because a DIFFERENT canvas is on a different screen.
            //
            // A canvas this view has not drawn has no cell, and that is not the same as a canvas sitting on
            // its first screen: nullptr is EnumerateCanvas's own authoring mode, which honours neither
            // reason, and it is the correct answer for a probe of a canvas the view never walked.
            const UICanvasContext* cell = view.FindCanvasState( canvas );

            const std::size_t before = out.Elements.size();
            if ( const auto walked = EnumerateCanvas( reg, canvas, viewportPx, out.Elements, cell ); !walked )
            {
                // A probe that reported zero elements and success would read as "this canvas is free", which
                // is exactly the empty-successful-answer the contract forbids. It refuses by name instead.
                out.Refusal = walked.GetError();
                return Common::MakeError( out.Refusal );
            }

            // EnumerateCanvas fills `out` from empty, so the accumulation is ours to do: a probe of a frame
            // with two canvases must describe both, not the last one.
            for ( std::size_t i = before; i < out.Elements.size(); ++i )
            {
                const UIElementNode& n = out.Elements[i];
                out.Walk.Visited++;
                out.Walk.MaxDepth = std::max( out.Walk.MaxDepth, static_cast<std::uint32_t>( n.Depth ) );
                if ( n.Drawn )
                {
                    out.Walk.Drawn++;
                    if ( n.Clipped )
                        out.Walk.Clipped++;
                }
                else
                {
                    out.Walk.Skipped++;
                    out.Walk.SkipCounts[static_cast<std::size_t>( n.Cause )]++;
                }
            }
        }

        // ONCE for the frame. The draw list holds every canvas's geometry, so reading it per canvas counted
        // the first canvas's batches again for each canvas after it.
        CaptureDrawList( dl, out );
        out.Valid = true;
        out.Captures++;
        return BOOLSUCCESS;
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
            BeginUIFrame( probe, reg );
            const bool drawn = RenderCanvas2D( probe, reg, canvas, dl, viewportPx ).IsSuccess();
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
                ECS::UIVisibility& Field;
                ECS::UIVisibility  Previous;
                ~VisibilityRestore()
                {
                    Field = Previous;
                }
            };
            auto&             field = reg.get<ECS::UILayoutComponent>( element ).Data.Visibility;
            VisibilityRestore restore{ field, field };
            field = ECS::UIVisibility::Hidden;

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
