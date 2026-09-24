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

        // The panorama `.detex`'s header GUID, adopted at creation; null when the file was absent or stated
        // none then. A material's cube slot names the skybox by it (MATL 3).
        [[nodiscard]] const Common::Content::AssetGuid& Guid() const
        {
            return m_Guid;
        }

    private:
        bool                       m_ReadyForUse = false;
        Common::Content::AssetGuid m_Guid;
    };
} // namespace Desert::Assets
