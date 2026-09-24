// The v24 -> v25 scene step (AF6c, decision D3): records are sorted by id and each states its place among
// its siblings. What must hold is a RELATION, checked over the whole tracked corpus: the hierarchy the
// loader builds (Rules::PlanSceneStitch, the loader's own plan) is the same before and after, and does not
// depend on the order the records sit in the file.

#include <SceneMigration.hpp>
#include <Common/Content/CanonicalText.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/SceneStitchRules.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs        = std::filesystem;
namespace Migration = Desert::Migration;
namespace Rules     = Desert::Core::Rules;

namespace
{
    using Records   = std::vector<Desert::Assets::EntityData>;
    using Hierarchy = std::map<uint64_t, std::vector<uint64_t>>; // parent id (0 = scene root) -> children

    uint64_t IdOf( const Desert::Assets::EntityData& record )
    {
        return static_cast<uint64_t>( record.id.value_or( Common::UUID( 0 ) ) );
    }

    // The hierarchy the loader builds from these records, in the order it builds it: pass 2 collects the
    // plan's Loads, pass 3 the prefab instances (each to a parent made before it), and the attaches are
    // then made together in Rules::OrderAttaches order (SceneSerializer::InstantiateRecords). A root is an
    // attach to parent 0 (the loader's null entity), ordered with the rest and listed under tree[0].
    Hierarchy Loaded( const Records& records )
    {
        uint64_t   minted = 1ull << 62;
        const auto plan   = Rules::PlanSceneStitch(
             records, [&]( void ) { return Common::UUID( ++minted ); },
             Rules::PrefabRecordPolicy::InstantiatedLater );
        Hierarchy                                   tree;
        std::unordered_set<uint64_t>                made;
        std::vector<Rules::PendingAttach<uint64_t>> attaches;
        for ( size_t slot = 0; slot < plan.Loads.size(); ++slot )
        {
            const auto& load = plan.Loads[slot];
            if ( load.Target != slot )
                continue; // a shadowed record lands on another entity; it makes no node of its own
            const uint64_t id = static_cast<uint64_t>( plan.Created[slot].Id );
            made.insert( id );
            if ( load.Parent == Rules::kNoSlot )
                attaches.push_back( { 0, id, Rules::SiblingIndexOf( records[load.Record] ) } );
            else
                attaches.push_back( { static_cast<uint64_t>( plan.Created[load.Parent].Id ), id,
                                      Rules::SiblingIndexOf( records[load.Record] ) } );
        }
        for ( const auto& prefab : plan.PrefabRecords )
        {
            const auto&    record = records[prefab.Record];
            const uint64_t id     = IdOf( record );
            const bool     hangs =
                 record.parent.has_value() && made.contains( static_cast<uint64_t>( *record.parent ) );
            attaches.push_back(
                 { hangs ? static_cast<uint64_t>( *record.parent ) : 0, id, Rules::SiblingIndexOf( record ) } );
            if ( id != 0 )
                made.insert( id );
        }
        Rules::OrderAttaches( attaches );
        for ( const auto& attach : attaches )
            tree[attach.Parent].push_back( attach.Child );
        return tree;
    }

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

    std::vector<fs::path> Corpus()
    {
        std::vector<fs::path> files;
        const std::string     root = RepoRoot();
        if ( root.empty() )
            return files;
        std::error_code ec;
        for ( const auto& entry : fs::recursive_directory_iterator( root + "Editor/Resources/Assets/Scenes", ec ) )
            if ( entry.path().extension() == ".desce" &&
                 entry.path().string().find( "Autosave" ) == std::string::npos )
                files.push_back( entry.path() );
        return files;
    }

    Desert::Migration::SceneSerialized Read( const fs::path& file )
    {
        std::ifstream     in( file, std::ios::binary );
        std::stringstream text;
        text << in.rdbuf();
        auto scene = rfl::json::read<Desert::Migration::SceneSerialized>( text.str() );
        EXPECT_TRUE( scene ) << file << " does not parse";
        return scene ? scene.value() : Desert::Migration::SceneSerialized{};
    }

    bool SortedById( const Records& records )
    {
        return std::is_sorted( records.begin(), records.end(),
                               []( const auto& a, const auto& b ) { return IdOf( a ) < IdOf( b ); } );
    }

