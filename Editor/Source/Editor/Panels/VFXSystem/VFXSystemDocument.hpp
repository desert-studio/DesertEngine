#pragma once

#include <Editor/Core/SubjectEditorRegistry.hpp>
#include <Editor/Panels/IPanel.hpp>

#include <Common/Core/Core.hpp> // Common::Filepath, which AssetMetadata.hpp names without including
#include <Engine/Assets/AssetMetadata.hpp>
#include <Engine/Assets/Serialization/VFXSystem.hpp>

#include <cstddef>
#include <memory>
#include <string>

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
     * READ-ONLY IN THIS SLICE: the document shows the asset's data as read at open; editing (one CommandHistory
     * entry per edit, VFXSystemEditCommand) and the editor override that lets the preview play unsaved data
     * (VFXSystemService::SetEditorOverride) are the next slices (12c/12a-override), so GetDiskState is Clean.
     */
    class VFXSystemDocument final : public ISubjectDocument
    {
    public:
        VFXSystemDocument( const Assets::AssetHandle& subject, Assets::AssetManager* assets );
        ~VFXSystemDocument() override;

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
            return m_Loaded ? DiskState::Clean : DiskState::Untracked;
        }

        void ExtendToolbar( AssetEditorToolbar& toolbar ) override;

    private:
        void EnsurePreview();
        void DrawSystemColumn();
        void DrawStackColumn();

        Assets::AssetManager*             m_Assets = nullptr;
        bool                              m_Loaded = false;
        std::string                       m_LoadError;
        Assets::Serialization::VFXSystemData m_Data;
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
