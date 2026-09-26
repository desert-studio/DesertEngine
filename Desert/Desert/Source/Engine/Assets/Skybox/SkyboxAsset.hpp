#pragma once

#include <Engine/Assets/TextureAsset.hpp>

#include <memory>

namespace Desert::Graphic
{
    struct StagedEnvironment;
}

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
        /// What `LoadFromFile` read on the loader's worker; null until the asset has been loaded.
        [[nodiscard]] std::shared_ptr<const Graphic::StagedEnvironment> StagedEnvironment() const
        {
            return m_Staged;
        }

        [[nodiscard]] const Common::Content::AssetGuid& Guid() const
        {
            return m_Guid;
        }

    private:
        bool                       m_ReadyForUse = false;
        Common::Content::AssetGuid m_Guid;
        std::shared_ptr<const Graphic::StagedEnvironment> m_Staged;
    };
} // namespace Desert::Assets
