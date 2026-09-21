#pragma once

#include "IComponentWidget.hpp"

#include <Editor/Core/Selection/AuthoringContext.hpp>

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
        const std::weak_ptr<Assets::AssetManager> m_AssetManager;

        // THIS PANEL'S OWN COPY OF WHAT IT AUTHORS. The state lives in the surface that owns it and is
        // PUBLISHED while that surface is the one being used — which is the whole of what replaced the four
        // process-wide statics in SkeletonEditMode. Kept here rather than read back out of the host so that
        // the tree still has an entity to name when nothing is published at all.
        Core::AuthoringContext m_Authoring;
    };
} // namespace Desert::Editor