// "THERE IS ONE PLACE A PREFAB INSTANCE IS BUILT, AND ONE PLACE ITS ADDRESS IS SPELLED."
//
// WHY THIS IS A CENSUS AND NOT A TEST. Ю19 put two things behind single functions: the PLACEMENT rule
// (PrefabAsset::Instantiate is where it is applied, and it is what stops a UI prefab from being created
// outside a canvas where it would cover no pixels in silence) and the override ADDRESS
// (Assets::PrefabPathKey is the only spelling of "which record of which prefab is this entity"). Neither
// can be defended by a test of itself: a SECOND caller of the factory would skip the rule and still
// compile, link, and pass every suite in this repository. What has to be asserted is the SET OF FILES,
// and that is a property of the source text.
//
// AND THE PROJECT HAS PAID FOR EXACTLY THIS ALREADY, in this very directory. PrefabFactory's own comment
// records a hand-written second copy of the identity stitch that had drifted in three ways at once, and
// the reason it could drift was that nothing compiled either copy. That is the same defect one floor up.
//
// THE REGISTER NAMES ONE ROW PER PERMITTED FILE, WITH ITS REASON, AND THE COUNT IS DERIVED. A gate that
// pinned "three callers" could be satisfied by editing the number to four; a gate that pins WHICH THREE
// cannot. (Pinning a count is the failure recorded in `pin-a-register-not-a-count`; pinning one literal
// phrase and missing the same lie eleven lines down is its sibling.)
//
// The second half is a CORPUS SWEEP over every `.deprefab` on disk, tracked or not. Before Ю19 that
// corpus was EMPTY — zero prefab files and zero scenes naming one, which is precisely why a prefab
// instance could lose every edit below its root without anybody noticing. A gate over an empty corpus
// proves nothing, so the sweep exists to grow with the corpus rather than to pass today.

#include "../SettingConsumers/setting_consumers_reader.hpp"

#include <Engine/Assets/Prefab/PrefabFormat.hpp>
#include <Engine/Assets/Prefab/PrefabPlacement.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            const std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Assets/Prefab/PrefabAsset.hpp" );
            if ( probe )
            {
                return prefix;
            }
            prefix += "../";
        }
        return {};
    }

    std::string ReadAll( const fs::path& path )
    {
        const std::ifstream in( path, std::ios::binary );
        std::ostringstream  buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    std::vector<fs::path> ProjectSources( const std::string& root )
    {
        std::vector<fs::path> out;
        for ( const char* tree :
              { "Desert/Desert/Source", "Desert/Common/Source", "Editor/Source", "Runtime/Source" } )
        {
            std::error_code ec;
            const fs::path  base = fs::path( root ) / tree;
            for ( auto it = fs::recursive_directory_iterator( base, ec );
                  !ec && it != fs::recursive_directory_iterator(); ++it )
            {
                const fs::path& p = it->path();
                if ( p.extension() == ".cpp" || p.extension() == ".hpp" )
                {
                    out.push_back( p );
                }
            }
        }
        std::sort( out.begin(), out.end() );
        return out;
    }

    // Path relative to the repository root, with '/' separators, so a row can be named as a reader would
    // name it and the assertion message is a path somebody can open.
    std::string Relative( const std::string& root, const fs::path& p )
    {
        std::error_code ec;
        const fs::path  rel = fs::relative( p, fs::path( root ), ec );
        if ( ec || rel.empty() )
        {
            return p.string();
        }
        std::string s = rel.generic_string();
        return s;
    }

    // Which project files mention @p needle in CODE — comments and string literals are blanked first
    // with the shared reader, because this file and the prose around the rows would otherwise count
    // themselves, and a census that counts its own explanation is worthless.
    std::set<std::string> FilesNaming( const std::string& root, const std::string& needle )
    {
        std::set<std::string> out;
        for ( const fs::path& p : ProjectSources( root ) )
        {
            const std::string code = Desert::Tests::ConsumerText::StripCommentsAndLiterals( ReadAll( p ) );
            if ( code.find( needle ) != std::string::npos )
            {
                out.insert( Relative( root, p ) );
            }
        }
        return out;
    }

    struct Row
    {
        const char* File;
        const char* Why;
    };

    void ExpectExactly( const std::set<std::string>& found, const std::vector<Row>& permitted,
                        const std::string& needle )
    {
        for ( const Row& row : permitted )
        {
            EXPECT_TRUE( found.count( row.File ) == 1 )
                 << "the register names " << row.File << " as a caller of `" << needle << "` (" << row.Why
                 << ") and the tree no longer has one there. Either the row is stale — delete it and say "
                    "why in the commit — or the call was removed and something else now does that work.";
        }

        // Derived, never typed: the count is whatever the register holds.
        for ( const std::string& file : found )
        {
            const bool known = std::any_of( permitted.begin(), permitted.end(),
                                            [&]( const Row& row ) { return file == row.File; } );
            EXPECT_TRUE( known )
                 << file << " names `" << needle
                 << "` and is not in the register below. If it is a new legitimate caller, add a row WITH "
                    "ITS REASON. If it is a second way to do what the register's rows already do, that is "
                    "the defect this census exists to catch: see the header of this file.";
        }
        EXPECT_EQ( found.size(), permitted.size() );
    }
} // namespace

// --- ONE BUILDER ------------------------------------------------------------------------------------

