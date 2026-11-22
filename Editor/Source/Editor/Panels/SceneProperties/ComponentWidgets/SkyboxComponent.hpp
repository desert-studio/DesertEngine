#pragma once

#include "IComponentWidget.hpp"

#include <memory>
#include <Engine/Assets/Common.hpp>

namespace Desert::Assets
{
    class AssetManager;
} // namespace Desert::Assets

namespace Desert::Editor
{
    class SkyboxComponentWidget final : public IComponentWidget
    {
    public:
        SkyboxComponentWidget( const std::weak_ptr<Assets::AssetManager>& assetManager );

        bool CanRemove() const override
        {
            return false;
        }

        void Render( ECS::Entity& entity, ::Desert::Core::Scene* scene = nullptr ) override;

    private:
        const std::weak_ptr<Assets::AssetManager> m_AssetManager;
    };
} // namespace Desert::Editor