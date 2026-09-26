#pragma once

#include <Engine/Assets/AssetGuidRef.hpp>
#include <Engine/Assets/AssetRefSerialization.hpp>
#include <Engine/Assets/TextureAsset.hpp>

#include <string_view>

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

    class AssetManager;

    // THE ONE TRANSLATION between the name the runtime binds a shader by (its file stem, which LoadFromFile
    // holds equal to the DSL-declared name) and the reference a file stores ({header GUID, stable key}).
    // A material's editor action and a scene's MaterialComponent both go through it, and it goes through
    // WriteAssetGuidRef/ResolveAssetGuidRef, so the two formats cannot drift into two spellings of one
    // reference. Both refuse by `site`: a name no loaded shader has, a shader with no header GUID, and a
    // GUID no loaded shader adopted.
    [[nodiscard]] Common::ResultStr<AssetGuidRef> FindShaderRefByName( const AssetManager& manager,
                                                                       std::string_view    name,
                                                                       const AssetRefSite& site );
    [[nodiscard]] Common::ResultStr<std::string>  FindShaderNameByRef( const AssetManager& manager,
                                                                       const AssetGuidRef& ref,
                                                                       const AssetRefSite& site );
} // namespace Desert::Assets
