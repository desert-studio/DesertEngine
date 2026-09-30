#pragma once

#include <cstdint>

#include <glm/vec2.hpp>

namespace Desert::Editor
{
    // WHAT A PREVIEW DOES WITH THE MOUSE, decided apart from the widget that reads the mouse.
    //
    // A preview is an asset's own window (the material editor, a viewer): where you LOOK at the asset, so
    // dragging orbits and double-click re-frames. Details draws no live preview (THM-FIXF: its rows are
    // ThumbnailService pictures, and a row's double-click "open" is AssetFieldOpen's), so there is no
    // one-angle mode here (THM-FIXG removed `Static` with its last caller).
    //
    // A pure function because the decision is the whole feature and ImGui cannot be driven from a test:
    // PreviewViewport::Draw gathers the events, hands them here, and applies what comes back.
    //
    // Yielded is the third state, and it is per FRAME, not per preview: a tool drawn over the picture (the
    // Animation Editor's bone gizmo) has the pointer, so the preview submits no item that could take the
    // hover or the press and moves no camera. The gizmo wins over navigation, as in UE's viewport client.
    enum class PreviewInteraction : uint8_t
    {
        Interactive,
        Yielded,
    };

    /// A tool over an Interactive preview: it yields while the pointer is on the tool or the tool is held,
    /// and never in the middle of a camera drag that merely crosses the tool (`anotherItemActive` is the
    /// preview's own button, held since the press). Decided in the SAME frame as the press: a hover read
    /// one frame late would let the preview's button claim the press first, and the gizmo would never start.
    [[nodiscard]] constexpr PreviewInteraction PreviewInteractionUnderTool( const bool toolUnderPointer,
                                                                            const bool toolHeld,
                                                                            const bool anotherItemActive ) noexcept
    {
        if ( toolHeld || ( toolUnderPointer && !anotherItemActive ) )
            return PreviewInteraction::Yielded;
        return PreviewInteraction::Interactive;
    }

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
        bool      Interacting = false; // the user is manipulating the camera or the light this frame
    };

    [[nodiscard]] inline PreviewInputResult PreviewInput( PreviewInteraction        mode,
                                                          const PreviewInputEvents& events )
    {
        PreviewInputResult result;

        if ( mode == PreviewInteraction::Yielded )
            return result;

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

    // WHO THE WHEEL BELONGS TO over a preview: the preview, exactly when the wheel zooms it. An Interactive
    // preview (the Material Editor, the asset viewers) zooms with it and must CLAIM it, or ImGui also scrolls
    // the window the preview sits in: the model zooms while the whole panel slides away under the cursor. A
    // pane with nothing to zoom (empty, or the sky dome fill) never zooms, so there the wheel passes through and keeps scrolling the panel as over any other row.
    enum class PreviewWheelOwner : uint8_t
    {
        PassThrough, // the parent window scrolls; the preview ignores the wheel
        Zoom,        // the preview zooms and the caller claims the wheel from the parent (SetItemUsingMouseWheel)
    };

    // `zoomable`: the pane has content and it is not the sky dome fill (the dome orbits but never dollies).
    // Claimed on every hovered frame, not only on a frame the wheel moves: this ImGui (1.89 WIP) reads
    // SetItemUsingMouseWheel in the NEXT frame's wheel routing, so a claim taken on the notch's own frame
    // arrives after the parent has already scrolled.
    [[nodiscard]] constexpr PreviewWheelOwner WheelOwner( const PreviewInteraction mode, const bool zoomable,
                                                          const bool hovered ) noexcept
    {
        if ( mode != PreviewInteraction::Interactive || !zoomable || !hovered )
            return PreviewWheelOwner::PassThrough;
        return PreviewWheelOwner::Zoom;
    }
} // namespace Desert::Editor
