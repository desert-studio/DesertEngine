#include <Engine/Assets/ShaderGraphAsset.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

#include <filesystem>

namespace Desert::Assets
{
    ShaderGraphAsset::ShaderGraphAsset( AssetPriority priority, const Common::Filepath& filepath )
         : AssetBase( priority, filepath, AssetTypeID::ShaderGraph )
    {
        m_DisplayName = m_Metadata.Filepath.stem().string();
    }

    Common::BoolResultStr ShaderGraphAsset::LoadFromFile()
    {
        const std::string path = m_Metadata.Filepath.string();

        // Through the VFS first, so a packaged build reads the graph out of its .dpak exactly like every
        // other asset, then off the disk for a loose file the pak does not carry.
        std::string text;
        if ( const auto packed = Common::Utils::VFS::Exists( m_Metadata.Filepath )
                                      ? Common::Utils::VFS::ReadFile( m_Metadata.Filepath )
                                      : std::nullopt;
             packed.has_value() )
        {
            text = packed.value();
        }
        else
        {
            if ( auto read = Common::Utils::FileSystem::ReadFileContent( m_Metadata.Filepath ); read )
            {
                text = read.ExtractValue();
            }
            // A failed read leaves `text` empty on purpose: the branch below is the one refusal that names
            // both shapes ("empty or could not be opened").
        }

        if ( text.empty() )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "shader graph '{}' is empty or could not be opened", path );
        }

        auto parsed = Serialization::ShaderGraph::ParseShaderGraph( text );
        if ( !parsed )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "shader graph '{}' is not usable: {}", path,
                                                     parsed.GetError() );
        }

        m_Data        = parsed.ExtractValue();
        m_DisplayName = m_Data.Name.empty() ? m_Metadata.Filepath.stem().string() : m_Data.Name;
        ++m_Revision;
        m_Ready = true;

        LOG_INFO( "[ShaderGraph] '{}' loaded: {} node(s), {} link(s).", m_DisplayName, m_Data.Nodes.size(),
                  m_Data.Links.size() );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr ShaderGraphAsset::Unload()
    {
        m_Data        = Serialization::ShaderGraph::Document{};
        m_DisplayName = m_Metadata.Filepath.stem().string();
        m_Ready       = false;
        // m_Revision is NOT reset, for ControlRigAsset's reason: it is monotonic per instance, and
        // rewinding it would make the next load look like no change at all to a consumer comparing them.
        return BOOLSUCCESS;
    }

    Common::BoolResultStr ShaderGraphAsset::Save( const Common::Filepath&                     filepath,
                                                  const Serialization::ShaderGraph::Document& doc )
    {
        std::error_code ec;
        if ( filepath.has_parent_path() )
        {
            std::filesystem::create_directories( filepath.parent_path(), ec );
        }

        // Atomic, for SaveControlRigFile's reason: a failed write must not cost the author the graph they
        // already had on disk.
        if ( const auto ok = Common::Utils::FileSystem::WriteContentToFileAtomic(
                  filepath, Serialization::ShaderGraph::Serialize( doc ) );
             !ok )
        {
            return ok;
        }

        LOG_INFO( "[ShaderGraph] written: '{}', {} node(s), {} link(s).", filepath.string(), doc.Nodes.size(),
                  doc.Links.size() );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