TEST( PrefabInstantiationCensus, OnlyTheRegisteredFilesBuildAPrefabInstance )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not find the repository root from the test's working directory";

    const std::vector<Row> permitted = {
         { "Desert/Desert/Source/Engine/Runtime/Factory/PrefabFactory.cpp",
           "the factory itself, and its own recursion for a nested prefab" },
         { "Desert/Desert/Source/Engine/Assets/Prefab/PrefabAsset.cpp",
           "the ONE placement-checked entry point: it classifies the root, asks CheckPrefabPlacement, "
           "and attaches to the parent it was given" },
         { "Desert/Desert/Source/Engine/Core/Serialize/SceneSerializer.cpp",
           "the scene loader's pass 3. It does NOT go through PrefabAsset::Instantiate deliberately: the "
           "parent comes from the record's own `parent` link and is resolved against the entity map being "
           "built, and refusing a placement here would drop content out of a file that is already saved — "
           "a load must report, not veto" },
    };

    ExpectExactly( FilesNaming( root, "PrefabFactory::Instantiate" ), permitted, "PrefabFactory::Instantiate" );
}

// --- ONE ADDRESS ------------------------------------------------------------------------------------

TEST( PrefabInstantiationCensus, OnlyTheRegisteredFilesSpellAnOverrideAddress )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    // An override finds its entity by PrefabInstanceComponent::SourcePath. A fifth file comparing those
    // ids by hand — concatenating them, or matching only the last one — is the "middle link drops a
    // property" shape: both ends look right and the override lands on the wrong entity of the wrong
    // nested instance.
    //
    // THE NEEDLE IS THE COMPONENT'S NAME AND NOT `SourcePath`. The field name alone is carried by 23
    // files in this tree, almost all of them about an ASSET's source path (mesh import, texture cook) —
    // a census on it would be 19 rows of things that have nothing to do with prefabs, which is a gate
    // nobody can read and therefore a gate nobody maintains.
    const std::vector<Row> permitted = {
         { "Desert/Desert/Source/Engine/ECS/Components.hpp", "where the field is declared" },
         { "Desert/Desert/Source/Engine/Runtime/Factory/PrefabFactory.cpp",
           "stamps it while creating, and reads it back to index a finished instance" },
         { "Desert/Desert/Source/Engine/Core/Serialize/PrefabInstanceOverrides.cpp",
           "reads it to find the base record a live entity must be compared against" },
         { "Desert/Desert/Source/Engine/Assets/Prefab/PrefabAsset.cpp",
           "reads it so a record id survives \"Apply Instance Changes to Prefab\" instead of being "
           "renumbered, which would orphan every override in every scene" },
    };

    ExpectExactly( FilesNaming( root, "PrefabInstanceComponent" ), permitted, "PrefabInstanceComponent" );
}

// --- THE CORPUS -------------------------------------------------------------------------------------

TEST( PrefabInstantiationCensus, EveryPrefabOnDiskIsLoadableAndItsOverridesAddressSomething )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    std::error_code       ec;
    std::vector<fs::path> prefabs;
    for ( auto it = fs::recursive_directory_iterator( fs::path( root ), ec );
          !ec && it != fs::recursive_directory_iterator(); ++it )
    {
        const fs::path&   p = it->path();
        const std::string s = p.generic_string();
        // Agent worktrees and third-party checkouts live under the same root in this repository; a
        // sweep that walked them would be grading somebody else's tree.
        if ( s.find( "/ThirdParty/" ) != std::string::npos || s.find( "/.claude/" ) != std::string::npos ||
             s.find( "/build/" ) != std::string::npos )
        {
            it.disable_recursion_pending();
            continue;
        }
        if ( p.extension() == ".deprefab" )
        {
            prefabs.push_back( p );
        }
    }

    for ( const fs::path& p : prefabs )
    {
        const auto loadable = Desert::Assets::ParseLoadablePrefab( p.string(), ReadAll( p ) );
        ASSERT_TRUE( loadable ) << loadable.GetError();

        const Desert::Assets::PrefabData& tree = loadable.GetValue();
        ASSERT_FALSE( tree.Entities.empty() ) << p.string() << " holds no entity records";

        // The first record IS the root (PrefabAsset::CreateFromEntity writes the subtree pre-order), and
        // it is what the placement rule classifies. A file whose first record has no id cannot be
        // addressed by an override at all — PlanSceneStitch mints such a record a fresh id per load.
        EXPECT_TRUE( tree.Entities.front().id.has_value() )
             << p.string() << ": the root record states no id, so no override can ever address it";

        for ( const Desert::Assets::EntityData& record : tree.Entities )
        {
            if ( !record.PrefabOverrides.has_value() )
            {
                continue;
            }
            EXPECT_TRUE( record.PrefabPath.has_value() )
                 << p.string() << ": a record carries overrides but names no prefab to override";

            for ( const Desert::Assets::PrefabOverrideData& over : *record.PrefabOverrides )
            {
                EXPECT_FALSE( over.Path.empty() )
                     << p.string() << ": an override with an empty path addresses every entity and none";

                const bool statesSomething = over.Tag.has_value() || over.Translation.has_value() ||
                                             over.Rotation.has_value() || over.Scale.has_value() ||
                                             !over.Components.empty();
                EXPECT_TRUE( statesSomething )
                     << p.string()
                     << ": an override that states no field is a pinned copy of nothing — DiffPrefabEntity "
                        "returns nullopt for that case, so this file was not written by this engine";
            }
        }
    }

    // Said out loud rather than asserted: the number is the point of the sweep, and it was zero for the
    // whole life of the prefab subsystem.
    std::cout << "[census] .deprefab files swept: " << prefabs.size() << std::endl;
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
