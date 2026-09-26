#include <Engine/Assets/AnimGraphAsset.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>
#include <Common/Content/CanonicalText.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

#include <filesystem>

namespace Desert::Assets
{
    AnimGraphAsset::AnimGraphAsset( AssetPriority priority, const Common::Filepath& filepath )
         : AssetBase( priority, filepath, AssetTypeID::AnimGraph )
    {
        m_DisplayName = m_Metadata.Filepath.stem().string();

        // THE GRAPH'S IDENTITY IS ITS HEADER GUID (ANGR 1, T7d), adopted HERE for ControlRigAsset's reason:
        // the asset manager keys its handle lookup at creation. A file with no readable header keeps the
        // path-derived handle - the load refuses it by name, so none is ever READY under it.
        if ( const TextAssetIdentity identity = ReadTextAssetIdentity( m_Metadata.Filepath );
             !identity.Guid.IsNull() )
            AdoptHandleFromFile( identity.Handle(), identity.StableKey() );
    }

    Common::BoolResultStr AnimGraphAsset::LoadFromFile()
    {
        // The old path of a moved asset reads the file where it now lives, through the registry - the same
        // file the constructor took the identity from (ReadTextAssetIdentity).
        const std::filesystem::path file = ContentRegistry::FileToOpen( m_Metadata.Filepath );
        const std::string           path = file.string();

        // Through the VFS first, so a packaged build reads the graph out of its .dpak exactly like every
        // other asset, then off the disk for a loose file the pak does not carry.
        std::string text;
        if ( const auto packed =
                  Common::Utils::VFS::Exists( file ) ? Common::Utils::VFS::ReadFile( file ) : std::nullopt;
             packed.has_value() )
        {
            text = packed.value();
        }
        else
        {
            if ( auto read = Common::Utils::FileSystem::ReadFileContent( file ); read )
            {
                text = read.ExtractValue();
            }
            // A failed read leaves `text` empty on purpose: the branch below is the one refusal that names
            // both shapes ("empty or could not be opened").
        }

        if ( text.empty() )
        {
            m_Graph.reset();
            return Common::MakeFormattedError<bool>( "anim graph '{}' is empty or could not be opened", path );
        }

        auto parsed = Animation::Graph::Deserialize( text );
        if ( !parsed )
        {
            m_Graph.reset();
            return Common::MakeFormattedError<bool>( "anim graph '{}' is not usable: {}", path,
                                                     parsed.GetError() );
        }

        // REPLACED, NOT ASSIGNED THROUGH. A hot reload must not mutate the object the running evaluators
        // were built from underneath them: they are re-synced by the revision below, on a frame boundary,
        // by AnimationECSSystem. Rewriting in place would change the graph mid-evaluation.
        m_Graph       = std::make_shared<Animation::Graph::AnimGraph>( parsed.ExtractValue() );
        m_DisplayName = m_Graph->Name.empty() ? m_Metadata.Filepath.stem().string() : m_Graph->Name;
        ++m_Revision;

        LOG_INFO( "[Animation] Anim graph '{}' loaded: {} state(s), {} parameter(s).", m_DisplayName,
                  m_Graph->States.size(), m_Graph->Parameters.size() );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr AnimGraphAsset::Unload()
    {
        m_Graph.reset();
        m_DisplayName = m_Metadata.Filepath.stem().string();
        // m_Revision is NOT reset, for ControlRigAsset's reason: it is monotonic per instance, and
        // rewinding it would make the next load look like no change at all to a consumer comparing them.
        return BOOLSUCCESS;
    }

    Common::BoolResultStr AnimGraphAsset::Save( const Common::Filepath&            filepath,
                                                const Animation::Graph::AnimGraph& graph )
    {
        std::error_code ec;
        if ( filepath.has_parent_path() )
        {
            std::filesystem::create_directories( filepath.parent_path(), ec );
        }

        // Atomic, for SaveControlRigFile's reason: a failed write must not cost the author the graph they
        // already had on disk.
        if ( const auto ok =
                  Common::Content::WriteCanonicalJsonFileAtomic( filepath, Animation::Graph::Serialize( graph ) );
             !ok )
        {
            return ok;
        }

        LOG_INFO( "[Animation] Anim graph written: '{}', {} state(s), {} parameter(s).", filepath.string(),
                  graph.States.size(), graph.Parameters.size() );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
