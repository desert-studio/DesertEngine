#pragma once

#include <Editor/Panels/SkyboxViewer/SkyboxViewLevels.hpp>
#include <Editor/Panels/SkyboxViewer/SkyboxViewerIdentity.hpp>

#include <Common/Core/Core.hpp> // Common::Filepath, which AssetMetadata.hpp names without including
#include <Engine/Assets/AssetMetadata.hpp>

#include <glm/glm.hpp>

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
     * @brief One window per skybox (an HDR panorama `.detex` imported as ContentKind::Skybox): the sky all
     * around, a ball of it in the middle, orbited with the mouse.
     *
     * UE's TextureCube editor in its 3D mode, reduced to a viewer: the cube is the BACKGROUND (every ray that
     * misses the ball samples it by its own direction), so orbiting is looking around the sky, and the ball in
     * front shows the same map wrapped onto an object.
     *
     * EXPOSURE AND ROTATION ARE THE WINDOW'S, NOT THE ASSET'S AND NOT THE COMPONENT'S. A SkyboxComponent turns
     * and grades the sky for one scene; this window is about the FILE, so it shows the file as authored (unit
     * gain, white tint) and offers its own two knobs for looking at it — changing them writes nothing anywhere.
     * Exposure is the preview scene's grade (PreviewViewport::SceneSetup::Exposure), rotation is the SkyLook the
     * resolver hands the pass.
     */
    class SkyboxViewerDocument final : public SkyboxViewerBase
    {
    public:
        SkyboxViewerDocument( const Assets::AssetHandle& skybox, Assets::AssetManager* assets );
        ~SkyboxViewerDocument() override;

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 900.0f, 720.0f };
        }

        void OnPreUpdate() override;
        void OnUIRender() override;

        // Asked of the metadata, as every asset document does: whether the asset is still THERE. A skybox the
        // service cannot build is reported in the window, not as a dead subject.
        [[nodiscard]] bool IsSubjectAlive() const override;

        // ~PreviewViewport idles the device and releases the renderer, which is what returns the slot.
        void ReleaseRendererSlot() override;

        [[nodiscard]] bool HasPreview() const override
        {
            return true;
        }
        void SetPreviewViewpoint( const PreviewViewpoint& viewpoint ) override;

        // "Look at zenith / mid sky / horizon" — the three elevations a sky has to be checked at — and the
        // viewing modes (SkyboxView::ViewActions: 3D/2D, level, EV, rotation), reachable from the palette
        // because macOS refuses synthetic input and a headless shot cannot drag.
        [[nodiscard]] std::vector<DocumentAction> Actions() override;

    private:
        void EnsurePreview();
        void SetOrbitDegrees( float yawDegrees, float pitchDegrees );

        Assets::AssetManager*            m_Assets = nullptr;
        std::unique_ptr<PreviewViewport> m_Preview;
        std::unique_ptr<UI::UIHelper>    m_UIHelper;
        bool                             m_DrewThisFrame = false;
        glm::uvec2                       m_RenderSize{ 0u, 0u };
        std::string                      m_Unavailable; // why there is no picture, in the words the pane shows
        // An orbit asked for before the preview exists (the palette can aim a window that has not drawn yet).
        std::optional<glm::vec2> m_PendingOrbitDegrees;

        // The window's own viewing knobs (see the class comment and SkyboxViewLevels.hpp). Never saved.
        SkyboxView::ViewState m_View;
        uint32_t              m_PrefilteredMips = 0; // read off the cached chain each frame
    };

    // The `.detex` path opener for SKYBOX panoramas: NotMine unless the file's header says ContentKind::Skybox,
    // so the texture opener keeps every other `.detex`. Find-or-create the SkyboxAsset, load it (which checks
    // the file is there), then open it through the one handle route, Core::RequestOpenAsset.
    [[nodiscard]] SubjectEditorRegistry::PathOpenOutcome
    RequestSkyboxDocument( Assets::AssetManager* assets, const std::string& path,
                           const SubjectEditorRegistry& editors );
} // namespace Desert::Editor