    std::vector<std::string> Lines( const Records& records )
    {
        Desert::Migration::SceneSerialized scene;
        scene.Entities  = records;
        const auto text = Common::Content::CanonicalJsonText( rfl::json::write( scene ) );
        EXPECT_TRUE( text );
        std::vector<std::string> lines;
        std::istringstream       in( text ? text.GetValue() : std::string() );
        for ( std::string line; std::getline( in, line ); )
            lines.push_back( line );
        return lines;
    }

    Desert::Migration::SceneSerialized Fixture()
    {
        // The prefab instance sits BEFORE C2 in the file, but the v24 loader attached it after (pass 3);
        // the orphan names a parent nothing answers to and was therefore a root.
        constexpr const char* kV24Scene = R"({"SceneName":"S","Entities":[
            {"id":90,"Tag":"RootA"},
            {"id":7,"parent":90,"Tag":"C1"},
            {"id":80,"PrefabPath":"Prefabs/P.deprefab","parent":90},
            {"id":3,"parent":90,"Tag":"C2"},
            {"id":50,"Tag":"RootB"},
            {"id":4,"parent":404,"Tag":"Orphan"}],
            "UnitVersion":1,"SceneVersion":24})";
        return rfl::json::read<Desert::Migration::SceneSerialized>( kV24Scene ).value();
    }
} // namespace

TEST( SceneSiblingOrderMigration, TheStepIsTheOneBelowTheTextHeaderHead )
{
    EXPECT_EQ( Migration::kSceneVersionSiblingOrder + 1, Migration::kSceneVersionTextHeader );
    EXPECT_EQ( Migration::kSceneVersionTextHeader, Desert::Core::kSceneVersion );
    EXPECT_EQ( Migration::kSceneVersionSiblingOrder, Migration::kSceneVersionTextureAssetRefs + 1 );
}

TEST( SceneSiblingOrderMigration, TheLoadedTreeIsTheOneTheV24LoaderBuilt )
{
    auto       scene  = Fixture();
    const auto before = Loaded( scene.Entities );
    const auto report = Migration::MigrateScene( scene, fs::temp_directory_path(), "S.desce" );
    ASSERT_TRUE( report.SiblingOrderRaised );
    EXPECT_EQ( report.SiblingOrder.Indexed, 6u );
    EXPECT_TRUE( SortedById( scene.Entities ) );
    EXPECT_EQ( Loaded( scene.Entities ), before );
    EXPECT_EQ( before.at( 90 ), ( std::vector<uint64_t>{ 7, 3, 80 } ) );
    EXPECT_EQ( before.at( 0 ), ( std::vector<uint64_t>{ 90, 50, 4 } ) );
}

// The defect AF6c found: an ordinary child saved AFTER a prefab sibling came back BEFORE it, because pass 2
// attached ordinary children at once and pass 3 appended every prefab instance behind them.
TEST( SceneSiblingOrderMigration, AnOrdinaryChildAfterAPrefabSiblingStaysAfterIt )
{
    constexpr const char* kV25Scene = R"({"SceneName":"S","Entities":[
        {"id":3,"parent":90,"Tag":"C2","siblingIndex":2},
        {"id":7,"parent":90,"Tag":"C1","siblingIndex":0},
        {"id":80,"PrefabPath":"Prefabs/P.deprefab","parent":90,"siblingIndex":1},
        {"id":90,"Tag":"RootA","siblingIndex":0}],
        "UnitVersion":1,"SceneVersion":25})";
    const auto            scene     = rfl::json::read<Desert::Migration::SceneSerialized>( kV25Scene ).value();
    EXPECT_EQ( Loaded( scene.Entities ).at( 90 ), ( std::vector<uint64_t>{ 7, 80, 3 } ) );

    // And the round trip: a saved v25 scene with the prefab sibling in the middle keeps it there under any
    // record order.
    auto         shuffled = scene.Entities;
    std::mt19937 shuffle( 26 );
    for ( int round = 0; round < 8; ++round )
    {
        std::shuffle( shuffled.begin(), shuffled.end(), shuffle );
        EXPECT_EQ( Loaded( shuffled ).at( 90 ), ( std::vector<uint64_t>{ 7, 80, 3 } ) );
    }
}

