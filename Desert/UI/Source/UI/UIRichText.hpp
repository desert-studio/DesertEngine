#pragma once

// RICH TEXT — the BBCode parse ([b] [i] [color=#hex] [size=] [link=] [icon=] [img=]), the line layout (wrap,
// auto-size, alignment, overflow) and the SDF glyph emission of a UIText block. Split out of
// UICanvasRenderer2D.cpp (UI-R2D-SPLIT).

#include <UI/UIWalkCtx.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Desert::UI::Walk
{
    // The [link=msg] runs of one drawn block — what turns a click on a link into its message (UE's
    // hyperlink decorator raising its delegate). In: the pointer in the element's space, for the hover
    // highlight (unset = no hover). Out: every link run's box in that same space and its message.
    struct RichTextLinks
    {
        struct Hit
        {
            Rect        Box;
            std::string Message;
        };
        std::optional<glm::vec2> Pointer;
        std::vector<Hit>         Hits;
    };

    void  DrawText2D( IUICanvasResources& res, Graphic::Render2D::DrawList2D& dl, const UITextData& t,
                      const Rect& rect, float scale, const glm::vec4& tint, double viewSeconds,
                      RichTextLinks* links = nullptr );
    float MeasureTextPx( IUICanvasResources& res, const std::string& text, float fontSizePx );
} // namespace Desert::UI::Walk
