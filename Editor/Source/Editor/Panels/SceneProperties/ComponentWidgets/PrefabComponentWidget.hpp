#pragma once

#include "IComponentWidget.hpp"

namespace Desert::Editor
{
    class PrefabComponentWidget final : public IComponentWidget
    {
    public:
        explicit PrefabComponentWidget( const Assets::AssetManager* assetManager );

        bool CanRemove() const override
        {
            return false;
        }

        void Render( ECS::Entity& entity, ::Desert::Core::Scene* scene ) override;

    private:
        const Assets::AssetManager* m_AssetManager;
        std::string                 m_SelectPathBuf;
    };
} // namespace Desert::Editor