// A `.demat`'s MaterialId IS its asset handle, and the editor addresses a document BY that handle.
//
// So two files carrying one MaterialId are not a tidiness problem, they are a wrong-asset problem:
// whichever registers second wins AssetManager::m_HandleLookup and MaterialService's maps, the Edit
// button and a double-click on either file open the same document, and the other material can never be
// resolved at all. `MP_LitConst.demat` and `MP_HandUnlit.demat` both carried 6666666666666666666, and a
// developer found it by having the Material Editor open somebody else's material in front of him.
//
// The collision stayed invisible for as long as a material was chosen from a combo box BY NAME. Nothing
// about the ids changed when documents became handle-addressed; what changed is that the defect acquired
// a symptom. That is the argument for asserting the property in a test rather than remembering it.
//
// Three things are asserted here:
//
//   1. No two shipped `.demat` share a MaterialId, and every one of them states one.
//   2. Every MaterialGuid a shipped scene names resolves to exactly ONE `.demat` — the relation between
//      the two sides, not either side alone (the defect class this project keeps finding).
//   3. The rule MaterialService refuses on, driven directly, including the cases that must NOT refuse.

#include <gtest/gtest.h>

#include <Engine/Assets/MaterialData.hpp>
#include <Engine/Runtime/Services/Material/MaterialIdentity.hpp>

// Same serialization environment as SurfaceMaterialAsset.cpp: glm/UUID adapters + json backend.
#include <Common/Core/Serialization/GlmReflection.hpp>
#include <rflcpp/rfl/json.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using Desert::Assets::MaterialData;
using Desert::Runtime::IsMaterialIdentityCollision;

namespace
{
    // Walks up from the working directory looking for a file only the repository has. Copied in shape
    // from Desert/Tests/Engine/CloudProtocolScene, which needs the same thing for the same reason: the
    // test runner's working directory is not fixed.
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Core/SceneSettings.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadAll( const std::filesystem::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // A scene the REPOSITORY ships, as opposed to one this machine happens to have on disk.
    // `Scenes/Autosave/` holds the editor's gitignored crash-recovery copies (.gitignore:98). They are
    // never migrated and are not content, so a sweep that descends into them passes in CI — where a
    // fresh checkout has no such directory — and fails on any machine that has run the editor. The
    // terrain-material suite hit exactly that on a stale `Clouds_Demo_autosave.desce` and its helper is
    // copied here rather than reinvented (the suites share no header; copy-paste is the convention this
    // directory already follows for RepoRoot too).
    bool IsShippedScene( const std::filesystem::path& p )
    {
        for ( const auto& part : p )
            if ( part == "Autosave" )
                return false;
        return true;
    }
} // namespace

// ── The materials in the repository ────────────────────────────────────────────────────────────────

// WHY A SWEEP AND NOT A LIST. Two files collided here once and the way it was found was a developer
// tripping over it, months later, with the Material Editor showing him the wrong asset. A list of known
// ids would have to be edited by the same person who adds the next colliding file.
TEST( MaterialIdentity, NoTwoShippedMaterialsShareAMaterialId )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    const std::filesystem::path dir = root + "Editor/Resources/Assets/Materials";
    ASSERT_TRUE( std::filesystem::exists( dir ) ) << dir.string() << " is missing";

    // id -> the first file that claimed it, so a failure can name BOTH sides.
    std::map<uint64_t, std::string> claimed;
    int                             seen = 0;

    for ( const auto& entry : std::filesystem::recursive_directory_iterator( dir ) )
    {
        if ( !entry.is_regular_file() || entry.path().extension() != ".demat" )
            continue;

        const auto parsed = rfl::json::read<MaterialData>( ReadAll( entry.path() ) );
        ASSERT_TRUE( parsed ) << entry.path().string() << " does not parse as a material";
        ++seen;

        const std::string name = entry.path().filename().string();

        // A material with no MaterialId falls back to a PATH-derived handle, which is a different
        // identity scheme and cannot collide with a stated one. It is still asserted, because a shipped
        // material whose identity moves when the file is renamed is the defect the id exists to prevent.
        ASSERT_TRUE( parsed.value().MaterialId.has_value() )
             << name
             << " states no MaterialId, so its handle is derived from its path and any rename "
                "silently breaks every scene that names it.";

        const uint64_t id = static_cast<uint64_t>( *parsed.value().MaterialId );
        EXPECT_NE( id, 0u ) << name << " states MaterialId 0, which is the null handle.";

        const auto [it, inserted] = claimed.emplace( id, name );
        EXPECT_TRUE( inserted ) << "MaterialId " << id << " is claimed by BOTH '" << it->second << "' and '"
                                << name
                                << "'. A `.demat`'s MaterialId is its asset handle, so only one of the "
                                   "two can ever resolve: the Edit button, a double-click and every mesh "
                                   "slot naming this id open whichever registered first. Give one of them "
                                   "a different MaterialId and re-point every scene that names it.";
    }

