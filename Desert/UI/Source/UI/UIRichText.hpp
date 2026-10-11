#pragma once

// RICH TEXT — the BBCode parse ([b] [i] [color=#hex] [size=] ...), the line layout (wrap, auto-size, alignment,
// overflow) and the SDF glyph emission of a UIText block. Split out of UICanvasRenderer2D.cpp (UI-R2D-SPLIT).

#include <UI/UIWalkCtx.hpp>

#include <string>

namespace Desert::UI::Walk
{
    void  DrawText2D( IUICanvasResources& res, Graphic::Render2D::DrawList2D& dl, const UITextData& t,
                      const Rect& rect, float scale, const glm::vec4& tint, double viewSeconds );
    float MeasureTextPx( IUICanvasResources& res, const std::string& text, float fontSizePx );
} // namespace Desert::UI::Walk
