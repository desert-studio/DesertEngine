#pragma once

#include <Editor/Core/SubjectEditorRegistry.hpp>
#include <Editor/Panels/IPanel.hpp>

#include <Common/Core/Core.hpp> // Common::Filepath, which AssetMetadata.hpp names without including
#include <Engine/Assets/AssetMetadata.hpp>
#include <Engine/Assets/Serialization/VFXSystem.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor::UI
{
    class UIHelper;
}

namespace Desert::Editor
{
    class PreviewViewport;

    /**
     * @brief One window per `.dfx`: UE's Niagara System editor (VFX-12, first slice 12a/12b).
     *
     * Three columns as the VfxSystemWindow mockup: SYSTEM (system fields, User parameters, the emitter list) |
     * PREVIEW (a PreviewViewport whose scene holds one VFXComponent playing this system, realtime) | STACK (the
     * selected emitter's modules by stage - Emitter Update, Particle Spawn, Particle Update - then Render rows).
     *
     * WORKING AND SAVED (12c): the window edits m_Working, a copy of the asset's data; Dirty is m_Working !=
     * m_Saved (derived, never a flag). A stack input's gesture (a drag, a typed number, a `set` from the control
     * socket) is ONE CommandHistory entry (VFXSystemEditCommand, the whole system before/after), refused by name
     * when ValidateVFXSystemData rejects the result. Save writes m_Working through VFXSystemAsset::Save, re-reads
     * the held asset and refreshes VFXSystemService, so level effects play what was written.
     *
     * THE PREVIEW PLAYS THE WORKING COPY (12a-override, UE's Niagara editor plays its own system copy): whenever
     * m_Working differs from what the preview last got - an edit, an undo, a redo - a new immutable copy goes to
     * the preview's VFXComponent::EditedSystem, and the new pointer restarts the simulation (UE ResetSimulation on
     * every stack change). Levels keep playing the asset until Save.
     */
    class VFXSystemDocument final : public ISubjectDocument
    {
    public:
        VFXSystemDocument( const Assets::AssetHandle& subject, Assets::AssetManager* assets );
        ~VFXSystemDocument() override; // forgets the history's entries, which point at m_Working

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 1280.0f, 720.0f };
        }

        void OnPreUpdate() override;
        void OnUIRender() override;

        [[nodiscard]] bool IsSubjectAlive() const override;

        [[nodiscard]] bool ClaimsView() const override
        {
            return true;
        }

        [[nodiscard]] bool HoldsView() const override
        {
            return m_PreviewLive;
        }

        // ~PreviewViewport idles the device and releases the renderer, which is what returns the slot.
        void ReleaseView() override;

        [[nodiscard]] bool HasPreview() const override
        {
            return true;
        }

        [[nodiscard]] DiskState GetDiskState() const override
        {
            if ( !m_Loaded )
                return DiskState::Untracked;
            return m_Working != m_Saved ? DiskState::Dirty : DiskState::Clean;
        }

        bool SaveDocument() override;

        /// Every stack input a number sets: `<Emitter>.<Stage>.<Module>.<Input>` for a Value, plus `.Min`/`.Max`
        /// for a Random range (a module repeated in one stage is `<Module>#2`, ...). Curve and Binding inputs are
        /// listed as not settable, saying why.
        [[nodiscard]] std::vector<EditableProperty> EditableProperties() const override;
        [[nodiscard]] Common::BoolResultStr         SetEditableProperty( const std::string&        name,
                                                                         const std::vector<float>& value ) override;

        void ExtendToolbar( AssetEditorToolbar& toolbar ) override;

    private:
        void EnsurePreview();
        void DrawSystemColumn();
        void DrawStackColumn();
        void DrawStage( const char* title, std::size_t emitter, std::size_t stage );
        void DrawInputEditor( std::size_t emitter, std::size_t stage, std::size_t module, std::size_t input );
        // Ends a gesture that began from @p before: one undo step labelled @p label, or - when the result is not a
        // valid system - m_Working back to @p before and the refusal shown.
        Common::BoolResultStr Commit( Assets::Serialization::VFXSystemData before, const std::string& label );

        Assets::AssetManager*             m_Assets = nullptr;
        bool                              m_Loaded = false;
        std::string                       m_LoadError;
        Assets::Serialization::VFXSystemData m_Working; // what the window edits (CommandHistory target)
        Assets::Serialization::VFXSystemData m_Saved;   // what the file holds
        // What the preview plays: a frozen copy of m_Working, replaced whenever the two differ.
        std::shared_ptr<const Assets::Serialization::VFXSystemData> m_Played;
        // m_Working when the active widget's gesture began (set while a drag is held).
        std::optional<Assets::Serialization::VFXSystemData> m_GestureBefore;
        std::string                                         m_Refusal;
        std::size_t                       m_SelectedEmitter = 0;

        std::unique_ptr<PreviewViewport> m_Preview;
        std::unique_ptr<UI::UIHelper>    m_UIHelper;
        bool                             m_PreviewLive   = false;
        bool                             m_DrewThisFrame = false;
        glm::uvec2                       m_ViewSize{ 0u, 0u };
    };

    // The `.dfx` path opener: find-or-create the VFXSystemAsset, make sure it is read, then open it through the one
    // handle route, Core::RequestOpenAsset. NotMine for any other extension or a path with no file.
    [[nodiscard]] SubjectEditorRegistry::PathOpenOutcome
    RequestVFXSystemDocument( Assets::AssetManager* assets, const std::string& path,
                              const SubjectEditorRegistry& editors );
} // namespace Desert::Editor
