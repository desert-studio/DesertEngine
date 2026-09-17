#include "ShaderAsset.hpp"

#include <Common/Utilities/FileSystem.hpp>

namespace Desert::Assets
{
    ShaderAsset::ShaderAsset( AssetPriority priority, const Common::Filepath& filepath )
         : AssetBase( priority, filepath, GetTypeID() )
    {
    }

    Common::BoolResultStr ShaderAsset::LoadFromFile()
    {
        // A missing .shader file used to "load" as empty content and fail later, inside the
        // compiler, with a message that no longer named the file. Refuse here, with the path.
        auto raw = Common::Utils::FileSystem::ReadFileContent( m_Metadata.Filepath );
        if ( !raw )
            return Common::MakeError( raw.GetError() );
        m_ShaderContent = raw.ExtractValue();

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