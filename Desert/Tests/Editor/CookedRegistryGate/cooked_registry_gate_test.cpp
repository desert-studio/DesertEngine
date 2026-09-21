// THE COOK GATE — "on disk" stopped meaning "shipped", so this is what fails the build instead.
//
// ── WHY IT EXISTS, AND WHY IT EXISTS *NOW* ────────────────────────────────────────────────────────
//
// GAP_ANALYSIS T2.7 says a cook gate is needed "the same day T2.6 lands", and the author of the lazy
// loading slice wrote down exactly why it was not needed yet: he had LEFT the directory walk in place,
// so the walk still minted every handle and registered every shell, and the packager saw exactly what
// it had always seen. His sentence — "the danger opens together with T2.4, when the walk disappears".
//
// The walk has disappeared. Both hosts now boot from `Cooked/AssetRegistry.dreg` and neither reads a
// directory, so a content file that has no row in it is a file the engine does not have: it is not
// preloaded, it is not offered in a picker, and — the part that actually costs money — it is not in a
// packaged build, while the editor session that authored it looked completely normal.
//
// ── THE PRECEDENT, WHICH IS THE ARGUMENT FOR MAKING THIS ERROR RATHER THAN WARN ───────────────────
//
// `PackagedContentTrees.hpp` exists because a hand-typed sequence of `AddTreeToPak` calls forgot fonts
// and icons. `Constants.hpp` declared FONTS_PATH and ICONS_PATH, the runtime services scanned them,
// and the packager packed three other trees — so a built game contained NOT ONE `.ttf` and the first
// frame with text died. No test saw it, because the only thing that would have was packaging.
//
// That is this defect with a different list, so it gets the same answer the packager got: a relation
// asserted over the CONTENT TREE, not over a schema somebody maintains. Counted by walking the disk
// with the same census the loader reads rows back through.
//
// ── WHAT IS DELIBERATELY NOT ASSERTED ─────────────────────────────────────────────────────────────
//
// The DECLARED IDENTITY column and the DEPENDENCY EDGES. Both can only be read by parsing an asset
// with the class that owns its format, which is the engine, which this suite does not link — and a
// gate that cannot compute a value must not judge it. They converge on the editor's own post-boot
// cook; what this gate holds is the property a missing row destroys.

#include <Common/Content/ContentKinds.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Project/ProjectFormat.hpp>
#include <Common/Utilities/AssetRegistry.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>

namespace fs = std::filesystem;

namespace
{
    // The suite runs from build/Bin/Tests/<cfg>, so the repository is some way up. Probed by a file
    // that can only be this repository's.
    fs::path RepoRoot()
    {
        fs::path prefix = ".";
        for ( int up = 0; up < 8; ++up )
        {
            if ( fs::exists( prefix / "Editor" / "Desert.deproj" ) )
                return fs::absolute( prefix ).lexically_normal();
            prefix /= "..";
        }
        return {};
    }

    std::string LowerExtension( const fs::path& file )
    {
        std::string ext = file.extension().string();
        std::transform( ext.begin(), ext.end(), ext.begin(),
                        []( unsigned char c ) { return static_cast<char>( ::tolower( c ) ); } );
        return ext;
    }

    // Opens the sandbox project the way the editor opens it, INCLUDING the working directory. Engine
    // resource roots are never remapped by a project (Constants.hpp says so beside them), so
    // `Resources/Shaders/` resolves against the process's working directory and both hosts `cd` into
    // the directory that holds it before starting. A gate that did not would find no shaders and
    // certify a registry that is missing 76 rows.
    class SandboxProject
    {
    public:
        explicit SandboxProject( const fs::path& repoRoot )
            : m_SavedRoot( Common::Constants::Path::CurrentProjectRoot() ),
              m_SavedCwd( fs::current_path() )
        {
            const fs::path editorDir = repoRoot / "Editor";
            fs::current_path( editorDir );

            const auto json = Common::Utils::FileSystem::ReadFileContent( ( editorDir / "Desert.deproj" ).string() );
            if ( json )
            {
                if ( const auto project = Common::Project::ReadProjectFile( json.GetValue() ) )
                {
                    Common::Constants::Path::SetProjectRoot( editorDir, project.GetValue().AssetsRoot );
                    m_Opened = true;
                }
            }
        }

        ~SandboxProject()
        {
            Common::Constants::Path::SetProjectRoot( m_SavedRoot.ProjectDir, m_SavedRoot.AssetsRoot );
            std::error_code ec;
            fs::current_path( m_SavedCwd, ec );
        }

