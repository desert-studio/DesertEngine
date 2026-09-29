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
        ShaderAsset( const Common::Filepath& filepath );

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

        // The template manifest the file declares (`Role <Name>`, `Default Surface`) — see ReadShaderManifest.
        const std::string& GetRole() const
        {
            return m_Role;
        }
        bool IsDefaultSurface() const
        {
            return m_DefaultSurface;
        }

    private:
        bool        m_ReadyForUse = false;
        std::string m_ShaderContent;
        std::string m_Role;
        bool        m_DefaultSurface = false;
    };

    // The roles engine code asks the template registry for. A role is declared by the shader file
    // (`Role <Name>`), exactly one loaded shader per role.
    inline constexpr std::string_view kPBRSurfaceRole = "PBRSurface"; // the batched PBR backend (until MAT1a)
    inline constexpr std::string_view kDebugColorRole = "DebugColor"; // the scripting flat-colour material

    class AssetManager;

    // THE ONE TRANSLATION between the name the runtime binds a shader by (its file stem, which LoadFromFile
    // holds equal to the DSL-declared name) and the reference a file stores ({header GUID, stable key}).
    // A material's editor action and a scene's MaterialComponent both go through it, and it goes through
    // WriteAssetGuidRef/ResolveAssetGuidRef, so the two formats cannot drift into two spellings of one
    // reference. Both refuse by `site`: a name no loaded shader has, a shader with no header GUID, and a
    // GUID no loaded shader adopted.
    [[nodiscard]] Common::ResultStr<AssetGuidRef>
    FindShaderRefByName( const AssetManager& manager, std::string_view name, const AssetRefSite& site );
    [[nodiscard]] Common::ResultStr<std::string>
    FindShaderNameByRef( const AssetManager& manager, const AssetGuidRef& ref, const AssetRefSite& site );

    // THE TEMPLATE REGISTRY, over the loaded shaders' manifests. Exactly one shader declares @p role; none or
    // several is a refusal listing every declaring path (none: the role and "no loaded shader declares it").
    [[nodiscard]] Common::ResultStr<Common::AssetHandle> FindTemplateByRole( const AssetManager& manager,
                                                                             std::string_view    role );

    // The template a new material is created with. @p projectOverride is the project's optional `.deproj`
    // "DefaultSurfaceTemplate" GUID (Project::ProjectContext::DefaultSurfaceTemplate(); empty = not stated); a
    // stated GUID no loaded shader has is refused naming @p deprojPath. Without it, the one shader declaring
    // `Default Surface` — none or several is refused listing the paths.
    [[nodiscard]] Common::ResultStr<Common::AssetHandle>
    FindDefaultSurfaceTemplate( const AssetManager& manager, std::string_view projectOverride,
                                std::string_view deprojPath );
} // namespace Desert::Assets
