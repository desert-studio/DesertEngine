#include "ProjectContext.hpp"

#include <Common/Core/Core.hpp> // DESERT_VERIFY
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Constants.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <optional>

namespace Desert::Project
{
    namespace
    {
        std::optional<ProjectFile> s_Current;
        std::string                s_FilePath;

        std::string RegistryFile( const std::string& configDirectory )
        {
            return ( std::filesystem::path( configDirectory ) / "projects.json" ).string();
        }
    } // namespace

    std::string ProjectContext::ConfigDirectory()
    {
        const char* home = std::getenv( "HOME" );
#ifdef DESERT_PLATFORM_WINDOWS
        if ( !home )
            home = std::getenv( "USERPROFILE" );
#endif
        std::filesystem::path dir = std::filesystem::path( home ? home : "." ) / ".desertengine";
        std::error_code       ec;
        std::filesystem::create_directories( dir, ec );
        return dir.string();
    }

    bool ProjectContext::Open( const std::string& deprojPath, RecordInRecent record )
    {
        // Disk first (dev, loose .deproj), else a packaged game serves the descriptor from a mounted .dpak.
        if ( !Common::Utils::FileSystem::Exists( deprojPath ) )
        {
            LOG_ERROR( "[Project] File not found: {}", deprojPath );
            return false;
        }
        const bool onDisk = std::filesystem::exists( deprojPath );

        // An empty .deproj is as unusable as an unreadable one — both refuse here, before the parse.
        const auto rawRead = Common::Utils::FileSystem::ReadFileContent( deprojPath );
        if ( !rawRead || rawRead.GetValue().empty() )
        {
            LOG_ERROR( "[Project] Cannot read {}", deprojPath );
            return false;
        }

        auto parsed = Common::Project::ReadProjectFile( rawRead.GetValue() );
        if ( !parsed.IsSuccess() )
        {
            LOG_ERROR( "[Project] {}: {}", deprojPath, parsed.GetError() );
            return false;
        }

        s_Current  = parsed.ExtractValue();
        s_FilePath = std::filesystem::absolute( deprojPath ).string();

        // THE decoupling step: point every engine content path (and the Cooked/ cache) at this project.
        // Must happen before any subsystem reads the constants — callers open the project while parsing
        // --project, before the engine spins up.
        const std::filesystem::path projectDir = std::filesystem::path( s_FilePath ).parent_path();
        Common::Constants::Path::SetProjectRoot( projectDir, s_Current->AssetsRoot );

        // Make sure the standard content folders exist (a freshly created project has only a few). Skipped for
        // a packaged game (opened from a read-only .dpak) — its content lives in the archive, not on disk.
        if ( onDisk )
        {
            // The census lives beside the format (desert-shared ProjectFormat.hpp) — the same rows the
            // launcher scaffolds a new project from, so "what a project has" cannot fork between creator
            // and opener. ASSETS_PATH was just remapped above, so each row lands inside this project;
            // Tests/Engine/ProjectFormat asserts every row equals the Constants::Path global it answers to.
            std::error_code ec;
            for ( const std::string_view folder : Common::Project::StandardContentFolders )
                std::filesystem::create_directories( Common::Constants::Path::ASSETS_PATH / folder, ec );
        }

        // Two independent reasons to stay out of the registry, and they are not the same reason: a
        // packaged game reads its descriptor out of a mounted .dpak and is not on this machine's
        // disk at all, and a headless capture run is on disk but is not a person opening a project.
        if ( onDisk && record == RecordInRecent::Yes )
            RegisterRecent( ConfigDirectory(), s_FilePath );
        LOG_INFO( "[Project] Opened '{}' ({}) — assets root: {}", s_Current->Name, s_FilePath,
                  Common::Constants::Path::ASSETS_PATH.string() );
        return true;
    }

