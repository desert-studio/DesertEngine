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
    void DrawImageWidget( ElementFrame& frame )
    {
        auto& ctx   = frame.Ctx;
        auto& tree  = frame.Tree;
        auto& e     = frame.E;
        auto& scale = frame.Scale;
        auto& dl    = frame.Dl;
        auto& st    = frame.St;
        auto& mn    = frame.Mn;
        auto& mx    = frame.Mx;

        // A sprite block — reuses DrawBox so it gets GIF playback, 9-slice and the static path.
        // With no sprite bound it draws nothing (an empty Image is invisible, not a solid box).
        const auto& im = *tree.Get<UIImageData>( e );
        if ( HandleSet( im.Sprite ) )
            DrawBox( ctx.View.Resources(), dl, mn, mx,
                     Tinted( ctx, glm::vec4( st.Color( StyleSlot::ImageTint, im.Tint ), im.Opacity ) ), im.Sprite,
                     im.SpriteBorder, scale, 0.0f );
    }

    void DrawRenderTextureWidget( ElementFrame& frame )
    {
        auto& ctx  = frame.Ctx;
        auto& tree = frame.Tree;
        auto& e    = frame.E;
        auto& dl   = frame.Dl;
        auto& rect = frame.ElementRect;
        auto& mn   = frame.Mn;
        auto& mx   = frame.Mx;

        // A LIVE WORLD IN THE RECT (Ю16). The backend rendered it offscreen before this frame's
        // render pass opened — a nested one is illegal, which is why the producer runs in the
        // host's pre-update and this site only samples what it left.
        //
        // ASKING IS ALSO THE DEMAND. Reaching this line is how the backend learns the element
        // is on screen; an element the walk skipped — not Visible, scrolled out of a clipped
        // list, on a screen that is not current — is never asked about, and the backend
        // destroys its capture and gives the renderer slot back. That is the whole answer to
        // "what happens on the seventh one", and it is an answer no flag could have given:
        // a slot comes back by DESTRUCTION and by nothing else.
        const auto& rt = *tree.Get<UIRenderTextureData>( e );

        // NOT THEMED, and that is a decision. Every other element here resolves its colour
        // through a StyleSlot, but a theme's Image.Tint belongs to UIImageComponent — the
        // Details panel shows the Image slots only for an element that HAS one, and
        // Desert/Tests/Engine/UIStyle pins that pairing. Borrowing the slot would give this
        // element a themed value the author cannot see or edit. Tint here is a straight
        // multiply on a live picture, so it is the element's own field and nothing else's.
        const glm::vec4 tint = Tinted( ctx, glm::vec4( rt.Tint, rt.Opacity ) );

        if ( rect.W > 0.0f && rect.H > 0.0f )
        {
            if ( const void* world = ResolveRenderTexture( ctx, e, rt, rect ); world != nullptr )
            {
                dl.AddImage( world, mn, mx, { 0.0f, 0.0f }, { 1.0f, 1.0f }, tint );
            }
            else
            {
                // MAGENTA, NOT NOTHING, and it is the same rule IUIMaterialSource states: an
                // element that renders nothing is indistinguishable from an element that was
                // meant to render nothing. The reason is already in the log with its numbers
                // — who refused knows why, this site only knows that somebody did.
                dl.AddRectFilled( mn, mx, Tinted( ctx, glm::vec4( 1.0f, 0.0f, 1.0f, 1.0f ) ), 0.0f );
            }
        }
    }
} // namespace Desert::UI::Walk
