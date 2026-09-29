#pragma once

#include <Editor/Panels/IPanel.hpp>

namespace Desert::Editor
{
    /**
     * @brief What Ctrl+S writes: the asset of the document window that has the keyboard, or the scene.
     *
     * UE's Ctrl+S in an asset editor saves THAT asset and never the level. EditorLayer's last-focused document
     * (m_FocusedDocument) is deliberately not cleared when the keyboard goes to the viewport — Ctrl+Tab resumes
     * from it — so it cannot answer "is a document in focus NOW"; the caller passes that separately. A focused
     * document with nothing on disk (Untracked: a procedural subject) saves nothing rather than falling through
     * to the scene: the person asked for the window in front of them, not for the level behind it.
     */
    enum class SaveShortcutTarget
    {
        Scene,
        FocusedDocument,
        Nothing
    };

    [[nodiscard]] inline SaveShortcutTarget ResolveSaveShortcut( const bool              documentHasFocus,
                                                                 const ISubjectDocument* focused )
    {
        if ( !documentHasFocus || focused == nullptr )
            return SaveShortcutTarget::Scene;
        return focused->GetDiskState() == ISubjectDocument::DiskState::Untracked
                    ? SaveShortcutTarget::Nothing
                    : SaveShortcutTarget::FocusedDocument;
    }
} // namespace Desert::Editor