        bool Opened() const
        {
            return m_Opened;
        }

    private:
        Common::Constants::Path::ProjectRootState m_SavedRoot;
        fs::path                                  m_SavedCwd;
        bool                                      m_Opened = false;
    };

    struct DiskFile
    {
        std::string Kind;
        uint64_t    Size = 0;
    };

    // Every content file the engine would consider content, keyed the way it keys them. The census is
    // the SAME one `ContentRegistry` reads rows back through, which is why it lives in `Common`: a
    // gate with its own list of extensions would be the second list this whole task is about ending.
    std::map<std::string, DiskFile> WalkContent()
    {
        std::map<std::string, DiskFile> found;

        for ( std::size_t i = 0; i < Common::Content::CONTENT_KIND_COUNT; ++i )
        {
            const auto kind = static_cast<Common::Content::ContentKind>( i );
            const Common::Content::ContentKindSpec spec = Common::Content::KindSpec( kind );

            for ( const fs::path& candidate : Common::Utils::FileSystem::ListFilesRecursive( *spec.Root ) )
            {
                if ( LowerExtension( candidate ) != spec.Extension )
                    continue;

                const std::string key = Common::AssetHandle::StableKeyForPath( candidate );
                if ( key.empty() )
                    continue;

                found.emplace( key,
                               DiskFile{ std::string( spec.Name ),
                                         Common::Utils::FileSystem::GetFileSize( candidate ) } );
            }
        }
        return found;
    }
} // namespace

// 0. THE GATE CAN SEE WHAT IT CLAIMS TO CHECK. Without this every assertion below runs over an empty
// set and reports green — a census that found nothing wrong is byte-identical to one that found
// nothing at all.
TEST( CookedRegistryGate, TheGateCanSeeBothTheRegistryAndTheContentTree )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    SandboxProject project( root );
    ASSERT_TRUE( project.Opened() ) << "Editor/Desert.deproj could not be read";

    ASSERT_TRUE( fs::is_directory( Common::Constants::Path::SHADERDIR_PATH ) )
         << "engine resources do not resolve from the working directory this suite set, so the walk "
            "below would miss every shader and certify a registry that has none";

    EXPECT_FALSE( WalkContent().empty() )
         << "the content walk found no files at all, which cannot be true of this repository";

    EXPECT_TRUE( Common::Utils::FileSystem::Exists( Common::Utils::AssetRegistry::DefaultPath() ) )
         << "there is no cooked asset registry at " << Common::Utils::AssetRegistry::DefaultPath().string()
         << ". Since T2.4 neither host walks the content roots at boot, so a checkout without this file "
            "is a checkout with no content and a build of it would ship none. Run "
            "`cd Editor && ../build/Bin/Debug/AssetRegistryTool cook Desert.deproj` and commit it.";
}

// ── THE RELATION ────────────────────────────────────────────────────────────────────────────────────

TEST( CookedRegistryGate, EveryContentFileOnDiskHasARowInTheCommittedRegistry )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    SandboxProject project( root );
    ASSERT_TRUE( project.Opened() );

    auto loaded = Common::Utils::AssetRegistry::LoadFrom( Common::Utils::AssetRegistry::DefaultPath() );
    ASSERT_TRUE( loaded ) << loaded.GetError();

    const Common::Utils::AssetRegistry& registry = loaded.GetValue();
    const auto                          onDisk   = WalkContent();
    ASSERT_FALSE( onDisk.empty() );

    for ( const auto& [key, file] : onDisk )
    {
        const Common::Utils::AssetRegistryEntry* row = registry.FindByKey( key );
        ASSERT_NE( row, nullptr )
             << "'" << key << "' (" << file.Kind
             << ") is committed content and has no row in the cooked asset registry.\n"
                "Since the boot stopped walking the content roots this file DOES NOT REACH THE ENGINE: "
                "no preload, no picker, and — the expensive part — not one byte of it in a packaged "
                "build, while the editor session that added it looked entirely normal. This is the "
                "shape that shipped a game with no .ttf in it.\n"
                "Fix: `cd Editor && ../build/Bin/Debug/AssetRegistryTool cook Desert.deproj`, then "
                "commit Editor/Cooked/AssetRegistry.dreg.";

        EXPECT_EQ( row->Kind, file.Kind )
             << "'" << key << "' is recorded as a '" << row->Kind << "' and is a '" << file.Kind
             << "' on disk, so the loader will build it with the wrong asset class";
    }
}

