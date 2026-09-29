#pragma once

#include <functional>
#include <utility>

namespace Desert::Core
{
    /**
     * @brief Who decides what the OS frame's close button does.
     *
     * Without an owner the close stops the application at once, which is right for the runtime and every
     * tool. The editor installs itself as the owner because a close from the frame must ask the same
     * Save / Don't Save / Cancel question File -> Exit asks: before this gate the frame's x ended the run
     * directly and every unsaved document was lost without a word, while the menu entry beside it asked.
     *
     * The owner answers whether the application may stop NOW. An owner that raised questions answers
     * false and ends the run itself (Application::Close) once the last one is answered; Cancel simply
     * never calls it. Pure, so the suite drives it without a window.
     */
    class WindowCloseGate
    {
    public:
        using Owner = std::function<bool()>;

        void Install( Owner owner )
        {
            m_Owner = std::move( owner );
        }

        void Uninstall()
        {
            m_Owner = nullptr;
        }

        [[nodiscard]] bool HasOwner() const noexcept
        {
            return static_cast<bool>( m_Owner );
        }

        /// The frame's close arrived. True: stop the application now. False: the owner took the close
        /// over, and the window must be told to stay (the platform's should-close flag cleared).
        [[nodiscard]] bool StopsNow() const
        {
            return !m_Owner || m_Owner();
        }

    private:
        Owner m_Owner;
    };
} // namespace Desert::Core
