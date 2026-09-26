#pragma once

// THE SPLASH BEHAVES AS A WINDOW: it can be minimized, closed and moved. UE's editor splash is borderless
// with a close button of its own; ours has the two buttons the editor's own frame has (minimize and close,
// the same squares, colours and glyphs — Editor/Widgets/WindowButtonStyle.hpp) and moves by its
// background, as the editor's title bar does.
//
// Everything here is arithmetic and state with no window behind it, so it is tested without one
// (Desert/Tests/Editor/SplashLayout): where the buttons are, which one a point is over, when a press
// counts as a click, and what the start does once a close has been asked for.

#include <Editor/Splash/SplashLayout.hpp>
#include <Editor/Widgets/WindowButtonStyle.hpp>

#include <cstddef>

namespace Desert::Editor::Splash
{
    // The buttons' strip at the top of the splash, in design points. The editor's bar is one ImGui frame
    // high; the splash is a picture a person looks at from further back, so its strip is a little taller.
    inline constexpr float kButtonHeight    = 32.0f;
    inline constexpr float kButtonGlyphSize = 16.0f;

    enum class SplashButton
    {
        None,
        Minimize,
        Close,
    };

    // Bottom-origin, like every other rect of the layout. Close sits in the corner, minimize to its left,
    // touching: the order and the spacing of the editor's own frame.
    [[nodiscard]] constexpr Rect ButtonRect( const SplashButton button )
    {
        const float width = UI::kWindowButtonWidth;
        switch ( button )
        {
            case SplashButton::Close:
                return { kWidth - width, kHeight - kButtonHeight, width, kButtonHeight };
            case SplashButton::Minimize:
                return { kWidth - 2.0f * width, kHeight - kButtonHeight, width, kButtonHeight };
            case SplashButton::None:
                break;
        }
        return {};
    }

    [[nodiscard]] constexpr bool Contains( const Rect& rect, const float x, const float y )
    {
        return x >= rect.X && x < rect.X + rect.W && y >= rect.Y && y < rect.Y + rect.H;
    }

    /// Which button the design point (@p x, @p y — bottom-origin) is over.
    [[nodiscard]] constexpr SplashButton HitTestButtons( const float x, const float y )
    {
        if ( Contains( ButtonRect( SplashButton::Close ), x, y ) )
            return SplashButton::Close;
        if ( Contains( ButtonRect( SplashButton::Minimize ), x, y ) )
            return SplashButton::Minimize;
        return SplashButton::None;
    }

    /// A point on the screen in design points. @p windowLeft / @p windowTop are the window's top-left
    /// corner and @p pointX / @p pointY the cursor, all in the SAME top-origin screen space (points, not
    /// pixels); @p fit is the FitScale the window was made with.
    struct DesignPoint
    {
        float X = 0.0f;
        float Y = 0.0f; // bottom-origin
    };
    [[nodiscard]] constexpr DesignPoint ScreenToDesign( const float pointX, const float pointY,
                                                        const float windowLeft, const float windowTop,
                                                        const float fit )
    {
        const float scale = fit > 0.0f ? fit : 1.0f;
        return { ( pointX - windowLeft ) / scale, kHeight - ( pointY - windowTop ) / scale };
    }

    /// WHEN A PRESS IS A CLICK: what every platform button does — the press has to start on the button
    /// and the release has to land on the SAME button. Dragging off before letting go cancels, which is
    /// the one way a person has to change their mind after pressing close. Fed the pointer's button and
    /// the button under it on every observation; returns the button that was clicked, if any.
    class ButtonTracker
    {
    public:
        SplashButton Update( const SplashButton under, const bool down )
        {
            SplashButton clicked = SplashButton::None;
            if ( down && !m_WasDown )
                m_Pressed = under;
            else if ( !down && m_WasDown )
            {
                if ( m_Pressed != SplashButton::None && m_Pressed == under )
                    clicked = m_Pressed;
                m_Pressed = SplashButton::None;
            }
            m_WasDown = down;
            m_Under   = under;
            return clicked;
        }

        [[nodiscard]] SplashButton Hovered() const
        {
            return m_Under;
        }
        // Held down on this button right now (the pressed colour), not merely pressed once.
        [[nodiscard]] SplashButton Pressed() const
        {
            return m_WasDown && m_Pressed == m_Under ? m_Pressed : SplashButton::None;
        }

    private:
        SplashButton m_Pressed = SplashButton::None;
        SplashButton m_Under   = SplashButton::None;
        bool         m_WasDown = false;
    };

    /// The colour a button's square is drawn with now: clear at rest, the shared hover/pressed colours
    /// otherwise.
    [[nodiscard]] inline UI::ButtonColour ButtonFill( const SplashButton button, const ButtonTracker& tracker )
    {
        const bool close = button == SplashButton::Close;
        if ( tracker.Pressed() == button )
            return close ? UI::kCloseButtonPressed : UI::kWindowButtonPressed;
        if ( tracker.Hovered() == button )
            return close ? UI::kCloseButtonHovered : UI::kWindowButtonHovered;
        return {};
    }

    /// WHAT THE START DOES NEXT. The staged load runs one stage per frame (EditorLayer::OnUpdate); a close
    /// asked for on the splash is honoured BEFORE the next stage starts, so the stages after it never run
    /// and the editor leaves through its ordinary close.
    enum class StartupStep
    {
        RunStage,
        Quit,
        Done,
    };
    [[nodiscard]] constexpr StartupStep NextStartupStep( const bool closeRequested, const std::size_t next,
                                                         const std::size_t count )
    {
        if ( closeRequested )
            return StartupStep::Quit;
        return next < count ? StartupStep::RunStage : StartupStep::Done;
    }
} // namespace Desert::Editor::Splash
