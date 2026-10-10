#include "UIIntrospection.hpp"

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

    Common::BoolResultStr CaptureFrame( const IUITree& tree, const std::vector<UICanvasWalk>& canvases,
                                        const Graphic::Render2D::DrawList2D& dl, const Rect& viewportPx,
                                        UIFrameProbe& out )
    {
        out.Reset();
        out.ViewportPx = viewportPx;
        for ( const UICanvasWalk& walk : canvases )
            out.Canvases.push_back( walk.Canvas );

        for ( const UICanvasWalk& walk : canvases )
        {
            // EACH CANVAS IS ENUMERATED WITH ITS OWN CELL of this view, never with a neighbour's: the two
            // skip causes EnumerateCanvas needs from a context — which screen is current, what a binding
            // says — are per (canvas x view), so handing it the wrong cell would mark this canvas's screens
            // "not current" because a DIFFERENT canvas is on a different screen.
            //
            // A canvas this view has not drawn has no cell, and that is not the same as a canvas sitting on
            // its first screen: nullptr is EnumerateCanvas's own authoring mode, which honours neither
            // reason, and it is the correct answer for a probe of a canvas the view never walked.
            // The cell is handed in with the canvas (UICanvasWalk) because the view is keyed by the host's
            // own ids; the ECS overload looks it up with UIViewContext::FindCanvasState.
            const NodeId           canvas = walk.Canvas;
            const UICanvasContext* cell   = walk.Cell;

            const std::size_t before = out.Elements.size();
            if ( const auto walked = EnumerateCanvas( tree, canvas, viewportPx, out.Elements, cell ); !walked )
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

} // namespace Desert::UI
