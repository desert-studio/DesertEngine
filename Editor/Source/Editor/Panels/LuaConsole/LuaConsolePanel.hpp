#pragma once

#include "../IPanel.hpp"

#include <memory>
#include <string>
#include <vector>

namespace Desert::Core
{
    class Scene;
}
namespace Desert::Assets
{
    class AssetManager;
}
namespace Desert::Scripting
{
    class ScriptEngine;
}

namespace Desert::Editor
{
    // An interactive Lua REPL against the live editor scene. It owns its OWN ScriptEngine (all the same
    // bindings — Entity/World/Input/Material/...), created lazily on first use, so you can query and
    // poke the scene from the editor without entering Play. Hidden by default; View -> Lua Console.
    class LuaConsolePanel final : public IPanel
    {
    public:
        LuaConsolePanel( ::Desert::Core::Scene* scene, ::Desert::Assets::AssetManager* assetManager );
        ~LuaConsolePanel() override;

        glm::vec2 GetDefaultSize() const override
        {
            return { 720.0f, 420.0f };
        }

        void OnUIRender() override;

        // Run a line typed somewhere else — the status bar's Cmd field. Queued rather than executed on the
        // spot: the console owns the Lua engine, the log and the history, and a second entry point into
        // them would be a second console.
        static void Submit( std::string command );

    private:
        void Execute();

        struct Line
        {
            std::string Text;
            bool        IsInput = false;
            bool        IsError = false;
        };

        ::Desert::Core::Scene*                     m_Scene        = nullptr;
        ::Desert::Assets::AssetManager*            m_AssetManager = nullptr;
        std::unique_ptr<::Desert::Scripting::ScriptEngine> m_Engine; // lazily created on first Execute

        std::vector<Line>        m_Log;
        char                     m_Input[512] = {};
        std::vector<std::string> m_History;
        int                      m_HistoryPos    = -1; // -1 = editing a fresh line
        bool                     m_ScrollToBottom = false;
        bool                     m_ReclaimFocus   = false;

    public:
        // ImGui InputText history callback (Up/Down recall). Public so the free callback can reach it.
        int OnHistoryCallback( void* data );
    };
} // namespace Desert::Editor
