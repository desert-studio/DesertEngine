#pragma once

#include <Engine/Assets/TextureAsset.hpp>

namespace Desert::Assets
{
    class ShaderAsset final : public AssetBase
    {
    public:
        ShaderAsset( AssetPriority priority, const Common::Filepath& filepath );

        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;

        bool IsReadyForUse() const override
        {
            return m_ReadyForUse;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::Shader;
        }

        const auto& GetShaderContent() const
        {
            return m_ShaderContent;
        }

    private:
        bool        m_ReadyForUse = false;
        std::string m_ShaderContent;
    };
} // namespace Desert::Assets
