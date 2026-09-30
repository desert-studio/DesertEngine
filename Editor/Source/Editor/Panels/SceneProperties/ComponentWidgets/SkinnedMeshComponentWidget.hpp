#pragma once

#include "IComponentWidget.hpp"

#include <Editor/Core/Selection/AuthoringContext.hpp>
#include <Editor/Widgets/ThumbnailCache.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>

#include <memory>
#include <string>
#include <unordered_set>

namespace Desert::Editor
{
    class SkinnedMeshComponentWidget final : public IComponentWidget
    {
    public:
        SkinnedMeshComponentWidget( const std::weak_ptr<Assets::AssetManager>& assetManager );

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
        std::unique_ptr<UI::UIHelper>             m_UIHelper;
        ThumbnailCache                            m_Thumbnails;
        std::unordered_set<std::string>           m_RefusedThumbnails; // logged once, no per-frame retry

        // THIS PANEL'S OWN COPY OF WHAT IT AUTHORS. The state lives in the surface that owns it and is
        // PUBLISHED while that surface is the one being used — which is the whole of what replaced the four
        // process-wide statics in SkeletonEditMode. Kept here rather than read back out of the host so that
        // the tree still has an entity to name when nothing is published at all.
        Core::AuthoringContext m_Authoring;
    };
} // namespace Desert::Editor