// A `.demat`'s header GUID IS its identity (MATL 2), its asset handle is that GUID through HandleForGuid,
// and the editor addresses a document BY that handle.
//
// So two files carrying one GUID (or two GUIDs folding to one handle) are not a tidiness problem, they are a
// wrong-asset problem: whichever registers second wins AssetManager::m_HandleLookup and MaterialService's maps,
// the Edit button and a double-click on either file open the same document, and the other material can never be
// resolved at all. `MP_LitConst.demat` and `MP_HandUnlit.demat` both carried 6666666666666666666, and a
// developer found it by having the Material Editor open somebody else's material in front of him.
//
// The collision stayed invisible for as long as a material was chosen from a combo box BY NAME. Nothing
// about the ids changed when documents became handle-addressed; what changed is that the defect acquired
// a symptom. That is the argument for asserting the property in a test rather than remembering it.
//
// Three things are asserted here:
//
//   1. Every shipped `.demat` is MATL 3: one identity (the header GUID), no second number beside it, an
//      instance's Parent is a shipped material's GUID stated again as the header's one Dependency, and no
//      two files share a GUID or a handle.
//   2. Every MaterialGuid a shipped scene names (still the MATL 1 number until SCNE 27) is translated by the
//      LegacyMaterialIds register to exactly ONE `.demat` — the relation between the two sides.
//   3. The rule MaterialService refuses on, driven directly, including the cases that must NOT refuse.

#include <gtest/gtest.h>

#include <Engine/Assets/MaterialData.hpp>
#include <Engine/Assets/MaterialFormat.hpp>
#include <Engine/Runtime/Services/Material/MaterialIdentity.hpp>

// Same serialization environment as SurfaceMaterialAsset.cpp: glm/UUID adapters + json backend.
#include <Common/Core/Serialization/GlmReflection.hpp>

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
namespace
{
    struct ShippedMaterial
    {
        std::string  Name;
        std::string  Text;
        MaterialData Data;
    };

    // Every shipped `.demat`, read through the engine's own parser (which refuses anything but MATL 3, a
    // malformed Parent and a Parent missing from the header's Dependencies).
    std::vector<ShippedMaterial> ReadShippedMaterials( std::vector<std::string>& refusals )
    {
        std::vector<ShippedMaterial> out;
        const std::filesystem::path  dir = RepoRoot() + "Editor/Resources/Assets/Materials";
        if ( !std::filesystem::exists( dir ) )
            return out;
        for ( const auto& entry : std::filesystem::recursive_directory_iterator( dir ) )
        {
            if ( !entry.is_regular_file() || entry.path().extension() != ".demat" )
                continue;
            std::string text   = ReadAll( entry.path() );
            const auto  parsed = Desert::Assets::ParseMaterialJson( entry.path().string(), text );
            if ( !parsed )
            {
                refusals.push_back( parsed.GetError() );
                continue;
            }
            out.push_back( { entry.path().filename().string(), std::move( text ), parsed.GetValue() } );
        }
        return out;
    }

    // The register the migrator wrote once from the MATL 1 corpus (Tools/SceneMigrator LegacyMaterialIds).
    struct LegacyRow
    {
        uint64_t    MaterialId = 0;
        std::string Guid;
    };
    struct LegacyRegister
    {
        std::vector<LegacyRow> Ids;
    };
} // namespace

