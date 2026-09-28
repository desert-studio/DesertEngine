#pragma once

#include <Editor/Panels/AnimationEditor/AnimationEditorIdentity.hpp>
#include <Editor/Panels/AnimationEditor/AnimationTransport.hpp>

#include <Common/Core/Core.hpp> // Common::Filepath, which AssetMetadata.hpp names without including
#include <Engine/Assets/AssetMetadata.hpp>

#include <glm/glm.hpp>

#include <memory>
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
     * @brief One window per animation clip (`.anim`): UE's Animation Editor (Persona) — part 1, the preview
     * viewport and the transport.
     *
     * THE MESH IS FOUND BY THE CLIP'S RIG. A clip names a skeleton signature and nothing else; the window shows
     * the first registered `.skmesh` (by path, so the pick is stable) whose skeleton signature is the clip's.
     * No such mesh is a named error in the window — the signature by number — never a stand-in mesh.
     *
     * THE POSE IS A FUNCTION OF THE TRANSPORT'S TIME. AnimationTransport owns the clock; the preview's animator
     * is stopped and told the time every frame (PreviewViewport::SetAnimationTime), so a scrub, a step or a
     * palette "Set Time" land on the same pose a playing clip shows at that time.
     *
     * The window leaves room (a right-hand column) for the parts that follow: notifies/curves tracks, the
     * skeleton tree and bone details.
     */
    class AnimationEditorDocument final : public AnimationEditorBase
    {
    public:
        AnimationEditorDocument( const Assets::AssetHandle& clip, Assets::AssetManager* assets );
        ~AnimationEditorDocument() override;

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 1200.0f, 800.0f };
        }

        void OnPreUpdate() override;
        void OnUIRender() override;

        [[nodiscard]] bool IsSubjectAlive() const override;

        [[nodiscard]] bool HasPreview() const override
        {
            return true;
        }
        void SetPreviewViewpoint( const PreviewViewpoint& viewpoint ) override;

        // Palette entries for a headless shot: "Play/Pause", "Set Time 0%".."Set Time 100%" (quarters of the
        // clip), "Next Frame", "Previous Frame".
        [[nodiscard]] std::vector<DocumentAction> Actions() override;

    protected:
        void DestroyPreview() override;

    private:
        void EnsurePreview();
        void DrawOverlay( const glm::vec2& origin ) const;
        void DrawTransport();

        Assets::AssetManager*            m_Assets = nullptr;
        std::unique_ptr<PreviewViewport> m_Preview;
        std::unique_ptr<UI::UIHelper>    m_UIHelper;
        bool                             m_DrewThisFrame = false;
        glm::uvec2                       m_RenderSize{ 0u, 0u };
        std::string                      m_Unavailable; // why there is no picture, in the words the pane shows
        std::string                      m_ClipName;
        std::string                      m_MeshName;
        AnimationTransport               m_Transport;
        std::unique_ptr<glm::vec2>       m_PendingOrbitDegrees;
    };

    // The `.anim` path opener: find-or-create the AnimationAsset, load it, then open it through the one handle
    // route, Core::RequestOpenAsset. Any other extension is NotMine.
    [[nodiscard]] SubjectEditorRegistry::PathOpenOutcome
    RequestAnimationEditorDocument( Assets::AssetManager* assets, const std::string& path,
                                    const SubjectEditorRegistry& editors );

    // The clip file extension the opener claims.
    inline constexpr const char* kAnimationClipExtension = ".anim";
} // namespace Desert::Editor
