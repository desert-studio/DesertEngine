#include <Engine/UI/Widgets/UIWidgets.hpp>

#include <Engine/UI/UICanvasRenderer2D.hpp>
#include <Engine/UI/UIOverlay.hpp>
#include <Engine/Assets/Common.hpp>
#include <Engine/Text/BakedFont.hpp>
#include <Engine/UI/UIStyleResolver.hpp>
#include <Engine/Text/Utf8.hpp>
#include <Engine/UI/UICanvasLayout.hpp>
#include <Engine/UI/UIDataStore.hpp>
#include <Engine/UI/UIPathGeometry.hpp>
#include <Engine/UI/UIRichText.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <span>
#include <unordered_set>
#include <array>
#include <optional>
#include <cstdint>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

namespace Desert::UI::Walk
{
    void DrawTextWidget( ElementFrame& frame )
    {
        auto& ctx     = frame.Ctx;
        auto& tree    = frame.Tree;
        auto& e       = frame.E;
        auto& scale   = frame.Scale;
        auto& dl      = frame.Dl;
        auto& st      = frame.St;
        auto& rect    = frame.ElementRect;
        auto& binding = frame.Binding;

        // A bound label draws the store's string, and a keyed one draws its translation, both
        // without the component ever being touched — the authored text is never written back.
        //
        // ONE DRAW, AND IT USED TO BE TWO. The Ю15 merge (bb89ba87) resolved a conflict with
        // Ю13's theming by KEEPING BOTH SIDES: a themed draw that read `binding.Text`, and
        // directly under it an UNTHEMED draw that read ResolveLabel. Every label in the engine
        // was therefore emitted twice — double the glyph geometry in every text batch, SDF
        // edges composited over themselves, and the unthemed copy painted LAST, which is what
        // made Ю13's theming of text silently do nothing. The two halves compose rather than
        // compete: theme first (it decides colour, size and font), then resolve the string
        // (ResolveLabel already subsumes `binding.Text` — see its own comment).
        UITextData text = Themed( st, *tree.Get<UITextData>( e ) );
        text.Text       = ResolveLabel( ctx.View.Resources().Text(), text.Text, binding );
        DrawText2D( ctx.View.Resources(), dl, text, rect, scale, ctx.View.Tint, ctx.View.Time );
    }

    void DrawIconWidget( ElementFrame& frame )
    {
        auto& ctx  = frame.Ctx;
        auto& tree = frame.Tree;
        auto& e    = frame.E;
        auto& dl   = frame.Dl;
        auto& st   = frame.St;
        auto& rect = frame.ElementRect;

        UIIconData icon = *tree.Get<UIIconData>( e );
        icon.Color      = st.Color( StyleSlot::IconColor, icon.Color );
        DrawIcon( ctx.View.Resources(), dl, icon, rect, ctx.View.Tint );
    }
} // namespace Desert::UI::Walk
