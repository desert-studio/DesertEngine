#pragma once

#include <Engine/UI/Ecs/UIIntrospectionEcs.hpp>

#include <unordered_map>

namespace Desert::Editor::Core
{
    // Routes one UI frame probe per open scene document, from the render pass that HAS the numbers to the
    // panel that shows them.
    //
    // KEYED BY SCENE, AND THAT IS THE WHOLE REASON THIS IS NOT A SINGLE STRUCT. The editor draws N scene
    // documents per frame, each with its own EditorUIPass; a single shared slot would show whichever pass
    // ran last, which is a wrong answer that looks exactly like a right one. It is the same defect the
    // UICanvasContext was introduced to close, and it is closed here the same way — state per view.
    //
    // The GATE lives in UI::UIFrameProbeSink, not here: this class only decides where a capture goes, and
    // the sink decides whether a capture happens at all. Arming is remembered so a document opened while
    // the panel is already up starts armed too, instead of showing an empty table until the user toggles
    // the panel off and on again.
    class UIProbeRegistry
    {
    public:
        static UIProbeRegistry& Get()
        {
            static UIProbeRegistry s;
            return s;
        }

        // The UI Debugger panel calls this while it is open and again when it closes.
        void SetArmed( bool armed )
        {
            m_Armed = armed;
            for ( auto& [key, sink] : m_Sinks )
                sink.SetArmed( armed );
        }

        [[nodiscard]] bool IsArmed() const
        {
            return m_Armed;
        }

        // The pass's own slot. @p sceneKey is the Scene's address — compared, never dereferenced.
        [[nodiscard]] ::Desert::UI::UIFrameProbeSink& Slot( const void* sceneKey )
        {
            auto it = m_Sinks.find( sceneKey );
            if ( it == m_Sinks.end() )
            {
                it = m_Sinks.emplace( sceneKey, ::Desert::UI::UIFrameProbeSink{} ).first;
                it->second.SetArmed( m_Armed );
            }
            return it->second;
        }

        // What the panel reads. Null when this scene has no UI pass, or has not drawn since arming —
        // which the panel must be able to tell apart from "this canvas costs nothing".
        [[nodiscard]] const ::Desert::UI::UIFrameProbeSink* Find( const void* sceneKey ) const
        {
            const auto it = m_Sinks.find( sceneKey );
            return it == m_Sinks.end() ? nullptr : &it->second;
        }

        // A closed document's slot: dropped, so a reopened scene at the same address cannot inherit the
        // previous one's numbers.
        void Forget( const void* sceneKey )
        {
            m_Sinks.erase( sceneKey );
        }

    private:
        bool                                                            m_Armed = false;
        std::unordered_map<const void*, ::Desert::UI::UIFrameProbeSink> m_Sinks;
    };
} // namespace Desert::Editor::Core
