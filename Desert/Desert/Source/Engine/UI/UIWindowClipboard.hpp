#pragma once

#include <Engine/Core/Window.hpp>
#include <Engine/UI/UITextEditState.hpp>

namespace Desert::UI
{
    // The host's window as the UI's clipboard (UIInput::Clipboard). One adapter for both hosts, so the
    // runtime and the editor preview cannot disagree on what Ctrl+V reads.
    class WindowClipboard final : public IUIClipboard
    {
    public:
        explicit WindowClipboard( Window& window ) : m_Window( window )
        {
        }
        [[nodiscard]] std::string GetText() const override
        {
            return m_Window.GetClipboardText();
        }
        void SetText( const std::string& text ) override
        {
            m_Window.SetClipboardText( text );
        }

    private:
        Window& m_Window;
    };
} // namespace Desert::UI
