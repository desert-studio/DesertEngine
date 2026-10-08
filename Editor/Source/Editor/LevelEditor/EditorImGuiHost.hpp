#pragma once

#include <Common/Core/ResultStr.hpp>

#include "Editor/Widgets/WindowChrome.hpp"

#include <functional>
#include <memory>
#include <optional>

namespace Desert::Engine
{
    class Application;
} // namespace Desert::Engine

namespace Desert::ImGui
{
    class ImGuiLayer;
} // namespace Desert::ImGui

namespace Desert::Editor
{
    // THE EDITOR'S WINDOW AND ITS IMMEDIATE-MODE UI (UE: FSlateApplication::Create, called from the level
    // editor's start-up): the frame the OS no longer draws, the close gate that turns the OS close into the
    // editor's ordered exit, the ImGui context and its backend layer, the io flags, the theme. EditorLayer
    // owns one and calls it at the points of OnAttach / OnUIRender / OnDetach where these lines used to be.
    class EditorImGuiHost
    {
    public:
        // Window frame (only when the window is undecorated), close gate, ImGui context, editor fonts,
        // backend layer, io flags, theme. `requestExit` is the one ordered exit: the frame's close button and
        // the OS close both take it. A refusal ends the run.
        [[nodiscard]] Common::BoolResultStr Attach( Engine::Application& application,
                                                    std::function<void()> requestExit );

        // The ImGui frame and ImGuizmo's per-frame state (ImGuizmo is one global — begun ONCE, here, before
        // any panel issues a Manipulate()).
        void BeginFrame();
        // The window edges the OS frame used to give: eight 6px windows of their own, submitted last and
        // outside the dockspace host so they sit above the panels that reach the screen edge. A no-op while
        // maximized or when the OS draws the frame.
        void DrawResizeBorders();
        void EndFrame();

        // The OS close goes back to the platform's own handling. First in OnDetach.
        void UninstallCloseGate();
        // The backend layer leaves; reported, not returned — the rest of OnDetach still has to run.
        void Detach();

        // Empty while the OS draws the frame. A reference to the optional itself: LevelEditorCommands binds it
        // before Attach fills it.
        [[nodiscard]] std::optional<UI::WindowChrome>& Chrome() { return m_WindowChrome; }

    private:
        Engine::Application* m_Application = nullptr;
        // Held as an optional rather than a value because it binds a reference to the Application's window,
        // which does not exist at construction time — and it stays EMPTY when the window is decorated, which
        // is what keeps "the editor draws the frame" and "the OS draws the frame" one code path with one
        // condition instead of two builds.
        std::optional<UI::WindowChrome>    m_WindowChrome;
        std::shared_ptr<ImGui::ImGuiLayer> m_ImGuiLayer;
    };
} // namespace Desert::Editor