    bool ProjectContext::Save()
    {
        if ( !s_Current || s_FilePath.empty() )
            return false;
        // ENGINEVERSION IS NOT TOUCHED HERE, AND THAT IS THE WHOLE OF К4.
        //
        // It used to be stamped with Common::Version::Full() on every save — a string carrying THIS
        // MACHINE's commit hash and its `.dirty` flag, written into a file git tracks and the whole team
        // shares. So the field meant "whichever developer last happened to pick a startup scene", every
        // one of them wrote a different value, and the churn travelled in commits. Its own header called
        // it the input to a collection compatibility check; that check reads Common::Version::CommitCount()
        // and has never read this. Nothing read it at all.
        //
        // The field now means what the launcher already writes into it: THE ENGINE THE PROJECT WAS
        // CREATED WITH. That is a fact about the project rather than about a machine, it is stable, it
        // belongs in a shared file by the §6 procedure, and the launcher's tile keeps the line it draws.
        // Deleting the field instead would have thrown away a real answer to "which version was this made
        // in" and cost more to reinstate later than leaving it correct costs now.
        //
        // THE KEYS THIS BUILD CANNOT NAME ARE TAKEN FROM THE FILE AT THE MOMENT OF WRITING, not
        // remembered from the moment of opening (К11, and the same reasoning as К9 for editor.json).
        // `s_Current` is parsed once by Open() and held for the whole session, so an editor that
        // started before a key existed carries a ForeignKeys that has never heard of it — and would
        // delete it here. The launcher's settings screen writes this same file from another process
        // while the editor is up, which is exactly when that happens.
        //
        // These two paragraphs are ONE decision seen from both ends, which is why they sit together:
        // this build stops asserting a fact it has no business asserting (the version), and stops
        // discarding facts it cannot read (everyone else's keys). Both are the same rule — write what
        // you own, carry what you do not.
        //
        // KEYS ONLY, NEVER VALUES. Every field this struct declares is written from what this
        // session has, so two writers still resolve a real disagreement last-writer-wins; only the
        // leftovers are adopted. A file that cannot be re-read is not a reason to refuse the save —
        // the write below replaces it wholesale anyway — but it IS a reason to say so, because the
        // leftovers are then this session's, which may be older than the disk's.
        if ( const auto raw = Common::Utils::FileSystem::ReadFileContent( s_FilePath ); raw )
        {
            if ( auto onDisk = Common::Project::ReadProjectFile( raw.GetValue() ); onDisk.IsSuccess() )
                s_Current->UnknownKeys = std::move( onDisk.ExtractValue().UnknownKeys );
            else
                LOG_WARN( "[Project] {} could not be re-read before saving ({}), so any key written "
                          "into it by another program since this project was opened is not carried "
                          "across.",
                          s_FilePath, onDisk.GetError() );
        }

        // Atomic (write-then-rename), because the .deproj is the one file without which the project
        // does not open at all: the plain primitive truncates in place, so a write interrupted half
        // way used to leave zero bytes where the descriptor was.
        if ( !Common::Utils::FileSystem::WriteContentToFileAtomic(
                  std::filesystem::path( s_FilePath ), Common::Project::WriteProjectFile( *s_Current ) ) )
        {
            LOG_ERROR( "[Project] Could not save {} — the file on disk is unchanged", s_FilePath );
            return false;
        }
        LOG_INFO( "[Project] Saved {}", s_FilePath );
        return true;
    }

    bool ProjectContext::SetDefaultScene( const std::string& sceneRelPath )
    {
        if ( !s_Current )
            return false;
        s_Current->DefaultScene = sceneRelPath;
        return Save();
    }

    bool ProjectContext::HasProject()
    {
        return s_Current.has_value();
    }

    const ProjectFile& ProjectContext::Current()
    {
        // SAID OUT LOUD, because the alternative is undefined behaviour that reads as a corrupt project
        // file. Twenty-one call sites take this reference and HasProject() is a separate question none of
        // them is obliged to ask; before this line, calling Current() before a project was opened
        // dereferenced an empty optional and carried on with whatever was in that storage.
        DESERT_VERIFY( s_Current.has_value(),
                       "ProjectContext::Current() before a project was opened — ask HasProject() first" );
        return *s_Current;
    }

