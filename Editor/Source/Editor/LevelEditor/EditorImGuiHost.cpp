#include "Editor/LevelEditor/EditorImGuiHost.hpp"

#include <Common/Core/Core.hpp>
#include <Common/Core/Logger.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Glfw.hpp>
#include "Editor/Core/EditorResources.hpp"
#include "Editor/Core/ThemeManager.hpp"
#include "Editor/ImGuiIntegration/ImGuiLayer.hpp"
#include "Editor/Widgets/WindowButtonStyle.hpp"

#include <ImGui/imgui.h>
#include <ImGuizmo.h>

#include <utility>

namespace Desert::Editor
{
    Common::BoolResultStr EditorImGuiHost::Attach( Engine::Application& application,
                                                   std::function<void()> requestExit )
    {
        m_Application = &application;

        // THE WINDOW FRAME, IF THIS EDITOR OWNS IT. Asked of the window rather than assumed from the
        // ApplicationInfo that requested it: a fullscreen-over-the-taskbar window is frameless whatever was
        // asked for, and the window is the one that knows what actually happened (Window::IsDecorated).
        if ( const auto& window = application.GetWindow(); window && !window->IsDecorated() )
        {
            // The same ordered close the control channel's `quit` takes: Run() leaves its loop, every layer is
            // detached, the device goes idle. Two ways to end a session would drift.
            m_WindowChrome.emplace( *window, requestExit );
        }

        // THE OS FRAME'S CLOSE ASKS WHAT File -> Exit ASKS. The application no longer stops on the event
        // itself: the exit either closes at once (nothing dirty) or raises the Save / Don't Save / Cancel
        // questions and closes after the last one; Cancel leaves the editor running, so the platform's
        // should-close flag is cleared here rather than left set behind a live window.
        application.GetCloseGate().Install(
             [app = &application, exit = std::move( requestExit )]()
             {
                 exit();
                 if ( const auto& window = app->GetWindow() )
                 {
                     // GLFW takes back the handle Window hands out as const void*.
                     // NOLINTNEXTLINE(bugprone-casting-through-void,cppcoreguidelines-pro-type-const-cast)
                     auto* native = static_cast<GLFWwindow*>( const_cast<void*>( window->GetNativeWindow() ) );
                     glfwSetWindowShouldClose( native, GLFW_FALSE );
                 }
                 return false;
             } );

        // The context first, then the editor fonts into its atlas, then the backend (which uploads them).
        ::ImGui::CreateContext();
        EditorResources::Initialize( UI::IconFontFile().string() );
        m_ImGuiLayer = ImGui::ImGuiLayer::Create();
        if ( const auto attached = m_ImGuiLayer->OnAttach(); !attached.IsSuccess() )
            return Common::MakeFormattedError( "ImGui layer failed to attach: {}", attached.GetError() );

        ImGuiIO& io = ::ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // multi-viewport / platform windows

        ThemeManager::SetDarkTheme();

        // With viewports enabled, platform windows look identical to regular ones only without rounding and
        // with an opaque background.
        ImGuiStyle& style = ::ImGui::GetStyle();
        if ( io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable )
        {
            style.WindowRounding              = 0.0f;
            style.Colors[ImGuiCol_WindowBg].w = 1.0f;
        }
        return BOOLSUCCESS;
    }

    void EditorImGuiHost::BeginFrame()
    {
        m_ImGuiLayer->Begin();
        ImGuizmo::BeginFrame();
    }

    void EditorImGuiHost::DrawResizeBorders()
    {
        if ( m_WindowChrome )
            m_WindowChrome->DrawResizeBorders();
    }

    void EditorImGuiHost::EndFrame()
    {
        m_ImGuiLayer->End();
    }

    void EditorImGuiHost::UninstallCloseGate()
    {
        m_Application->GetCloseGate().Uninstall();
    }

    void EditorImGuiHost::Detach()
    {
        if ( const auto detached = m_ImGuiLayer->OnDetach(); !detached.IsSuccess() )
            LOG_ERROR( "[EditorImGuiHost] ImGui layer failed to detach: {}", detached.GetError() );
        m_ImGuiLayer.reset();
    }
} // namespace Desert::Editor
