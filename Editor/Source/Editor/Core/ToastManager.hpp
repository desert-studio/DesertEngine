#pragma once

#include <ImGui/imgui.h>

#include <cstdio>
#include <string>
#include <vector>

namespace Desert::Editor
{
    enum class ToastLevel
    {
        Info,
        Success,
        Warning,
        Error
    };

    // Transient, non-blocking notifications stacked at the bottom-right of the viewport (save/import/errors).
    // Push from anywhere; Draw() once per frame from EditorLayer decays + renders them. Header-only singleton.
    class ToastManager
    {
    public:
        static ToastManager& Get();

        static void Push( std::string message, ToastLevel level = ToastLevel::Info, float seconds = 4.0f )
        {
            Get().m_Toasts.push_back( Toast{ std::move( message ), level, seconds } );
        }

        void Draw()
        {
            if ( m_Toasts.empty() )
                return;

            const float          dt = ::ImGui::GetIO().DeltaTime;
            const ImGuiViewport* vp = ::ImGui::GetMainViewport();
            const float          x  = vp->WorkPos.x + vp->WorkSize.x - 12.0f;
            float                y  = vp->WorkPos.y + vp->WorkSize.y - 12.0f;

            int idx = 0;
            for ( auto it = m_Toasts.begin(); it != m_Toasts.end(); )
            {
                it->TimeLeft -= dt;
                if ( it->TimeLeft <= 0.0f )
                {
                    it = m_Toasts.erase( it );
                    continue;
                }

                const float  fade = it->TimeLeft < 0.5f ? ( it->TimeLeft / 0.5f ) : 1.0f;
                const ImVec4 col  = LevelColor( it->Level );

                char id[24];
                std::snprintf( id, sizeof( id ), "##toast%d", idx++ );

                ::ImGui::SetNextWindowBgAlpha( 0.92f * fade );
                ::ImGui::SetNextWindowPos( ImVec2( x, y ), ImGuiCond_Always, ImVec2( 1.0f, 1.0f ) );
                ::ImGui::PushStyleColor( ImGuiCol_Border, ImVec4( col.x, col.y, col.z, fade ) );
                ::ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 2.0f );

                const ImGuiWindowFlags flags =
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;
                if ( ::ImGui::Begin( id, nullptr, flags ) )
                {
                    ::ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( col.x, col.y, col.z, fade ) );
                    ::ImGui::TextUnformatted( LevelTag( it->Level ) );
                    ::ImGui::PopStyleColor();
                    ::ImGui::SameLine();
                    ::ImGui::TextUnformatted( it->Message.c_str() );
                    y = ::ImGui::GetWindowPos().y - 8.0f; // stack the next one above this
                }
                ::ImGui::End();

                ::ImGui::PopStyleVar();
                ::ImGui::PopStyleColor();
                ++it;
            }
        }

    private:
        struct Toast
        {
            std::string Message;
            ToastLevel  Level;
            float       TimeLeft;
        };

        static ImVec4 LevelColor( ToastLevel l )
        {
            switch ( l )
            {
                case ToastLevel::Success: return ImVec4( 0.40f, 0.85f, 0.45f, 1.0f );
                case ToastLevel::Warning: return ImVec4( 0.95f, 0.75f, 0.30f, 1.0f );
                case ToastLevel::Error:   return ImVec4( 0.95f, 0.40f, 0.40f, 1.0f );
                default:                  return ImVec4( 0.45f, 0.70f, 0.95f, 1.0f );
            }
        }
        static const char* LevelTag( ToastLevel l )
        {
            switch ( l )
            {
                case ToastLevel::Success: return "OK";
                case ToastLevel::Warning: return "WARN";
                case ToastLevel::Error:   return "ERROR";
                default:                  return "INFO";
            }
        }

        std::vector<Toast> m_Toasts;
    };
} // namespace Desert::Editor
