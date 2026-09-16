#include <Engine/Assets/ControlRigAsset.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

#include <filesystem>

namespace Desert::Assets
{
    ControlRigAsset::ControlRigAsset( AssetPriority priority, const Common::Filepath& filepath )
         : AssetBase( priority, filepath, AssetTypeID::ControlRig )
    {
        m_DisplayName = m_Metadata.Filepath.stem().string();
    }

    Common::BoolResultStr ControlRigAsset::Load()
    {
        const std::string path = m_Metadata.Filepath.string();

        // Through the VFS first, so a packaged build reads the rig out of its .dpak exactly like every
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
            return Common::MakeFormattedError<bool>( "control rig '{}' is empty or could not be opened", path );
        }

        auto parsed = Serialization::ParseControlRig( text );
        if ( !parsed )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "control rig '{}' is not usable: {}", path,
                                                     parsed.GetError() );
        }

        m_Data        = parsed.ExtractValue();
        m_DisplayName = m_Data.Name.empty() ? m_Metadata.Filepath.stem().string() : m_Data.Name;
        ++m_Revision;
        m_Ready = true;

        LOG_INFO( "[Animation] Control rig '{}' loaded: {} controls, {} bone drives.", m_DisplayName,
                  m_Data.Controls.size(), m_Data.Drives.size() );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr ControlRigAsset::Unload()
    {
        m_Data        = Serialization::ControlRigData{};
        m_DisplayName = m_Metadata.Filepath.stem().string();
        m_Ready       = false;
        // m_Revision is NOT reset, for UIThemeAsset's reason: it is monotonic per instance, and rewinding
        // it would make the next load look like no change at all to a consumer that compares revisions.
        return BOOLSUCCESS;
    }

    Common::BoolResultStr ControlRigAsset::Save( const Common::Filepath&              filepath,
                                                 const Serialization::ControlRigData& data )
    {
        std::error_code ec;
        if ( filepath.has_parent_path() )
        {
            std::filesystem::create_directories( filepath.parent_path(), ec );
        }

        // SaveControlRigFile validates before it writes and goes through the atomic write primitive, so a
        // rejected rig never reaches disk and a failed write cannot cost the author the rig they had.
        if ( const auto ok = Serialization::SaveControlRigFile( filepath, data ); !ok )
        {
            return ok;
        }

        LOG_INFO( "[Animation] Control rig written: '{}', {} controls, {} bone drives.", filepath.string(),
                  data.Controls.size(), data.Drives.size() );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
