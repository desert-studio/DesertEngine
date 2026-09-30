#pragma once

#include <optional>

#include <Engine/Assets/AssetGuidRef.hpp>
#include <Common/Content/ShaderAssetHeader.hpp>
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
        [[nodiscard]] const std::string& GetRole() const
        {
            return m_Role;
        }
        [[nodiscard]] bool IsDefaultSurface() const
        {
            return m_DefaultSurface;
        }

    private:
        bool        m_ReadyForUse = false;
        std::string m_ShaderContent;
        std::string m_Role;
        bool        m_DefaultSurface = false;
    };

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

    // The same reference, for a shader chosen BY HANDLE (a template picker row, a role lookup). Refuses by
    // `site` a handle no loaded shader has and a shader file with no header GUID.
    [[nodiscard]] Common::ResultStr<AssetGuidRef>
    FindShaderRefByHandle( const AssetManager& manager, Common::AssetHandle shader, const AssetRefSite& site );

    // The loaded shader whose file stem — the ShaderService compile key — is @p compileName. Only for a caller
    // that CREATES a shader under a name (the node-graph editor compiles its graph to `<Name>.dshader`) and
    // must then find the asset it made; a template is never chosen by name.
    [[nodiscard]] std::optional<Common::AssetHandle> FindShaderHandleByCompileName( const AssetManager& manager,
                                                                                    std::string_view compileName );

    // Whether @p shader is a loaded template declaring `Role PBRSurface` (the batched PBR backend).
    [[nodiscard]] bool IsPBRSurfaceTemplate( const AssetManager& manager, Common::AssetHandle shader );

    // A MaterialComponent's Shader: the compile key of an OVERRIDE template. FindShaderNameByRef's refusals, and
    // a reference naming the `Role PBRSurface` template, which overrides nothing (the mesh draws its material
    // slots; the corpus holds no such key), refused by `site` with the path it states.
    [[nodiscard]] Common::ResultStr<std::string>
    FindOverrideShaderNameByRef( const AssetManager& manager, const AssetGuidRef& ref, const AssetRefSite& site );

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
