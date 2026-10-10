#pragma once

#include <Editor/Core/SubjectEditorRegistry.hpp>
#include <Editor/Panels/IPanel.hpp>

#include <Common/Core/Core.hpp> // Common::Filepath, which AssetMetadata.hpp names without including
#include <Engine/Assets/AssetMetadata.hpp>

#include <Engine/Core/Formats/ImageFormat.hpp>

#include <memory>
#include <optional>
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
     * ONE AUTHORED SETTING, THE COLOUR SPACE (UE UTexture::SRGB). It is written to the `.detex` at once (as UE's
     * texture editor applies a setting) through ApplyColorSpace, the one path the toolbar combo and the control
     * channel's `set ColorSpace` both take; the built GPU image is evicted so the next Get re-cooks under the new
     * DDC key. Every other fact here is read-only, so the document answers Clean and offers no save.
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

        [[nodiscard]] bool HoldsView() const override
        {
            return false;
        }

        [[nodiscard]] bool ClaimsView() const override
        {
            return false;
        }

        // Nothing in this window changes the file, so it cannot differ from it.
        [[nodiscard]] DiskState GetDiskState() const override
        {
            return DiskState::Clean;
        }

        // `ColorSpace` (int: 0 = Linear, 1 = sRGB; Timing "Rebake") — the asset's authored colour space.
        [[nodiscard]] std::vector<EditableProperty> EditableProperties() const override;
        [[nodiscard]] Common::BoolResultStr         SetEditableProperty( const std::string&        name,
                                                                         const std::vector<float>& value ) override;

    private:
        // THE ONE WRITE: Assets::SetTextureColorSpace on the `.detex`, then evict the built image (re-cook).
        [[nodiscard]] Common::BoolResultStr ApplyColorSpace( ::Desert::Core::Formats::TextureColorSpace space );
        // The `.detex` path from the asset's metadata; empty when the asset is gone.
        [[nodiscard]] std::string AssetFile() const;
        // Read from the file on first need and kept in step by ApplyColorSpace.
        [[nodiscard]] Common::ResultStr<::Desert::Core::Formats::TextureColorSpace> CurrentColorSpace() const;

        // Zoom about `pivot` (a screen position inside the canvas) so the texel under it stays under it.
        void ZoomAbout( float newZoom, const ImVec2& pivot, const ImVec2& canvasCentre );

        Assets::AssetManager*         m_Assets = nullptr;
        std::unique_ptr<UI::UIHelper> m_UIHelper;
        bool                          m_Fit  = true; // zoom follows the canvas until the user zooms or pans
        float                         m_Zoom = 1.0f; // screen pixels per texel
        ImVec2                        m_Pan  = ImVec2( 0.0f, 0.0f ); // image centre offset from canvas centre
        mutable std::optional<::Desert::Core::Formats::TextureColorSpace> m_ColorSpace;
    };

    // The `.detex` path opener: find-or-create the TextureAsset, load it, hand it to the texture service (which
    // builds the GPU image on first Get), then open it through the one handle route, Core::RequestOpenAsset.
    // NotMine for any other extension or a path with no file; Failed, with the reason logged, when it will not
    // load.
    [[nodiscard]] SubjectEditorRegistry::PathOpenOutcome
    RequestTextureDocument( Assets::AssetManager* assets, const std::string& path,
                            const SubjectEditorRegistry& editors );
} // namespace Desert::Editor
