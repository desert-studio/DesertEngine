#pragma once

#include <Editor/Core/IPanel.hpp>

#include <Editor/Core/Commands/InputAssetEdit.hpp>
#include <Editor/Core/SubjectEditorRegistry.hpp>

#include <Engine/Assets/Serialization/WorldParameters.hpp>

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
     * @brief UE's Material Parameter Collection editor: one window per `.dworldparams`, opened by double-clicking
     * the asset — the parameter list (name, Scalar/Vector, default), add and remove.
     *
     * The window edits a WORKING copy; every change is one entry of the CommandHistory (the input editors'
     * InputAssetEditTransaction, which is generic over the data) and Save writes it through the asset serializer
     * (WorldParametersAsset::Save), then re-reads the asset so the world's instance re-adopts the new defaults
     * (Scene::SyncWorldParameters). A list the serializer refuses (a duplicate name, a non-identifier) is
     * refused at Save with the reason, never written.
     */
    class WorldParametersPanel final : public ISubjectDocument
    {
    public:
        WorldParametersPanel( const Assets::AssetHandle& subject, Assets::AssetManager* assets );

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 560.0f, 420.0f };
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
        using Data = Assets::Serialization::WorldParametersData;

        [[nodiscard]] InputAssetOwner<Data> Owner() const;

        Assets::AssetManager*           m_Assets = nullptr;
        Assets::AssetHandle             m_Handle;
        Common::Filepath                m_Path; // empty: the subject did not load
        std::shared_ptr<Data>           m_Data;
        Data                            m_OnDisk;
        InputAssetEditTransaction<Data> m_Edits;
        std::string                     m_Status;
        bool                            m_StatusIsError = false;
    };

    // The Content Browser's "Open" on a `.dworldparams`: registers the file with the asset manager if needed,
    // loads it, and asks the one handle route (Core::RequestOpenAsset) for its WorldParametersPanel. NotMine for
    // any other extension or a path with no file; Failed, with the reason logged, when it will not load or open.
    [[nodiscard]] SubjectEditorRegistry::PathOpenOutcome
    RequestWorldParametersDocument( Assets::AssetManager* assets, const std::string& path,
                                    const SubjectEditorRegistry& editors );
} // namespace Desert::Editor
