#pragma once

// THE TWO LOADING PICTURES, ON THE ENGINE'S OWN 2D BATCHER (Render2D) — no pass of their own: each host draws
// them into the draw list it already fills for the game's UI (the editor's EditorUIPass, the runtime's present
// block), so the overlay is composited exactly where a UI canvas is.
//
//   DrawLoadingStrip        — a track and a block sliding along it, driven by a FRAME count rather than a clock
//                             so an unattended capture of frame N is reproducible. The runtime's level loading
//                             screen draws it under its opaque cover.
//   DrawStreamingWaitOverlay — what the player sees while world streaming blocks play (WP12, decision O2: the
//                             cell under their feet is not resident yet): the frame dimmed, "Loading…" in the
//                             project's default UI font, and the strip.

#include <Engine/Graphic/Render2D/DrawList2D.hpp>

#include <cstdint>

namespace Desert::UI
{
    void DrawLoadingStrip( Graphic::Render2D::DrawList2D& dl, float width, float height, std::uint32_t frame );

    void DrawStreamingWaitOverlay( Graphic::Render2D::DrawList2D& dl, float width, float height,
                                   std::uint32_t frame );
} // namespace Desert::UI