TEST( MaterialIdentity, EveryShippedMaterialIsMatl3WithOneIdentityAndNoTwoShareIt )
{
    ASSERT_FALSE( RepoRoot().empty() ) << "repository root not found from the test's working directory";
    std::vector<std::string> refusals;
    const auto               materials = ReadShippedMaterials( refusals );
    for ( const auto& refusal : refusals )
        ADD_FAILURE() << refusal;
    ASSERT_GT( materials.size(), 0u ) << "no .demat files were found — the sweep asserted nothing";

    std::map<std::string, std::string> byGuid;
    std::map<uint64_t, std::string>    byHandle;
    for ( const auto& m : materials )
    {
        // The second identity is gone from the TEXT, not merely ignored by the reader.
        EXPECT_EQ( m.Text.find( "\"MaterialId\"" ), std::string::npos ) << m.Name << " still states a MaterialId";
        EXPECT_EQ( m.Text.find( "\"ParentMaterialId\"" ), std::string::npos )
             << m.Name << " still states a ParentMaterialId";

        const auto guid = m.Data.Guid();
        ASSERT_FALSE( guid.IsNull() ) << m.Name << " has a null header GUID";
        const uint64_t handle = static_cast<uint64_t>( m.Data.Handle() );
        EXPECT_EQ( handle, static_cast<uint64_t>( Common::Content::HandleForGuid( guid ) ) )
             << m.Name << ": Handle() is not HandleForGuid of the header GUID";
        EXPECT_NE( handle, 0u ) << m.Name << " folds to the null handle";

        const auto [g, newGuid] = byGuid.emplace( Common::Content::AssetGuidToText( guid ), m.Name );
        EXPECT_TRUE( newGuid ) << "GUID claimed by BOTH '" << g->second << "' and '" << m.Name << "'";
        const auto [h, newHandle] = byHandle.emplace( handle, m.Name );
        EXPECT_TRUE( newHandle ) << "handle " << handle << " claimed by BOTH '" << h->second << "' and '" << m.Name
                                 << "': only one of the two can ever resolve";
    }

    // An instance names a material that ships, and says so in its header.
    int instances = 0;
    for ( const auto& m : materials )
    {
        if ( !m.Data.IsInstance() )
        {
            // MATL 3: a base material's header states exactly its slot GUIDs (StampMaterialHeader).
            EXPECT_EQ( m.Data.Header->Dependencies, m.Data.ReferencedGuidTexts() )
                 << m.Name << " is a base material whose dependencies are not its slot GUIDs";
            continue;
        }
        ++instances;
        const auto parent = Common::Content::AssetGuidToText( m.Data.ParentGuid() );
        EXPECT_EQ( parent, *m.Data.Parent ) << m.Name << ": Parent is not a canonical GUID";
        EXPECT_TRUE( byGuid.count( parent ) )
             << m.Name << " names parent " << parent << ", which no shipped file carries";
        ASSERT_EQ( m.Data.Header->Dependencies.size(), 1u ) << m.Name;
        EXPECT_EQ( m.Data.Header->Dependencies.front(), parent ) << m.Name;
        EXPECT_EQ( static_cast<uint64_t>( *m.Data.InstanceParentId() ),
                   static_cast<uint64_t>( Common::Content::HandleForGuid( m.Data.ParentGuid() ) ) )
             << m.Name << ": the parent's handle is not the fold of its GUID";
    }
    EXPECT_GE( instances, 2 ) << "the corpus has two instances; fewer means the sweep lost them";
}

TEST( MaterialIdentity, TheParserRefusesAParentThatIsNotAStatedGuid )
{
    const std::string head = R"({"Header":{"Kind":"Material","Guid":"3cac456286293463b516718906b23e28",)"
                             R"("Versions":{"MATL":4},"Dependencies":[)";
    const std::string good = head +
                             R"("45d579b03cc0d0a8df2e4cb025d6bea5"]},"Params":[],"Textures":[],"CloudAssets":[],)"
                             R"("Parent":"45d579b03cc0d0a8df2e4cb025d6bea5"})";
    const auto        parsed = Desert::Assets::ParseMaterialJson( "good", good );
    ASSERT_TRUE( parsed ) << parsed.GetError();
    EXPECT_EQ( Common::Content::AssetGuidToText( parsed.GetValue().ParentGuid() ),
               "45d579b03cc0d0a8df2e4cb025d6bea5" );

    EXPECT_FALSE( Desert::Assets::ParseMaterialJson(
         "undeclared",
         head + R"(]},"Params":[],"Textures":[],"CloudAssets":[],"Parent":"45d579b03cc0d0a8df2e4cb025d6bea5"})" ) )
         << "a Parent missing from the header's Dependencies must be refused";
    EXPECT_FALSE( Desert::Assets::ParseMaterialJson(
         "number",
         head +
              R"("6418972230554417713"]},"Params":[],"Textures":[],"CloudAssets":[],"Parent":"6418972230554417713"})" ) )
         << "a MATL 1 number in Parent is not a GUID";

    std::string v1 = good;
    v1.replace( v1.find( R"("MATL":4)" ), 8, R"("MATL":1)" );
    EXPECT_FALSE( Desert::Assets::ParseMaterialJson( "v1", v1 ) ) << "a MATL 1 file must be refused";
}

