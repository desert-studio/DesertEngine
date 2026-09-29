#include "ShaderAsset.hpp"

#include <Common/Content/ShaderAssetHeader.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/TextAssetHeaderCheck.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>
#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <array>
#include <format>
#include <utility>
#include <vector>

namespace Desert::Assets
{
    ShaderAsset::ShaderAsset( const Common::Filepath& filepath ) : AssetBase( filepath, GetTypeID() )
    {
        // THE SHADER'S IDENTITY IS ITS HEADER GUID (SHDR 1, T7j), adopted here for AnimGraphAsset's reason: the
        // asset manager keys its handle lookup at creation. A file with no readable header keeps the
        // path-derived handle - the load refuses it by name, so none is ever READY under it. Read through
        // ReadTextAssetIdentity like every header-GUID kind, so an old path after a move is the moved shader.
        const TextAssetIdentity identity = ReadTextAssetIdentity( m_Metadata.Filepath, &ReadShaderHeaderGuid );
        if ( !identity.Guid.IsNull() )
            AdoptHandleFromFile( identity.Handle(), identity.StableKey() );
    }

    Common::BoolResultStr ShaderAsset::LoadFromFile()
    {
        // A missing .shader file used to "load" as empty content and fail later, inside the
        // compiler, with a message that no longer named the file. Refuse here, with the path.
        // The file the constructor took the identity from (ReadTextAssetIdentity): past a move's redirector.
        const std::filesystem::path file = ContentRegistry::FileToOpen( m_Metadata.Filepath );
        auto                        raw  = Common::Utils::FileSystem::ReadFileContent( file );
        if ( !raw )
            return Common::MakeError( raw.GetError() );
        m_ShaderContent = raw.ExtractValue();

        // A shader of generation 0 states no header: it has no identity to reference, so it is refused, not
        // compiled anyway. The comment line stays in the source the compiler sees - the DSL skips it.
        const std::string path   = file.string();
        const auto        header = Common::Content::ReadShaderHeader( m_ShaderContent );
        if ( !header )
            return Common::MakeError(
                 std::format( "shader '{}' ({}): this build reads SHDR {}, a first-line header "
                              "with a GUID - run scripts/Dev/migrate.sh --write over it once",
                              path, header.GetError(), kShaderSchemaVersion ) );
        const std::array<Common::Content::SubsystemVersion, 1> subsystems = {
             Common::Content::SubsystemVersion{ kShaderSchemaTag, kShaderSchemaVersion } };
        if ( const auto checked = CheckStatedHeader( header.GetValue(), Common::Content::ContentKind::Shader,
                                                     kShaderSchemaTag, kShaderSchemaVersion, subsystems );
             !checked )
            return Common::MakeError( std::format( "shader '{}': {}", path, checked.GetError() ) );

        // The runtime names this shader by its file stem, and a material's shader GUID resolves to that stem
        // (SurfaceMaterialAsset::ResolveDependencies). A DSL name that differs would make the file say one
        // name and the engine bind another, so it is refused here rather than resolved either way.
        const auto manifest = Common::Content::ReadShaderManifest( m_ShaderContent );
        if ( !manifest )
            return Common::MakeError( std::format( "shader '{}': {}", path, manifest.GetError() ) );
        m_Role           = manifest.GetValue().Role;
        m_DefaultSurface = manifest.GetValue().DefaultSurface;

        const auto declared = Common::Content::ReadShaderDeclaredName( m_ShaderContent );
        if ( !declared )
            return Common::MakeError( std::format( "shader '{}': {}", path, declared.GetError() ) );
        if ( const std::string stem = file.stem().string(); declared.GetValue() != stem )
            return Common::MakeError(
                 std::format( "shader '{}' declares Shader \"{}\" but its file is named '{}': "
                              "rename one so they agree",
                              path, declared.GetValue(), stem ) );

        m_ReadyForUse = true;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr ShaderAsset::Unload()
    {
        // WAS `return BOOLSUCCESS;`, which kept both the whole `.shader` source text and the ready flag.
        // The flag half is the dangerous one — see AssetBase::Unload, rule 2.
        //
        // NOTE FOR ANYONE EVICTING THESE: the compiled `Graphic::Shader` — the VkShaderModules, the
        // descriptor set layouts and the pools — is the ShaderService's, not this asset's, and dropping
        // the source text does not touch it. That is correct and deliberate: a pipeline built from a
        // shader outlives the text it was compiled from.
        m_ShaderContent.clear();
        m_ShaderContent.shrink_to_fit();
        m_Role.clear();
        m_DefaultSurface = false;
        m_ReadyForUse = false;
        return BOOLSUCCESS;
    }

    Common::ResultStr<AssetGuidRef> FindShaderRefByHandle( const AssetManager& manager, Common::AssetHandle shader,
                                                           const AssetRefSite& site )
    {
        const auto asset = manager.FindByHandle<ShaderAsset>( shader );
        if ( asset == nullptr )
            return Common::MakeError<AssetGuidRef>( std::format( "{} on {}: no loaded shader has handle {}",
                                                                 site.Field, site.Context,
                                                                 static_cast<uint64_t>( shader ) ) );
        const Common::Filepath& file = asset->GetMetadata().Filepath;
        return WriteAssetGuidRef( ReadShaderHeaderGuid( file ), file, site );
    }

    std::optional<Common::AssetHandle> FindShaderHandleByCompileName( const AssetManager& manager,
                                                                      std::string_view    compileName )
    {
        for ( const auto& [handle, shader] : manager.FindAllByType<ShaderAsset>() )
            if ( shader->GetMetadata().Filepath.stem().string() == compileName )
                return handle;
        return std::nullopt;
    }

    bool IsPBRSurfaceTemplate( const AssetManager& manager, Common::AssetHandle shader )
    {
        const auto asset = manager.FindByHandle<ShaderAsset>( shader );
        return asset != nullptr && asset->GetRole() == Common::Content::kPBRSurfaceRole;
    }

    Common::ResultStr<std::string> FindOverrideShaderNameByRef( const AssetManager& manager, const AssetGuidRef& ref,
                                                                const AssetRefSite& site )
    {
        auto name = FindShaderNameByRef( manager, ref, site );
        if ( !name )
            return name;
        const auto guid = Common::Content::AssetGuidFromText( ref.Guid );
        if ( guid && IsPBRSurfaceTemplate( manager, Common::AssetHandle( static_cast<uint64_t>(
                                                         Common::Content::HandleForGuid( guid.GetValue() ) ) ) ) )
            return Common::MakeError<std::string>(
                 std::format( "{} on {}: '{}' (GUID {}) is the PBRSurface template, which overrides nothing - the "
                              "mesh draws its material slots; SceneMigrator v41 removes the key",
                              site.Field, site.Context, ref.Path, ref.Guid ) );
        return name;
    }

    Common::ResultStr<AssetGuidRef> FindShaderRefByName( const AssetManager& manager, std::string_view name,
                                                         const AssetRefSite& site )
    {
        for ( const auto& [handle, shader] : manager.FindAllByType<ShaderAsset>() )
        {
            const Common::Filepath& file = shader->GetMetadata().Filepath;
            if ( file.stem().string() == name )
                return WriteAssetGuidRef( ReadShaderHeaderGuid( file ), file, site );
        }
        return Common::MakeError<AssetGuidRef>(
             std::format( "{} on {}: no loaded shader is named '{}'", site.Field, site.Context, name ) );
    }

    Common::ResultStr<std::string> FindShaderNameByRef( const AssetManager& manager, const AssetGuidRef& ref,
                                                        const AssetRefSite& site )
    {
        // BY GUID, the shader's identity (the constructor adopts HandleForGuid of its header GUID).
        return ResolveAssetGuidRef(
             ref,
             [&manager]( const Common::Content::AssetGuid& guid ) -> std::optional<std::string>
             {
                 const auto shader = manager.FindByHandle<ShaderAsset>(
                      Common::AssetHandle( static_cast<uint64_t>( Common::Content::HandleForGuid( guid ) ) ) );
                 if ( shader == nullptr )
                     return std::nullopt;
                 return shader->GetMetadata().Filepath.stem().string();
             },
             site );
    }

    namespace
    {
        template <class Pred>
        Common::ResultStr<Common::AssetHandle> ExactlyOneTemplate( const AssetManager& manager, Pred declares,
                                                                   std::string_view what )
        {
            std::vector<std::pair<Common::AssetHandle, std::string>> found;
            for ( const auto& [handle, shader] : manager.FindAllByType<ShaderAsset>() )
                if ( shader->IsReadyForUse() && declares( *shader ) )
                    found.emplace_back( handle, shader->GetMetadata().Filepath.generic_string() );
            if ( found.size() == 1 )
                return Common::MakeSuccess( found.front().first );
            std::string paths;
            for ( const auto& [handle, path] : found )
                paths += std::format( "{}'{}'", paths.empty() ? "" : ", ", path );
            return Common::MakeError<Common::AssetHandle>(
                 found.empty()
                      ? std::format( "no loaded shader declares {}", what )
                      : std::format( "{} shaders declare {} — exactly one may: {}", found.size(), what, paths ) );
        }
    } // namespace

    Common::ResultStr<Common::AssetHandle> FindTemplateByRole( const AssetManager& manager, std::string_view role )
    {
        return ExactlyOneTemplate(
             manager, [role]( const ShaderAsset& shader ) { return shader.GetRole() == role; },
             std::format( "'Role {}'", role ) );
    }

    Common::ResultStr<Common::AssetHandle> FindDefaultSurfaceTemplate( const AssetManager& manager,
                                                                       std::string_view    projectOverride,
                                                                       std::string_view    deprojPath )
    {
        if ( !projectOverride.empty() )
        {
            const auto guid = Common::Content::AssetGuidFromText( projectOverride );
            if ( !guid )
                return Common::MakeError<Common::AssetHandle>( std::format(
                     "'{}': DefaultSurfaceTemplate is not a shader GUID ({})", deprojPath, guid.GetError() ) );
            const Common::AssetHandle handle(
                 static_cast<uint64_t>( Common::Content::HandleForGuid( guid.GetValue() ) ) );
            if ( manager.FindByHandle<ShaderAsset>( handle ) == nullptr )
                return Common::MakeError<Common::AssetHandle>( std::format(
                     "'{}': DefaultSurfaceTemplate {} names no loaded shader", deprojPath, projectOverride ) );
            return Common::MakeSuccess( handle );
        }
        return ExactlyOneTemplate(
             manager, []( const ShaderAsset& shader ) { return shader.IsDefaultSurface(); }, "'Default Surface'" );
    }
} // namespace Desert::Assets