    EXPECT_GT( seen, 0 ) << "no .demat files were found — the sweep asserted nothing";
}

// ── The relation: what a scene names, and what carries it ──────────────────────────────────────────

// Neither side is wrong on its own — a scene's MaterialGuid is a plausible number and each `.demat`'s
// MaterialId is a plausible number. The defect only exists in the DISAGREEMENT, which is why it is the
// agreement that is asserted (see the taxonomy in the desert-engine-verify skill).
TEST( MaterialIdentity, EveryMaterialGuidAShippedSceneNamesIsCarriedByExactlyOneMaterialFile )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    const std::filesystem::path materials = root + "Editor/Resources/Assets/Materials";
    const std::filesystem::path scenes    = root + "Editor/Resources/Assets/Scenes";
    ASSERT_TRUE( std::filesystem::exists( materials ) ) << materials.string() << " is missing";
    ASSERT_TRUE( std::filesystem::exists( scenes ) ) << scenes.string() << " is missing";

    // How many FILES carry each id. More than one is the collision the test above reports; the count is
    // kept here so this test can say "two files carry it" rather than "the scene is fine".
    std::map<uint64_t, std::vector<std::string>> carriers;
    for ( const auto& entry : std::filesystem::recursive_directory_iterator( materials ) )
    {
        if ( !entry.is_regular_file() || entry.path().extension() != ".demat" )
            continue;
        const auto parsed = rfl::json::read<MaterialData>( ReadAll( entry.path() ) );
        if ( !parsed || !parsed.value().MaterialId )
            continue;
        carriers[static_cast<uint64_t>( *parsed.value().MaterialId )].push_back(
             entry.path().filename().string() );
    }

    // Read as TEXT rather than through SceneSerialized: the ids live inside "MaterialGuids" arrays on
    // three different components, and a scan for the numbers is both shorter and blind to which component
    // they sat on — which is what this assertion wants. The canonical `.desce` text is multi-line, so the
    // key is matched with any whitespace around its colon.
    int checked = 0;
    for ( const auto& entry : std::filesystem::recursive_directory_iterator( scenes ) )
    {
        if ( !entry.is_regular_file() || entry.path().extension() != ".desce" )
            continue;
        if ( !IsShippedScene( entry.path() ) )
            continue;

        const std::string text = ReadAll( entry.path() );
        static const std::regex key( R"re("MaterialGuids"\s*:\s*\[)re" );

        for ( auto match = std::sregex_iterator( text.begin(), text.end(), key ); match != std::sregex_iterator();
              ++match )
        {
            size_t cursor = static_cast<size_t>( match->position() + match->length() );
            while ( cursor < text.size() && text[cursor] != ']' )
            {
                if ( !std::isdigit( static_cast<unsigned char>( text[cursor] ) ) )
                {
                    ++cursor;
                    continue;
                }
                size_t end = cursor;
                while ( end < text.size() && std::isdigit( static_cast<unsigned char>( text[end] ) ) )
                    ++end;

                const uint64_t guid = std::stoull( text.substr( cursor, end - cursor ) );
                cursor              = end;

                if ( guid == 0 )
                    continue; // an empty slot; the mesh falls back to its default material

                const auto it = carriers.find( guid );
                ++checked;

                // NOT an ASSERT that it exists: a scene may legitimately name a material that lives
                // beside an imported mesh rather than in Materials/. What must never be true is that it
                // names an id carried by TWO files, because then the scene does not say which.
                if ( it == carriers.end() )
                    continue;

                EXPECT_EQ( it->second.size(), 1u )
                     << entry.path().filename().string() << " names MaterialId " << guid
                     << ", which is carried by " << it->second.size()
                     << " material files — the scene cannot say which of them it means.";
            }
        }
    }

    EXPECT_GT( checked, 0 ) << "no MaterialGuids were found in any scene — the sweep asserted nothing";
}

