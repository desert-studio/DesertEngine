#pragma once

#include "IComponentWidget.hpp"

#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>

#include <memory>
#include <string>
#include <unordered_set>

namespace Desert::Editor
{
    class SkinnedMeshComponentWidget final : public IComponentWidget
    {
    public:
        /// @p ui is the host panel's (ComponentEditContext::UIHelper): the preview's texture id lives as long as
        /// it.
        SkinnedMeshComponentWidget( const std::weak_ptr<Assets::AssetManager>& assetManager, UI::UIHelper* ui );

        bool CanRemove() const override
        {
            return false;
        }

        void Render( ECS::Entity& entity, ::Desert::Core::Scene* scene = nullptr ) override;

    private:
        // The Skeletal Mesh row's preview box: the .skmesh photographed in its bind pose (UE: the slot shows the
        // USkeletalMesh's thumbnail), asked through ThumbnailService::RequestPose like the Content Browser's
        // tile, so the two share one picture. The glyph box until the picture lands; a refusal is logged once.
        void DrawMeshThumbnail( Assets::AssetManager& manager, const std::string& meshPath, float size,
                                bool filled );

        const std::weak_ptr<Assets::AssetManager> m_AssetManager;
        UI::UIHelper*                             m_UI = nullptr; // the host's; the pictures are SlotPictures()
    };
} // namespace Desert::Editor