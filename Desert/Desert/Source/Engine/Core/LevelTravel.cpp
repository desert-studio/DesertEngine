#include "LevelTravel.hpp"

#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Engine/Project/ProjectContext.hpp>

#include <filesystem>

namespace Desert::Core
{
    Common::ResultStr<std::string> ResolveLevelPath( std::string_view level )
    {
        std::string path;
        if ( level.empty() )
        {
            path = Project::ProjectContext::DefaultScenePath();
            if ( path.empty() )
                return Common::MakeError<std::string>(
                     "OpenLevel names no level and the project has no default map (DefaultScene in the "
                     ".deproj, set in Build Settings)" );
        }
        else
        {
            const std::filesystem::path named( level );
            if ( named.is_absolute() )
                path = named.string();
            else if ( Project::ProjectContext::HasProject() )
                path = ( std::filesystem::path( Project::ProjectContext::Directory() ) / named ).string();
            else
                return Common::MakeFormattedError<std::string>(
                     "OpenLevel('{}') refused: a relative level name is relative to the project folder and no "
                     "project is open",
                     level );
        }

        if ( !Common::Utils::FileSystem::Exists( path ) ) // VFS-aware: a packaged game reads its pak
            return Common::MakeFormattedError<std::string>( "OpenLevel('{}') refused: '{}' does not exist", level,
                                                            path );
        return Common::MakeSuccess( std::move( path ) );
    }

    LevelTravel& LevelTravel::Get()
    {
        static LevelTravel instance;
        return instance;
    }

    Common::BoolResultStr LevelTravel::Open( std::string_view level )
    {
        auto resolved = ResolveLevelPath( level );
        if ( !resolved )
        {
            LOG_ERROR( "[Level] {}", resolved.GetError() );
            return Common::MakeError<bool>( resolved.GetError() );
        }
        if ( m_Pending )
            LOG_INFO( "[Level] Travel to '{}' replaces the queued '{}'", resolved.GetValue(), *m_Pending );
        m_Pending = resolved.ExtractValue();
        LOG_INFO( "[Level] Travel queued: '{}' (applied at the next frame boundary)", *m_Pending );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr
    LevelTravel::TickTravel( const std::function<Common::BoolResultStr( const std::string& )>& load )
    {
        if ( !m_Pending )
            return Common::MakeSuccess( false );
        // Taken BEFORE the load runs, so a request the new level makes while loading is a NEW pending
        // travel for the next boundary, not the one being applied.
        const std::string path = std::move( *m_Pending );
        m_Pending.reset();
        if ( auto loaded = load( path ); !loaded )
            return loaded;
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Core