TEST( CookedRegistryGate, EveryRowNamesAFileThatExists )
{
    // The other direction, and it is not symmetric with the one above: a row whose file is gone sends
    // the loader to a path it cannot read, once per boot, for ever, and the log line it produces names
    // a file the reader will not find — which reads as a broken engine rather than a stale registry.
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    SandboxProject project( root );
    ASSERT_TRUE( project.Opened() );

    auto loaded = Common::Utils::AssetRegistry::LoadFrom( Common::Utils::AssetRegistry::DefaultPath() );
    ASSERT_TRUE( loaded ) << loaded.GetError();

    const auto onDisk = WalkContent();
    for ( const Common::Utils::AssetRegistryEntry& row : loaded.GetValue().Entries() )
    {
        EXPECT_NE( onDisk.find( row.Key ), onDisk.end() )
             << "the registry has a row for '" << row.Key
             << "' and no such file is on disk. Re-cook and commit: `cd Editor && "
                "../build/Bin/Debug/AssetRegistryTool cook Desert.deproj`.";
    }
}

TEST( CookedRegistryGate, NoRowRecordsAStaleSize )
{
    // The SIZE is the one column a tool can check without parsing an asset, and it is what makes
    // "this row is current" answerable at all. A row whose size disagrees with the file means the file
    // was edited after the cook — which for a `.tex` or a `.demat` means the IDENTITY column may name a
    // number the file no longer carries, and that is a reference resolving to nothing.
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    SandboxProject project( root );
    ASSERT_TRUE( project.Opened() );

    auto loaded = Common::Utils::AssetRegistry::LoadFrom( Common::Utils::AssetRegistry::DefaultPath() );
    ASSERT_TRUE( loaded ) << loaded.GetError();

    const auto onDisk = WalkContent();
    for ( const Common::Utils::AssetRegistryEntry& row : loaded.GetValue().Entries() )
    {
        const auto file = onDisk.find( row.Key );
        if ( file == onDisk.end() )
            continue; // the test above owns that failure; reporting it twice would hide this one

        EXPECT_EQ( row.Size, file->second.Size )
             << "'" << row.Key << "' is recorded at " << row.Size << " bytes and is " << file->second.Size
             << " on disk, so the registry predates an edit to the file";
    }
}

// ── AND THE CENSUS THAT KEEPS THE GATE HONEST ───────────────────────────────────────────────────────

TEST( CookedRegistryGate, EveryContentKindIsRepresentedByTheShippedCorpus )
{
    // A GATE THAT CANNOT SEE A KIND CANNOT GUARD IT. If a content kind exists in the census and this
    // repository ships no file of it, then no run of the two relations above has ever exercised that
    // kind's root or extension — and a mistake in either (a root that does not exist, an extension
    // spelled without its dot) would sit there green until the day somebody authored the first file.
    //
    // This is a REPORT and not a demand that every kind have content: it fails only if a kind is
    // missing from the registry AND from the disk, which is the state where the gate is silently
    // covering nothing. It is the same argument as "a census must be able to name what it forbids".
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    SandboxProject project( root );
    ASSERT_TRUE( project.Opened() );

    auto loaded = Common::Utils::AssetRegistry::LoadFrom( Common::Utils::AssetRegistry::DefaultPath() );
    ASSERT_TRUE( loaded ) << loaded.GetError();

    const auto onDisk = WalkContent();

    std::set<std::string> kindsOnDisk;
    for ( const auto& [key, file] : onDisk )
        kindsOnDisk.insert( file.Kind );

    for ( std::size_t i = 0; i < Common::Content::CONTENT_KIND_COUNT; ++i )
    {
        const auto        kind = static_cast<Common::Content::ContentKind>( i );
        const std::string name( Common::Content::KindName( kind ) );

        EXPECT_NE( kindsOnDisk.find( name ), kindsOnDisk.end() )
             << "this repository ships no '" << name
             << "' file, so nothing has ever exercised that census row: its root ("
             << Common::Content::KindSpec( kind ).Root->string() << ") and its extension ("
             << Common::Content::KindSpec( kind ).Extension
             << ") could both be wrong and every run of this gate would still be green. Add one file of "
                "that kind to the repository, or remove the row.";

        EXPECT_FALSE( loaded.GetValue().OfKind( name ).empty() )
             << "the registry holds no '" << name << "' row while the disk does";
    }
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
