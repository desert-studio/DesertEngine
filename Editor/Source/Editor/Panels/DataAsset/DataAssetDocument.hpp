#pragma once

#include <Editor/Core/SubjectEditorRegistry.hpp>
#include <Editor/Core/IPanel.hpp>

#include <Common/Core/Core.hpp> // Common::Filepath, which AssetMetadata.hpp names without including
#include <Engine/Assets/AssetMetadata.hpp>

#include <string>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
    /**
     * @brief One window per `.dasset`: a game's typed data asset in the GENERIC reflected property panel - UE's
     * UPrimaryDataAsset opened in the Details panel (ASSET-TYPES-OPEN).
     *
     * NO PER-TYPE EDITOR CODE: the window asks the asset which registered struct it holds and draws it with
     * PropertyEditorBuilder, the builder the Details panel draws components with. A WeaponDef a game module
     * registers is editable the moment it is registered, its Asset<> fields with the same picker a component's.
     *
     * EDITED IN PLACE, AS UE EDITS THE LOADED OBJECT: the builder writes into the asset's payload, so a running
     * game reading `asset->Get<WeaponDef>()` sees the new Damage at once. Undo entries are recorded inside the
     * builder with the payload as their edited object; the destructor forgets them (CommandHistory::DropFor).
     *
     * DIRTY IS DERIVED, NEVER A FLAG: the payload's serialized text against the text last saved or read, so an
     * undo back to the saved value is Clean again.
     */
    class DataAssetDocument final : public ISubjectDocument
    {
    public:
        DataAssetDocument( const Assets::AssetHandle& subject, Assets::AssetManager* assets );
        ~DataAssetDocument() override;

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 520.0f, 480.0f };
        }

        void OnUIRender() override;

        [[nodiscard]] bool IsSubjectAlive() const override;

        [[nodiscard]] bool HoldsView() const override
        {
            return false;
        }

        [[nodiscard]] bool ClaimsView() const override
        {
            return false;
        }

        [[nodiscard]] DiskState GetDiskState() const override;

        bool SaveDocument() override;

    private:
        /// The payload's text as the editor compares it (raw handles; the reference form is the file's business).
        [[nodiscard]] std::string PayloadFingerprint() const;

        Assets::AssetManager* m_Assets = nullptr;
        std::string           m_LoadError;        // why the window shows no fields; empty when it does
        std::string           m_SavedText;        // PayloadFingerprint at open and after each save
        std::string           m_Refusal;          // the last refused save, shown inline
        const void*           m_Edited = nullptr; // the payload the history's entries name (DropFor)
    };

    // The `.dasset` path opener: find-or-create the DataAsset, make sure it is read, then open it through the one
    // handle route, Core::RequestOpenAsset. NotMine for another extension or a path with no file; Failed, with
    // the reason logged, when it will not load (an unregistered Type is named) or open.
    [[nodiscard]] SubjectEditorRegistry::PathOpenOutcome
    RequestDataAssetDocument( Assets::AssetManager* assets, const std::string& path,
                              const SubjectEditorRegistry& editors );
} // namespace Desert::Editor
