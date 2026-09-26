#pragma once

#include <Editor/Panels/StaticMeshViewer/StaticMeshStats.hpp>
#include <Editor/Panels/StaticMeshViewer/StaticMeshViewerIdentity.hpp>

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
     * @brief One window per static mesh (`.stmesh`): the mesh in a large orbitable preview, lit by the preview's
     * HDR sky and sun, beside what it is made of — vertices, triangles, sections, the LOD chain and its bounds in
     * centimetres. UE's Static Mesh Editor reduced to a viewer.
     *
     * THE NUMBERS ARE THE BUILT PLATFORM DATA, the DDC value StaticMeshAsset draws (MeshSourceAsset -> builder ->
     * MeshBinary), not the source: the import settings (scale, up axis, generated LODs) are applied by the build,
     * so the source would describe a mesh nobody renders.
     *
     * LIGHT AND LOD ARE THE WINDOW'S. The light knobs are the preview scene's (PreviewViewport::SceneSetup) and
     * the LOD pick is the preview entity's StaticMeshComponent::ForcedLOD; changing them writes nothing to the
     * asset.
     *
     * A `.skmesh` is AssetTypeID::Mesh too, so the handle route refuses it by name before a window exists
     * (Core::AssetSubjectFor); the window repeats the refusal if it is ever handed one.
     */
    class StaticMeshViewerDocument final : public StaticMeshViewerBase
    {
    public:
        StaticMeshViewerDocument( const Assets::AssetHandle& mesh, Assets::AssetManager* assets );
        ~StaticMeshViewerDocument() override;

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 1100.0f, 760.0f };
        }

        void OnPreUpdate() override;
        void OnUIRender() override;

        [[nodiscard]] bool IsSubjectAlive() const override;

        // ~PreviewViewport idles the device and releases the renderer, which is what returns the slot.
        void ReleaseRendererSlot() override;

        [[nodiscard]] bool HasPreview() const override
        {
            return true;
        }
        void SetPreviewViewpoint( const PreviewViewpoint& viewpoint ) override;

        // "LOD Auto", "LOD 0".."LOD n-1": the LOD pick from the palette, because a headless shot cannot click a
        // combo. The list follows the mesh's built chain.
        [[nodiscard]] std::vector<DocumentAction> Actions() override;

    private:
        void EnsurePreview();
        void DrawStats() const;
        void DrawLightAndLOD();

        Assets::AssetManager*            m_Assets = nullptr;
        std::unique_ptr<PreviewViewport> m_Preview;
        std::unique_ptr<UI::UIHelper>    m_UIHelper;
        bool                             m_DrewThisFrame = false;
        glm::uvec2                       m_RenderSize{ 0u, 0u };
        std::string                      m_Unavailable; // why there is no picture, in the words the pane shows
        std::optional<StaticMeshStats>   m_Stats;
        std::optional<glm::vec2>         m_PendingOrbitDegrees;

        int m_ForcedLOD = -1; // -1 = auto, as StaticMeshComponent::ForcedLOD
    };

    // The `.stmesh` path opener: find-or-create the StaticMeshAsset, load it (which derives its platform data),
    // then open it through the one handle route, Core::RequestOpenAsset. Any other extension is NotMine.
    [[nodiscard]] SubjectEditorRegistry::PathOpenOutcome
    RequestStaticMeshDocument( Assets::AssetManager* assets, const std::string& path,
                               const SubjectEditorRegistry& editors );
} // namespace Desert::Editor