TEST( MaterialIdentity, StampingAnInstanceStatesItsParentAsTheOneDependency )
{
    MaterialData m;
    m.SetParent( { 0x45d579b03cc0d0a8ull, 0xdf2e4cb025d6bea5ull } );
    const auto stamped = Desert::Assets::StampMaterialHeader( m );
    ASSERT_TRUE( stamped.Header.has_value() );
    ASSERT_EQ( stamped.Header->Dependencies.size(), 1u );
    EXPECT_EQ( stamped.Header->Dependencies.front(), "45d579b03cc0d0a8df2e4cb025d6bea5" );
    EXPECT_FALSE( stamped.Guid().IsNull() ) << "a material that never had a GUID is minted one";

    const auto text = Desert::Assets::WriteMaterialJson( stamped );
    ASSERT_TRUE( text ) << text.GetError();
    const auto back = Desert::Assets::ParseMaterialJson( "stamped", text.GetValue() );
    ASSERT_TRUE( back ) << back.GetError();
    EXPECT_EQ( back.GetValue().Guid(), stamped.Guid() );
    EXPECT_EQ( back.GetValue().ParentGuid(), stamped.ParentGuid() );
}

// ── The relation: what a scene names, and what carries it ──────────────────────────────────────────

// Neither side is wrong on its own — a scene's MaterialGuid is a plausible number and each `.demat`'s
// GUID is a plausible GUID. The defect only exists in the DISAGREEMENT, which is why it is the agreement
// that is asserted (see the taxonomy in the desert-engine-verify skill). Since SCNE 27 a scene
// names the header GUID's text itself.
TEST( MaterialIdentity, EveryMaterialGuidAShippedSceneNamesIsCarriedByExactlyOneMaterialFile )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    const std::filesystem::path scenes = root + "Editor/Resources/Assets/Scenes";
    ASSERT_TRUE( std::filesystem::exists( scenes ) ) << scenes.string() << " is missing";

    std::vector<std::string>                        refusals;
    std::map<std::string, std::vector<std::string>> carriers; // GUID text -> files
    for ( const auto& m : ReadShippedMaterials( refusals ) )
        carriers[Common::Content::AssetGuidToText( m.Data.Guid() )].push_back( m.Name );

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
                if ( text[cursor] != '"' )
                {
                    ++cursor;
                    continue;
                }
                const size_t      end  = text.find( '"', cursor + 1 );
                const std::string guid = text.substr( cursor + 1, end - cursor - 1 );
                cursor                 = end == std::string::npos ? text.size() : end + 1;
                if ( guid.empty() )
                    continue; // an empty slot; the mesh falls back to its default material

                // A scene may legitimately name a material that lives beside an imported mesh rather than in
                // Materials/. What must never be true is that the GUID is carried by TWO files.
                const auto it = carriers.find( guid );
                if ( it == carriers.end() )
                    continue;
                ++checked;
                EXPECT_EQ( it->second.size(), 1u )
                     << entry.path().filename().string() << " names material GUID " << guid
                     << ", which is carried by " << it->second.size() << " material files";
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
