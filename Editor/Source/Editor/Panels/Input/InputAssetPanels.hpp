#pragma once

#include "../IPanel.hpp"

#include <Editor/Core/Commands/InputAssetEdit.hpp>

#include <Engine/Assets/Serialization/InputAssets.hpp>

#include <Common/Core/Core.hpp>

#include <memory>
#include <string>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
    /**
     * @brief UE's Input Action editor: one window per `.deinputaction`, opened by double-clicking the asset.
     *
     * The window edits a WORKING copy; every change is one entry of the CommandHistory (InputAssetEdit.hpp)
     * and Save writes it through the asset serializer (InputActionAsset::Save), then re-reads the asset so
     * Play reads what was saved.
     */
    class InputActionPanel final : public ISubjectDocument
    {
    public:
        InputActionPanel( const Assets::AssetHandle& subject, Assets::AssetManager* assets );

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 420.0f, 220.0f };
        }
        void               OnUIRender() override;
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
        bool                    SaveDocument() override;

    private:
        [[nodiscard]] InputAssetOwner<Assets::Serialization::InputActionData> Owner() const;

        Assets::AssetManager*                                   m_Assets = nullptr;
        Assets::AssetHandle                                     m_Handle;
        Common::Filepath                                        m_Path; // empty: the subject did not load
        std::shared_ptr<Assets::Serialization::InputActionData> m_Data;
        Assets::Serialization::InputActionData                  m_OnDisk;
        InputActionEditTransaction                              m_Edits;
        std::string                                             m_Status;
        bool                                                    m_StatusIsError = false;
    };

    /**
     * @brief UE's Input Mapping Context editor: the mappings (action + key), each with its ordered modifiers
     * and its triggers. One window per `.deinputcontext`; same working-copy / undo / save rules as above, and
     * a context the subsystem could not evaluate (ValidateInputMappingContext) is refused, not saved.
     */
    class InputMappingContextPanel final : public ISubjectDocument
    {
    public:
        InputMappingContextPanel( const Assets::AssetHandle& subject, Assets::AssetManager* assets );

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 560.0f, 640.0f };
        }
        void               OnUIRender() override;
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
        bool                    SaveDocument() override;

    private:
        [[nodiscard]] InputAssetOwner<Assets::Serialization::InputMappingContextData> Owner() const;

        Assets::AssetManager*                                           m_Assets = nullptr;
        Assets::AssetHandle                                             m_Handle;
        Common::Filepath                                                m_Path;
        std::shared_ptr<Assets::Serialization::InputMappingContextData> m_Data;
        Assets::Serialization::InputMappingContextData                  m_OnDisk;
        InputContextEditTransaction                                     m_Edits;
        std::string                                                     m_Status;
        bool                                                            m_StatusIsError = false;
    };
} // namespace Desert::Editor
