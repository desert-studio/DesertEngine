#include "ShaderAsset.hpp"

#include <Common/Content/ShaderAssetHeader.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Engine/Assets/TextAssetHeaderCheck.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>
#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <array>
#include <format>

namespace Desert::Assets
{
    ShaderAsset::ShaderAsset( AssetPriority priority, const Common::Filepath& filepath )
         : AssetBase( priority, filepath, GetTypeID() )
    {
        // THE SHADER'S IDENTITY IS ITS HEADER GUID (SHDR 1, T7j), adopted here for AnimGraphAsset's reason: the
        // asset manager keys its handle lookup at creation. A file with no readable header keeps the
        // path-derived handle - the load refuses it by name, so none is ever READY under it.
        const Common::Content::AssetGuid guid = ReadShaderHeaderGuid( m_Metadata.Filepath );
        if ( !guid.IsNull() )
            AdoptHandleFromFile( Common::UUID( static_cast<uint64_t>( Common::Content::HandleForGuid( guid ) ) ),
                                 Common::AssetHandle::StableKeyForPath( m_Metadata.Filepath ) );
    }

    Common::BoolResultStr ShaderAsset::LoadFromFile()
    {
        // A missing .shader file used to "load" as empty content and fail later, inside the
        // compiler, with a message that no longer named the file. Refuse here, with the path.
        auto raw = Common::Utils::FileSystem::ReadFileContent( m_Metadata.Filepath );
        if ( !raw )
            return Common::MakeError( raw.GetError() );
        m_ShaderContent = raw.ExtractValue();

        // A shader of generation 0 states no header: it has no identity to reference, so it is refused, not
        // compiled anyway. The comment line stays in the source the compiler sees - the DSL skips it.
        const std::string path   = m_Metadata.Filepath.string();
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
        const auto declared = Common::Content::ReadShaderDeclaredName( m_ShaderContent );
        if ( !declared )
            return Common::MakeError( std::format( "shader '{}': {}", path, declared.GetError() ) );
        if ( const std::string stem = m_Metadata.Filepath.stem().string(); declared.GetValue() != stem )
            return Common::MakeError( std::format( "shader '{}' declares Shader \"{}\" but its file is named '{}': "
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
        m_ReadyForUse = false;
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets