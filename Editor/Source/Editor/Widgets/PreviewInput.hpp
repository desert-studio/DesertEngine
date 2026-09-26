#pragma once

#include <cstdint>

#include <glm/vec2.hpp>

namespace Desert::Editor
{
    // WHAT A PREVIEW DOES WITH THE MOUSE, decided apart from the widget that reads the mouse.
    //
    // Two kinds of preview live in the editor and they want opposite gestures. An asset's own window (the
    // material editor, a viewer) is where you LOOK at the asset, so dragging orbits and double-click
    // re-frames. A Details row is a summary of a field: it is small, it sits in a scrolling panel where a
    // stray drag should scroll rather than spin the ball, and its double-click means what double-click
    // means on every other asset field in Details — open the asset (UE's SPropertyEditorAsset gesture,
    // Editor/Widgets/AssetFieldOpen.hpp). So a Static preview keeps one angle and turns double-click into
    // "open"; an Interactive one keeps the full camera.
    //
    // A pure function because the decision is the whole feature and ImGui cannot be driven from a test:
    // PreviewViewport::Draw gathers the events, hands them here, and applies what comes back.
    enum class PreviewInteraction : uint8_t
    {
        Interactive,
        Static,
    };

    // One frame of mouse and keyboard over the preview, as ImGui reported it.
    struct PreviewInputEvents
    {
        bool      Hovered       = false;
        bool      Active        = false; // the preview's button is held (LMB or RMB pressed on it)
        bool      RightDown     = false; // RMB held — a held drag with it pans instead of orbiting
        bool      LightKeyDown  = false; // L held — a held drag moves the sun instead of the camera
        bool      DoubleClicked = false; // LMB double-click this frame
        bool      Dome          = false; // Fill::SkyDome: the observer stands still, so no zoom
        glm::vec2 MouseDelta{ 0.0f };    // pixels this frame
        float     Wheel = 0.0f;          // wheel notches this frame
    };

    // What the widget must do. Deltas are in PIXELS; the widget owns the rates, because they depend on its
    // own camera (the pan rate scales with the orbit distance).
    struct PreviewInputResult
    {
        glm::vec2 OrbitDelta{ 0.0f };
        glm::vec2 PanDelta{ 0.0f };
        glm::vec2 SunDelta{ 0.0f };
        float     Wheel       = 0.0f;
        bool      Reframe     = false; // Interactive double-click: frame the content's bounds again
        bool      Open        = false; // Static double-click: open the previewed asset in its editor
        bool      Interacting = false; // the user is manipulating the camera or the light this frame
    };

    [[nodiscard]] inline PreviewInputResult PreviewInput( PreviewInteraction        mode,
                                                          const PreviewInputEvents& events )
    {
        PreviewInputResult result;

        if ( mode == PreviewInteraction::Static )
        {
            // Nothing but the open gesture: no orbit, pan, zoom or sun, so the picture Details shows is the
            // same angle every time the row is drawn, whatever the mouse did over it on the way past.
            result.Open = events.Hovered && events.DoubleClicked;
            return result;
        }

        // LMB-drag orbits, RMB-drag pans — the main viewport's split, so the muscle memory carries over —
        // and holding L turns either drag into moving the sun.
        if ( events.Active )
        {
            if ( events.MouseDelta.x != 0.0f || events.MouseDelta.y != 0.0f )
            {
                if ( events.LightKeyDown )
                    result.SunDelta = events.MouseDelta;
                else if ( events.RightDown )
                    result.PanDelta = events.MouseDelta;
                else
                    result.OrbitDelta = events.MouseDelta;
            }
            result.Interacting = true;
        }

        if ( events.Hovered )
        {
            // NO ZOOM IN THE DOME. There is nothing to approach — the subject is the sky — and the zoom's
            // clamp is expressed in a framed radius the dome does not have.
            if ( !events.Dome && events.Wheel != 0.0f )
            {
                result.Wheel       = events.Wheel;
                result.Interacting = true;
            }
            if ( events.DoubleClicked )
            {
                result.Reframe     = true;
                result.Interacting = true;
            }
        }

        return result;
    }

    // WHO THE WHEEL BELONGS TO over a preview. An Interactive preview (the Material Editor, the asset viewers)
    // zooms with it and must CLAIM it, or ImGui also scrolls the window the preview sits in: the model zooms
    // while the whole panel slides away under the cursor. A Static preview (a Details row) never zooms, so the
    // wheel passes through and keeps scrolling the Details panel exactly as it did.
    enum class PreviewWheelOwner : uint8_t
    {
        PassThrough, // the parent window scrolls; the preview ignores the wheel
        Zoom,        // the preview zooms and the caller claims the wheel from the parent (SetItemKeyOwner)
    };

    [[nodiscard]] inline PreviewWheelOwner WheelOwner( const PreviewInteraction mode, const bool hovered,
                                                       const float wheel ) noexcept
    {
        if ( mode != PreviewInteraction::Interactive || !hovered )
            return PreviewWheelOwner::PassThrough;
        // Claimed while hovered even on a frame with no wheel: ImGui routes the wheel by key ownership, and an
        // owner taken only on the frame the wheel moves is taken after the parent has already scrolled.
        static_cast<void>( wheel );
        return PreviewWheelOwner::Zoom;
    }
} // namespace Desert::Editor