// The other half of the same relation, and the one that says the v7 -> v8 migration produced a form the
// LOADER can read back. A migration is free to write any string it likes; what makes the string correct
// is that MakeAssetResolver::FromPath, which joins a relative path to the assets root, arrives at a file
// that exists. Asserted by doing exactly that join.
TEST( MaterialIdentity, EveryMaterialPathAShippedSceneNamesResolvesToAFileOnDisk )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    // The assets root as the editor sees it, prefixed by however far up the repository turned out to be.
    const std::filesystem::path assets = root + "Editor/Resources/Assets";
    const std::filesystem::path scenes = assets / "Scenes";
    ASSERT_TRUE( std::filesystem::exists( scenes ) ) << scenes.string() << " is missing";

    int checked = 0;
    for ( const auto& entry : std::filesystem::recursive_directory_iterator( scenes ) )
    {
        if ( !entry.is_regular_file() || entry.path().extension() != ".desce" )
            continue;
        if ( !IsShippedScene( entry.path() ) )
            continue;

        const std::string text = ReadAll( entry.path() );

        // Every quoted string that ends in the material extension, wherever it sits — an element of a
        // `MaterialPaths` array or the value of `Terrain.Material`. Scanning for the extension rather
        // than for the keys means a fifth place that names a material is covered the day it appears.
        const std::string ext = ".demat\"";
        for ( size_t at = text.find( ext ); at != std::string::npos; at = text.find( ext, at + 1 ) )
        {
            const size_t close = at + ext.size() - 1;
            const size_t open  = text.rfind( '"', close - 1 );
            if ( open == std::string::npos )
                continue;
            const std::string named = text.substr( open + 1, close - open - 1 );
            if ( named.empty() )
                continue;
            ++checked;

            // The join FromPath performs. An absolute path is used as it stands, which is what the
            // migration leaves a material outside the project as.
            const std::filesystem::path spelled( named );
            const std::filesystem::path full =
                 spelled.is_absolute() ? spelled : ( assets / spelled ).lexically_normal();

            EXPECT_TRUE( std::filesystem::exists( full ) )
                 << entry.path().filename().string() << " names the material '" << named
                 << "', which resolves to '" << full.string()
                 << "' and there is no such file. A scene names a material by a path relative to the "
                    "assets root; the loader joins the root to it exactly as this test just did.";
        }
    }

    EXPECT_GT( checked, 0 ) << "no material paths were found in any scene — the sweep asserted nothing";
}

// ── The rule MaterialService refuses on ────────────────────────────────────────────────────────────

TEST( MaterialIdentity, TwoDifferentFilesOnOneHandleAreACollision )
{
    EXPECT_TRUE( IsMaterialIdentityCollision( "Resources/Assets/Materials/MP_LitConst.demat",
                                              "Resources/Assets/Materials/MP_HandUnlit.demat" ) );
}

// The case that must NOT refuse, and the one that would have made this rule useless if it did: a
// material re-registering is routine (MaterialAssetUtils::CreatePBRMaterialAsset re-registers whatever it
// finds, and the editor re-registers on every shader change), and refusing there would break live edit.
TEST( MaterialIdentity, TheSameFileRegisteringAgainIsNotACollision )
{
    EXPECT_FALSE( IsMaterialIdentityCollision( "Resources/Assets/Materials/MP_LitConst.demat",
                                               "Resources/Assets/Materials/MP_LitConst.demat" ) );
}

// Two spellings of one path are one file. AssetManager::CreateAsset already deduplicates on
// StableKeyForPath, so this cannot arise from the asset database — but the rule must not depend on that
// being true, because the message it produces names both strings and would read as nonsense.
TEST( MaterialIdentity, TwoSpellingsOfOnePathAreNotACollision )
{
    EXPECT_FALSE( IsMaterialIdentityCollision( "Resources/Assets/Materials/M.demat",
                                               "./Resources/Assets/Materials/M.demat" ) );
    EXPECT_FALSE( IsMaterialIdentityCollision( "Resources/Assets/Materials/M.demat",
                                               "Resources/Assets/Textures/../Materials/M.demat" ) );
}

// A runtime-built material has no file behind it. Refusing on it would reject a material over a duplicate
// nobody can go and look at, and the log line would name an empty path.
TEST( MaterialIdentity, AMaterialWithNoFileIsNeverACollision )
{
    EXPECT_FALSE( IsMaterialIdentityCollision( "", "Resources/Assets/Materials/M.demat" ) );
    EXPECT_FALSE( IsMaterialIdentityCollision( "Resources/Assets/Materials/M.demat", "" ) );
    EXPECT_FALSE( IsMaterialIdentityCollision( "", "" ) );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
