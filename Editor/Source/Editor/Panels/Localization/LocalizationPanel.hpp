#pragma once

#include "../IPanel.hpp"

namespace Desert::Editor
{
    /**
     * @brief The language the editor is previewing in, and every hole in the translation. View ->
     *        Localization.
     *
     * WHY A PANEL AND NOT ONLY A MENU ENTRY. Ю15's first decision — what happens to a missing key — is
     * only worth taking if the answer is VISIBLE. On screen a missing key draws as itself, and in the log
     * it is one `LOG_ERROR` per (key, language); both are honest and neither answers "how much of this
     * project is translated?". This panel answers that in one list, and it is the only place in the editor
     * where a translator can see every key beside what it currently resolves to.
     *
     * IT IS NOT A TRANSLATION EDITOR, deliberately. A `.destrings` is prose, and prose is authored in a
     * text editor by somebody who can see the whole file — an ImGui grid of single-line fields is a worse
     * tool for that job than the one every translator already has. What the editor owes is the WHOLE
     * PICTURE and a live preview, which is what this is.
     *
     * The language combo is the same `Localization::SetLanguage` the command palette's Language entries
     * call, so there is one way to change the language and two doors onto it.
     */
    class LocalizationPanel final : public IPanel
    {
    public:
        LocalizationPanel();

        ImVec2 GetDefaultSize() const override
        {
            return ImVec2( 560.0f, 420.0f );
        }

        void OnUIRender() override;
    };
} // namespace Desert::Editor
