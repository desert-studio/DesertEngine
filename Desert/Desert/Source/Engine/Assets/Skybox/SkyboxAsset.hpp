#pragma once

#include <Engine/Assets/TextureAsset.hpp>

namespace Desert::Assets
{
    class SkyboxAsset final : public AssetBase
    {
    public:
        SkyboxAsset( AssetPriority priority, const Common::Filepath& filepath );

        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;

        bool IsReadyForUse() const override
        {
            return m_ReadyForUse;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::Skybox;
        }

    private:
        bool                          m_ReadyForUse = false;
    };
} // namespace Desert::Assets
