#pragma once

#include <Editor/Core/SubjectEditorRegistry.hpp>
#include <Editor/Panels/IPanel.hpp>

#include <Common/Core/Core.hpp> // Common::Filepath, which AssetMetadata.hpp names without including
#include <Engine/Assets/AssetMetadata.hpp>

#include <memory>
#include <string>

#include <ImGui/imgui.h>

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
    /**
     * @brief One window per `.detex`: the texture itself, at any zoom, with the facts a material slot cannot show.
     *
     * UE's texture editor, reduced to what a viewer needs: the picture, wheel zoom about the cursor, drag to
     * pan, "Fit" and "1:1", and one line of facts read from the IMAGE that is bound on the GPU — size, format,
     * mip count, and whether it is block-compressed. Those come from the Image2D rather than from the asset
     * because the asset holds only the source path and the identity; the cooked format is decided by the cook
     * and is only knowable from what was actually uploaded.
     *
     * A VIEWER, NOT AN EDITOR: nothing here writes the file, so the document answers Clean and offers no save.
     *
     * NO RENDERER SLOT, NOW OR LATER. The picture is the image the texture service already owns, drawn through
     * the ImGui descriptor cache; no PreviewViewport, no Scene and no SceneRenderer is created, so the six-slot
     * census counts this document at zero for its whole life (same answer as the cloud documents, and for the
     * same reason).
     */
    class TextureViewerDocument final : public ISubjectDocument
    {
    public:
        TextureViewerDocument( const Assets::AssetHandle& subject, Assets::AssetManager* assets );
        ~TextureViewerDocument() override;

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 720.0f, 640.0f };
        }

        void OnUIRender() override;

        // Asked of the metadata, as every asset document does: the question is whether the asset is still
        // THERE, not whether it still loads as a texture — a load failure is reported in the window.
        [[nodiscard]] bool IsSubjectAlive() const override;

        [[nodiscard]] bool HoldsRendererSlot() const override
        {
            return false;
        }

        [[nodiscard]] bool ClaimsRendererSlot() const override
        {
            return false;
        }

        // Nothing in this window changes the file, so it cannot differ from it.
        [[nodiscard]] DiskState GetDiskState() const override
        {
            return DiskState::Clean;
        }

    private:
        // Zoom about `pivot` (a screen position inside the canvas) so the texel under it stays under it.
        void ZoomAbout( float newZoom, const ImVec2& pivot, const ImVec2& canvasCentre );

        Assets::AssetManager*         m_Assets = nullptr;
        std::unique_ptr<UI::UIHelper> m_UIHelper;
        bool                          m_Fit  = true; // zoom follows the canvas until the user zooms or pans
        float                         m_Zoom = 1.0f; // screen pixels per texel
        ImVec2                        m_Pan  = ImVec2( 0.0f, 0.0f ); // image centre offset from canvas centre
    };

    // The `.detex` path opener: find-or-create the TextureAsset, load it, hand it to the texture service (which
    // builds the GPU image on first Get), then open it through the one handle route, Core::RequestOpenAsset.
    // NotMine for any other extension or a path with no file; Failed, with the reason logged, when it will not
    // load.
    [[nodiscard]] SubjectEditorRegistry::PathOpenOutcome
    RequestTextureDocument( Assets::AssetManager* assets, const std::string& path,
                            const SubjectEditorRegistry& editors );
} // namespace Desert::Editor