    std::string ProjectContext::Directory()
    {
        return s_FilePath.empty() ? std::string()
                                  : std::filesystem::path( s_FilePath ).parent_path().string();
    }

    std::string ProjectContext::FilePath()
    {
        return s_FilePath;
    }

    std::string ProjectContext::DefaultScenePath()
    {
        if ( !s_Current || s_Current->DefaultScene.empty() )
            return {};
        return ( std::filesystem::path( Directory() ) / s_Current->DefaultScene ).string();
    }

    Common::ResultStr<Common::Project::ProjectsRegistry>
    ProjectContext::RecentProjects( const std::string& configDirectory )
    {
        const std::string file = RegistryFile( configDirectory );

        // No registry yet is a fresh machine, not a failure — and it is the ONLY case that answers
        // "empty" successfully.
        if ( !std::filesystem::exists( file ) )
            return Common::MakeSuccess( Common::Project::ProjectsRegistry{} );

        const auto raw = Common::Utils::FileSystem::ReadFileContent( file );
        if ( !raw )
            return Common::MakeFormattedError<Common::Project::ProjectsRegistry>(
                 "{} exists but could not be read: {}", file, raw.GetError() );
        if ( raw.GetValue().empty() )
            return Common::MakeSuccess( Common::Project::ProjectsRegistry{} );

        auto parsed = Common::Project::ReadProjectsRegistry( raw.GetValue() );
        if ( !parsed.IsSuccess() )
            return Common::MakeFormattedError<Common::Project::ProjectsRegistry>( "{}: {}", file,
                                                                                  parsed.GetError() );
        // The reader migrates a registry from before LastOpened; the write below then puts the
        // current shape on disk, so the file upgrades the first time any project is opened.
        return Common::MakeSuccess( parsed.ExtractValue() );
    }

    void ProjectContext::RegisterRecent( const std::string& configDirectory, const std::string& deprojPath )
    {
        // READ-MODIFY-WRITE, AND THE READ IS HERE. projects.json has two writers in two programs —
        // this one and Tools/ProjectHub — and neither arbitrates, so the only thing that keeps a
        // write from erasing the other side's records is how OLD the copy being written is. This
        // side re-reads immediately before writing; the launcher does the same through
        // Hub::MutateProjects. Neither holds a session-long snapshot any more.
        //
        // The policy — most recent first, unique, no cap, LastOpened stamped — is ONE function in
        // desert-shared, called by this side and by the launcher. It used to be two copies over
        // one shared file, which is how both of them ended up carrying the same silent cap of ten:
        // lifting either alone would have had the other erase what it kept.
        auto onDisk = RecentProjects( configDirectory );
        if ( !onDisk.IsSuccess() )
        {
            // NOTHING IS WRITTEN. The registry is unreadable, so this process cannot know what it
            // would be overwriting — and it used to write anyway, over a registry it had silently
            // read as empty, which turned one bad byte into "all my projects are gone".
            LOG_ERROR( "[Project] The recent-projects registry was not updated with '{}': {}. The file "
                       "on disk is unchanged.",
                       deprojPath, onDisk.GetError() );
            return;
        }

        Common::Project::ProjectsRegistry registry = onDisk.ExtractValue();
        Common::Project::PromoteRecent( registry, deprojPath, Common::Project::UnixNow() );

        // Atomic (write-then-rename): this file is shared with the Project Hub, and an interrupted
        // in-place write left a torn projects.json that neither side could parse — every recent
        // project gone over one crash at the wrong moment. On failure the registry simply keeps its
        // previous list, which is the right outcome for a convenience file: name it and move on.
        const std::string file = RegistryFile( configDirectory );
        if ( !Common::Utils::FileSystem::WriteContentToFileAtomic(
                  std::filesystem::path( file ), Common::Project::WriteProjectsRegistry( registry ) ) )
            LOG_ERROR( "[Project] Could not update the recent-projects registry {} — it keeps its "
                       "previous contents",
                       file );
    }
} // namespace Desert::Project
