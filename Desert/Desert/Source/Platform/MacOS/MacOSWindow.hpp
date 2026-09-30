#pragma once

#include <Engine/Core/Glfw.hpp>

#include <Engine/Core/Window.hpp>

#include <Common/Core/Events/WindowEvents.hpp>

namespace Desert::Platform::MacOS
{
    class MacOSWindow : public Desert::Window
    {
    public:
        MacOSWindow( const WindowSpecification& specification );
        virtual ~MacOSWindow();

        virtual Common::ResultStr<bool> Init() override;

        virtual void ProcessEvents() override;

        [[nodiscard]] virtual const std::string& GetTitle() const override
        {
            return m_Data.Specification.Title;
        }
        virtual void SetTitle( const std::string& title ) override;

        virtual void SetWindowSize( uint32_t width, uint32_t height ) override;
        virtual void SetWindowPos( int x, int y ) override;
        virtual void GetWindowPos( int& x, int& y ) const override;

        virtual void Maximize() override;
        virtual void Restore() override;
        virtual void Minimize() override;
        void         Show() override;

        [[nodiscard]] virtual bool IsWindowMaximized() const override;
        [[nodiscard]] virtual bool IsDecorated() const override
        {
            return m_Data.Specification.Decorated;
        }

        [[nodiscard]] bool HasDrawableArea() const override;

        [[nodiscard]] virtual uint32_t GetWidth() const override;
        [[nodiscard]] virtual uint32_t GetHeight() const override;
        // Mirrors WindowsWindow: the swapchain picks its present mode at creation, so the new pacing only
        // applies once it is rebuilt. MoltenVK exposes FIFO + IMMEDIATE, so switching VSync off works here
        // too — storing the flag alone (which is all this used to do) left the setting inert.
        virtual void SetVSync( bool enabled ) override
        {
            m_Data.Specification.VSync = enabled;
            if ( m_SwapChain )
            {
                m_SwapChain->SetVSync( enabled );
                m_SwapChain->RequestRebuild( m_Data.Specification.Width, m_Data.Specification.Height );
            }
        }
        [[nodiscard]] virtual const void* GetNativeWindow() const override;

        [[nodiscard]] virtual Common::BoolResultStr PrepareNextFrame() const override;
        [[nodiscard]] virtual Common::BoolResultStr PresentFinalImage() const override;

        virtual std::shared_ptr<Graphic::SwapChain> GetWindowSwapChain() override
        {
            return m_SwapChain;
        }

        void SetEventTree( Common::EventTree& events ) override
        {
            m_Data.Events = &events;
        }

        [[nodiscard]] Common::EventTree* GetEventTree() const override
        {
            return m_Data.Events;
        }

        virtual Common::ResultStr<bool> SetupSwapChain() override;

    private:
        /// Refill the cached Width/Height from the OS after a call that resized the window without going
        /// through the resize callback yet. See the definition for why one frame of staleness matters.
        void RefreshCachedSize();

    private:
        struct WindowData
        {
            WindowSpecification Specification;
            Common::EventTree*  Events = nullptr;
        } m_Data;

        GLFWwindow*                         m_GLFWWindow;
        // A window created hidden is maximized when it is SHOWN, never before: see Show().
        bool                                m_MaximizeOnShow = false;
        std::shared_ptr<Graphic::SwapChain> m_SwapChain;
    };
} // namespace Desert::Platform::MacOS
