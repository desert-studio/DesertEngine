#include <Engine/Assets/ControlRigAsset.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>

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

        // THE RIG'S IDENTITY IS ITS HEADER GUID (format 2), adopted HERE rather than in the load because the
        // asset manager keys its handle lookup at creation. A file with no readable header keeps the
        // path-derived handle - the load refuses a version-1 file by name, so none is ever READY under it.
        const Common::Content::AssetGuid guid = ReadTextHeaderGuid( m_Metadata.Filepath );
        if ( !guid.IsNull() )
            AdoptHandleFromFile( Common::UUID( static_cast<uint64_t>( Common::Content::HandleForGuid( guid ) ) ),
                                 Common::AssetHandle::StableKeyForPath( m_Metadata.Filepath ) );
    }

    Common::BoolResultStr ControlRigAsset::LoadFromFile()
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

        // THE FORWARDS SOLVE IS PART OF WHAT WAS LOADED, so it is part of the line that says what was
        // loaded. Without it the log cannot tell a rig whose graph the loader silently dropped from one
        // that never had a graph — which is the only observation a person makes on a headless run.
        LOG_INFO( "[Animation] Control rig '{}' loaded: {} controls, {} bone drives, {}.", m_DisplayName,
                  m_Data.Controls.size(), m_Data.Drives.size(),
                  m_Data.Graph.has_value()
                       ? fmt::format( "a forwards solve of {} node(s)", m_Data.Graph->Nodes.size() )
                       : std::string( "no forwards solve (each control's own composition drives its bone)" ) );
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

        LOG_INFO( "[Animation] Control rig written: '{}', {} controls, {} bone drives, {} graph node(s).",
                  filepath.string(), data.Controls.size(), data.Drives.size(),
                  data.Graph.has_value() ? data.Graph->Nodes.size() : 0U );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