// The same defect one level up: a prefab root is instantiated after every ordinary root, so before the roots
// took their place by siblingIndex a prefab saved between two ordinary roots loaded after both.
TEST( SceneSiblingOrderMigration, APrefabRootBetweenOrdinaryRootsStaysBetweenThem )
{
    constexpr const char*       kV25Scene = R"({"SceneName":"S","Entities":[
        {"id":3,"Tag":"R3","siblingIndex":3},
        {"id":80,"PrefabPath":"Prefabs/P.deprefab","siblingIndex":1},
        {"id":7,"Tag":"R0","siblingIndex":0},
        {"id":81,"PrefabPath":"Prefabs/Q.deprefab","siblingIndex":4},
        {"id":5,"Tag":"R2","siblingIndex":2},
        {"id":6,"parent":5,"Tag":"Child","siblingIndex":0}],
        "UnitVersion":1,"SceneVersion":25})";
    const auto                  scene = rfl::json::read<Desert::Migration::SceneSerialized>( kV25Scene ).value();
    const std::vector<uint64_t> expected{ 7, 80, 5, 3, 81 };
    EXPECT_EQ( Loaded( scene.Entities ).at( 0 ), expected );

    auto         shuffled = scene.Entities;
    std::mt19937 shuffle( 27 );
    for ( int round = 0; round < 8; ++round )
    {
        std::shuffle( shuffled.begin(), shuffled.end(), shuffle );
        const auto tree = Loaded( shuffled );
        EXPECT_EQ( tree.at( 0 ), expected );
        EXPECT_EQ( tree.at( 5 ), ( std::vector<uint64_t>{ 6 } ) );
    }
}

// Over every tracked scene: a v24 file migrates to the same tree; a v25 file (the corpus after the run) is
// sorted and indexed. Either way, ANY permutation of the records loads the same tree.
TEST( SceneSiblingOrderMigration, EveryCorpusSceneKeepsItsTreeAndIgnoresRecordOrder )
{
    std::mt19937 shuffle( 25 );
    size_t       migrated = 0;
    size_t       current  = 0;
    for ( const auto& file : Corpus() )
    {
        auto scene = Read( file );
        if ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ) <
             Migration::kSceneVersionTextureAssetRefs )
            continue; // an older generation goes through every earlier step first; not this step's business
        const auto before = Loaded( scene.Entities );
        if ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ) <
             Migration::kSceneVersionSiblingOrder )
        {
            Migration::MigrateScene( scene, fs::temp_directory_path(), file );
            ++migrated;
        }
        else
            ++current;
        EXPECT_TRUE( SortedById( scene.Entities ) ) << file;
        for ( const auto& record : scene.Entities )
            EXPECT_TRUE( record.siblingIndex.has_value() ) << file << " record " << IdOf( record );
        EXPECT_EQ( Loaded( scene.Entities ), before ) << file;
        for ( int round = 0; round < 3; ++round )
        {
            std::shuffle( scene.Entities.begin(), scene.Entities.end(), shuffle );
            EXPECT_EQ( Loaded( scene.Entities ), before ) << file << " shuffled";
        }
    }
    std::printf( "[corpus] %zu scene(s) migrated from v24, %zu already at v25\n", migrated, current );
    EXPECT_GE( migrated + current, 20u ) << "the corpus walk found almost nothing - is the root right?";
}

// Adding a child after its siblings changes the file in ONE contiguous place - its own record - and no
// line of any other record, because the file order is a function of the id set.
TEST( SceneSiblingOrderMigration, AddingAnEntityChangesOnlyItsOwnLines )
{
    auto scene = Fixture();
    Migration::MigrateScene( scene, fs::temp_directory_path(), "S.desce" );
    const auto oldLines = Lines( scene.Entities );

    Desert::Assets::EntityData added;
    added.id           = Common::UUID( 60 );
    added.parent       = Common::UUID( 50 );
    added.siblingIndex = 0;
    added.Tag          = "Added";
    Records records    = scene.Entities;
    records.push_back( added );
    std::stable_sort( records.begin(), records.end(),
                      []( const auto& a, const auto& b ) { return IdOf( a ) < IdOf( b ); } );
    const auto newLines = Lines( records );
    ASSERT_GT( newLines.size(), oldLines.size() );

    size_t head = 0;
    while ( head < oldLines.size() && oldLines[head] == newLines[head] )
        ++head;
    size_t tail = 0;
    while ( tail < oldLines.size() - head &&
            oldLines[oldLines.size() - 1 - tail] == newLines[newLines.size() - 1 - tail] )
        ++tail;
    EXPECT_EQ( head + tail, oldLines.size() ) << "a line of an existing record changed";
    EXPECT_LE( newLines.size() - oldLines.size(), 8u ) << "the insertion is more than the new record";
    EXPECT_EQ( Loaded( records ).at( 50 ), ( std::vector<uint64_t>{ 60 } ) );
    EXPECT_EQ( Loaded( records ).at( 90 ), Loaded( scene.Entities ).at( 90 ) );
}
int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
