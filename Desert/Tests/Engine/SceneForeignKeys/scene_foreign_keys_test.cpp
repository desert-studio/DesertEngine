// K11 — THE ONE RELATION THAT CLOSES THE CLASS, AND IT NEEDS NO GPU TO STATE.
//
//     A .desce read and written back with no change loses nothing it said.
//
// The defect this is about is not a wrong line anywhere; it is that every writer in the tree
// enumerated ITS OWN REGISTRY and not the file it was rewriting. Both sides of that are individually
// correct — the registry is a true statement of what this build knows, the file is a true statement
// of what somebody wrote — and a unit test of either passes. So the assertion is the AGREEMENT, over
// the real corpus, and it is affordable because the merge is a pure function over two parsed trees.
//
// TWO STRENGTHS, and they are different claims. LOSS-FREE is the one that closes K11 and holds for
// every file: nothing the source said is missing or altered afterwards. BYTE-IDENTICAL is the strong
// form, and it additionally requires the file to be CANONICAL — to already state every key this build
// knows about — because a writer that knows a field the file omits is right to add it. Both are here,
// and the corpus test says how many files are at each.

#include <gtest/gtest.h>

#include <Engine/Core/Serialize/ForeignKeys.hpp>
#include <Engine/Core/SceneSettings.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Reflection/ReflectionSerializer.hpp>

#include <rflcpp/rfl/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>
#include <Common/Json/Document.hpp>

namespace
{
    // DeserializeReflected reads a Json::Node and collects wrong-typed values as Issues; every fixture this
    // file feeds it is well-typed, so an Issue is a failure here.
    void ReadReflectedValue( const Desert::Reflection::TypeInfo& type, void* obj, const Common::Json::Value& src,
                             const Desert::Reflection::AssetResolver* resolver = nullptr )
    {
        Common::Json::Issues issues;
        Desert::Reflection::DeserializeReflected( type, obj, Common::Json::Root( src ), issues, resolver );
        for ( const auto& issue : issues )
            ADD_FAILURE() << Common::Json::Describe( issue );
    }
    void ReadReflectedValue( const Desert::Reflection::TypeInfo& type, void* obj, const Common::Json::Object& src,
                             const Desert::Reflection::AssetResolver* resolver = nullptr )
    {
        ReadReflectedValue( type, obj, Common::Json::Value( src ), resolver );
    }
} // namespace

using namespace Desert::Core::Serialize;

namespace
{
    rfl::Generic::Object Parse( const std::string& json )
    {
        const auto parsed = rfl::json::read<rfl::Generic>( json );
        EXPECT_TRUE( parsed.has_value() ) << json;
        if ( !parsed.has_value() )
            return {};
        const auto object = parsed.value().to_object();
        EXPECT_TRUE( object.has_value() ) << json;
        return object.has_value() ? object.value() : rfl::Generic::Object{};
    }

    std::string Write( const rfl::Generic::Object& object )
    {
        return rfl::json::write( object );
    }

    KeyIsOurs Owns( std::vector<std::string> names )
    {
        auto set = std::make_shared<std::unordered_set<std::string>>( names.begin(), names.end() );
        return [set]( const std::string& key ) { return set->count( key ) != 0; };
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

    std::string ReadAll( const std::filesystem::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    std::vector<std::filesystem::path> Corpus()
    {
        std::vector<std::filesystem::path> scenes;
        const std::string                  root = RepoRoot();
        if ( root.empty() )
            return scenes;
        std::error_code ec;
        for ( const auto& entry :
              std::filesystem::recursive_directory_iterator( root + "Editor/Resources/Assets/Scenes", ec ) )
        {
            if ( !entry.is_regular_file() || entry.path().extension() != ".desce" )
                continue;

            // `Autosave/` IS SKIPPED, AND THE REASON IS THE POINT OF THIS SUITE, NOT AN EXCEPTION TO IT.
            //
            // Autosaves are UNTRACKED — `git ls-files` on that directory is empty — and the editor
            // rewrites them while it runs. Walking the filesystem therefore collects a corpus that
            // differs per machine: on CI, where the checkout is clean, the directory does not exist and
            // the canonicity assertion passes; on any developer who has had the editor open since before
            // the last format bump, the same assertion fails on scratch files nobody committed. That is
            // this project's recurring shape — a container whose contents come from a different source
            // than the question asked of it — and it cost the integrator a false red on the merge that
            // landed this very suite.
            //
            // The question here is "does a scene THE REPOSITORY SHIPS survive a read-modify-write", so
            // the corpus is the shipped scenes. A developer's own autosave being unmigrated is a real
            // thing but it is the migrator's `--check` to report, not a shared gate's to fail on.
            bool underAutosave = false;
            for ( const auto& part : entry.path() )
                if ( part == "Autosave" )
                    underAutosave = true;
            if ( underAutosave )
                continue;

            scenes.push_back( entry.path() );
        }
        return scenes;
    }

    // Every key of `source`, at every depth, as "a.b.c" -> the value's JSON. What the loss test
    // compares, because "is anything gone" is a question about the whole tree and not one level.
    void Flatten( const rfl::Generic::Object& source, const std::string& prefix,
                  std::map<std::string, std::string>& into )
    {
        for ( const auto& [key, value] : source )
        {
            const std::string path = prefix.empty() ? key : prefix + "." + key;
            if ( const auto nested = value.to_object(); nested.has_value() )
                Flatten( nested.value(), path, into );
            else
                into[path] = rfl::json::write( value );
        }
    }
} // namespace

// ── the merge, as properties ─────────────────────────────────────────────────────────────────────

TEST( ForeignKeys, AKeyThisBuildDoesNotDeclareIsKeptAndKeptWhereItWas )
{
    // Position matters as much as presence. Appending preserved keys at the end would mean that any
    // build opening another build's file rewrites the whole thing, and then no diff over a .desce
    // says anything about what changed.
    const auto source = Parse( R"({"A":1,"Foreign":"x","B":2})" );
    const auto fresh  = Parse( R"({"A":1,"B":2})" );

    EXPECT_EQ( Write( MergeObjects( fresh, source, NothingIsOurs() ) ), R"({"A":1,"Foreign":"x","B":2})" );
}

TEST( ForeignKeys, ThisBuildsValueWinsForEveryKeyThisBuildStates )
{
    // Preservation is about KEYS and never about VALUES. A field both builds know is settled by the
    // one doing the saving, exactly as it was before — otherwise a save would be unable to change
    // anything.
    const auto source = Parse( R"({"A":1,"Foreign":"x"})" );
    const auto fresh  = Parse( R"({"A":99})" );

    EXPECT_EQ( Write( MergeObjects( fresh, source, NothingIsOurs() ) ), R"({"A":99,"Foreign":"x"})" );
}

TEST( ForeignKeys, AKeyTheWriterOWNSAndDidNotStateWasDELETEDAndDoesNotComeBack )
{
    // The one place the distinction is load-bearing. EntitySerializer writes a component key only when
    // the entity HAS that component, so an entity whose light the user just removed looks exactly like
    // an entity carrying a component this build never heard of. Without the predicate the merge would
    // faithfully resurrect every component anyone deleted.
    const auto source = Parse( R"({"Tag":"Sun","DirectionLight":{"Intensity":1.0},"Alien":{"k":1}})" );
    const auto fresh  = Parse( R"({"Tag":"Sun"})" );

    const auto merged = MergeObjects( fresh, source, Owns( { "Tag", "DirectionLight" } ) );
    EXPECT_EQ( Write( merged ), R"({"Tag":"Sun","Alien":{"k":1}})" )
         << "a deleted component came back, or a foreign one was dropped";
}

TEST( ForeignKeys, AForeignKeyInsideAKnownBlockSurvivesToo )
{
    // The level that no diagnostic walks and that the merge still has to cover: a field another build
    // added INSIDE a component this build knows. Both sides are objects, so the disagreement is
    // settled one level down — and down there nothing is ours, so anything we did not write is kept.
    const auto source = Parse( R"({"DirectionLight":{"Intensity":1.0,"Temperature":6500}})" );
    const auto fresh  = Parse( R"({"DirectionLight":{"Intensity":3.0}})" );

    EXPECT_EQ( Write( MergeObjects( fresh, source, Owns( { "DirectionLight" } ) ) ),
               R"({"DirectionLight":{"Intensity":3.0,"Temperature":6500}})" );
}

TEST( ForeignKeys, ADeletedEntityDoesNotComeBackAndANewOneIsWrittenUnchanged )
{
    // Entities are the one array in a .desce whose elements have identity, so they are merged
    // element by element on `id`. The walk is over the FRESH array and looks the source up, which is
    // what makes a record the writer no longer produces a deletion rather than a loss.
    const auto source =
         Parse( R"({"Entities":[{"id":1,"Tag":"Kept","Alien":true},{"id":2,"Tag":"Deleted"}],"SceneName":"S"})" );
    const auto fresh = Parse( R"({"Entities":[{"id":1,"Tag":"Kept"},{"id":3,"Tag":"New"}],"SceneName":"S"})" );

    const auto merged = MergeSceneDocument( fresh, source, Owns( { "id", "Tag" } ) );
    EXPECT_EQ( Write( merged ),
               R"({"Entities":[{"id":1,"Tag":"Kept","Alien":true},{"id":3,"Tag":"New"}],"SceneName":"S"})" );
}

TEST( ForeignKeys, ADocumentMergedWithItselfIsItself )
{
    // Idempotence, which is what "read it and write it back changes nothing" reduces to once the
    // writer's output equals the file. If this ever fails, every save churns every file.
    const auto document = Parse( R"({"SceneName":"S","Entities":[{"id":1,"Tag":"A","Alien":{"deep":[1,2]}}],)"
                                 R"("Settings":{"Exposure":1.0,"Unknown":7},"SceneVersion":14})" );

    EXPECT_EQ( Write( MergeSceneDocument( document, document, Owns( { "id", "Tag" } ) ) ), Write( document ) );
}

TEST( ForeignKeys, ARecordIsMatchedByIdWhereverItSitsAndTheFirstClaimantKeepsTheId )
{
    // The source is found by IDENTITY, not by position: the fresh array is in the opposite order, and
    // each record still gets its own foreign key. Two source records claiming one id: the FIRST is the
    // match - the rule the linear scan had with its `break`, kept when the scan became an index.
    const auto source = Parse( R"({"Entities":[{"id":1,"Alien":"one"},{"id":2,"Alien":"two"},)"
                               R"({"id":2,"Alien":"shadow"}]})" );
    const auto fresh  = Parse( R"({"Entities":[{"id":2,"Tag":"B"},{"id":1,"Tag":"A"}]})" );

    EXPECT_EQ( Write( MergeSceneDocument( fresh, source, Owns( { "id", "Tag" } ) ) ),
               R"({"Entities":[{"id":2,"Alien":"two","Tag":"B"},{"id":1,"Alien":"one","Tag":"A"}]})" );
}

namespace
{
    // A document of `count` entity records shaped like a generated world's (a transform, a mesh block and
    // one foreign key each), with the fresh side in REVERSED order so no record is found by luck of place.
    std::pair<rfl::Generic::Object, rfl::Generic::Object> WorldOf( const int count )
    {
        std::string source = R"({"SceneName":"W","Entities":[)";
        std::string fresh  = R"({"SceneName":"W","Entities":[)";
        for ( int i = 0; i < count; ++i )
        {
            const int         reversed  = count - 1 - i;
            const std::string separator = i == 0 ? "" : ",";
            const std::string body      = R"(,"Translation":[1.0,2.0,3.0],"StaticMesh":{"Primitive":"Cube"})";
            source += separator + R"({"id":)" + std::to_string( 1000 + i ) + R"(,"Tag":"C)" + std::to_string( i ) +
                      "\"" + body + R"(,"Alien":)" + std::to_string( i ) + "}";
            fresh += separator + R"({"id":)" + std::to_string( 1000 + reversed ) + R"(,"Tag":"C)" +
                     std::to_string( reversed ) + "\"" + body + "}";
        }
        source += "]}";
        fresh += "]}";
        return { Parse( fresh ), Parse( source ) };
    }

    double BestMergeMs( const int count )
    {
        const auto [fresh, source] = WorldOf( count );
        const KeyIsOurs ours       = Owns( { "id", "Tag", "Translation", "StaticMesh" } );
        double          best       = 1e30;
        for ( int run = 0; run < 3; ++run )
        {
            const auto started = std::chrono::steady_clock::now();
            const auto merged  = MergeSceneDocument( fresh, source, ours );
            best               = std::min(
                 best,
                 std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - started ).count() );
            EXPECT_EQ( merged.get( "Entities" ).value().to_array().value().size(),
                       static_cast<std::size_t>( count ) );
        }
        return best;
    }
} // namespace

TEST( ForeignKeys, MergingFourTimesTheRecordsCostsAboutFourTimesNotSixteen )
{
    // THE RELATION, NOT A BUDGET. The entity merge used to scan the whole source array for every fresh
    // record, copying each record it looked at: a 50 179-record world spent 419 s in the Play snapshot on
    // it (Release, 2026-09-24). Milliseconds depend on the machine; the RATIO between N and 4N records
    // does not - linear is 4, quadratic is 16. The bound sits between them with room for noise, and the
    // best of three runs is taken so a descheduled run cannot fail it.
    constexpr int kN   = 600;
    const double  once = BestMergeMs( kN );
    const double  four = BestMergeMs( 4 * kN );

    const double ratio = four / std::max( once, 1e-3 );
    std::cout << "[ MERGE    ] " << kN << " records " << once << " ms, " << 4 * kN << " records " << four
              << " ms, ratio " << ratio << "\n";
    EXPECT_LT( ratio, 8.0 ) << kN << " records merged in " << once << " ms, " << 4 * kN << " in " << four
                            << " ms: the merge grows faster than the records do";
}

TEST( ForeignKeys, TheReportGroupsByNameAndCountsRatherThanRepeating )
{
    // A key on forty entities is one finding with a count of forty, not forty log lines.
    std::map<std::string, int> counted;
    const auto                 ours = Owns( { "Tag" } );
    for ( int i = 0; i < 40; ++i )
        CountForeignKeysAtLevel( Parse( R"({"Tag":"x","Alien":1})" ), ours, counted );
    CountForeignKeysAtLevel( Parse( R"({"Tag":"x","Other":1})" ), ours, counted );

    EXPECT_EQ( DescribeForeignKeys( counted ), "Alien (x40), Other" );
    EXPECT_TRUE( DescribeForeignKeys( {} ).empty() ) << "an empty report must be an empty string";
}

// ── the corpus ───────────────────────────────────────────────────────────────────────────────────

TEST( ForeignKeysCorpus, NoSceneOnDiskLosesAnythingItSaysWhenItIsWrittenBack )
{
    // THE ASSERTION K11 EXISTS FOR, over every .desce in the repository. The Settings block is put
    // through exactly what the saver does to it — DeserializeReflected into a default SceneSettings,
    // then SerializeReflected back out of the same reflection table — and the result is merged onto
    // the file. Nothing the file said may be missing or different afterwards.
    //
    // Before this change the same walk deleted every key SceneSettings does not declare, which is how
    // `EnableSSGI` was live in 69 of these files for a month with nothing reading it and nothing
    // saying so.
    const auto scenes = Corpus();
    ASSERT_GE( scenes.size(), 40u ) << "the scene corpus was not found";

    const auto* settingsType = Desert::Reflection::ReflectionRegistry::Get().Find( "SceneSettings" );
    ASSERT_NE( settingsType, nullptr ) << "the reflection table this suite audits is empty";

    int canonical = 0;
    for ( const auto& path : scenes )
    {
        SCOPED_TRACE( path.string() );
        const auto document = Parse( ReadAll( path ) );
        const auto stated   = document.get( "Settings" );
        if ( !stated.has_value() )
            continue;
        const auto block = stated.value().to_object();
        if ( !block.has_value() )
            continue;

        Desert::Core::SceneSettings settings;
        ReadReflectedValue( *settingsType, &settings, block.value() );
        rfl::Generic::Object written = Desert::Reflection::SerializeReflected( *settingsType, &settings );

        // AN ASSET HANDLE HAS TWO ON-DISK FORMS AND THIS SUITE CANNOT PRODUCE THE RIGHT ONE. The saver
        // passes SerializeReflected an asset RESOLVER, which writes a handle as a path string; without
        // one it writes a raw integer. Building a resolver needs an AssetManager, which needs the engine,
        // which is exactly what keeps this suite able to run the whole corpus in eleven seconds.
        //
        // So the field is taken from the file, on the same rule and for the same reason as
        // Migration::CanonicaliseSettingsV13ToV14. Fourteen scenes state `"SplashSprite": ""`, and
        // without this they would be reported as drift that does not exist — which is worse than no
        // measurement, because it looks like evidence.
        for ( const auto& field : settingsType->Fields )
            if ( field.Type == Desert::Reflection::FieldType::AssetHandle )
                if ( const auto asStated = block.value().get( field.Name ); asStated.has_value() )
                    written[field.Name] = asStated.value();

        const rfl::Generic::Object merged = MergeObjects( written, block.value(), NothingIsOurs() );

        std::map<std::string, std::string> before;
        std::map<std::string, std::string> after;
        Flatten( block.value(), "", before );
        Flatten( merged, "", after );

        for ( const auto& [key, value] : before )
        {
            const auto survived = after.find( key );
            ASSERT_NE( survived, after.end() ) << "Settings." << key << " was deleted by writing the file back";
            // A key this build DECLARES may legitimately be restated (that is what saving is). A key
            // it does not declare must come back byte for byte.
            const bool ours = std::any_of( settingsType->Fields.begin(), settingsType->Fields.end(),
                                           [&key]( const Desert::Reflection::FieldInfo& f )
                                           { return key == f.Name || key.rfind( f.Name + ".", 0 ) == 0; } );
            if ( !ours )
                EXPECT_EQ( survived->second, value ) << "Settings." << key
                                                     << " was rewritten by a build "
                                                        "that does not declare it";
        }

        if ( rfl::json::write( merged ) == rfl::json::write( block.value() ) )
            ++canonical;
    }

    // THE STRONG FORM, AND IT IS AN ASSERTION BECAUSE IT IS NOW TRUE OF EVERY FILE. It was true of NONE
    // of them before this task: a scene stated only the fields that existed when it was last saved (26
    // of 51 in the oldest), and a float hand-edited to `0.26` came back as `0.2599999904632568`. Both
    // were settled in the FILES by the v13 -> v14 canonicalisation rather than in code (§4.3, §4.5), so
    // from here a save that changes nothing writes the same bytes.
    //
    // A scene added later that fails this has not been through the migrator. That is the message.
    EXPECT_EQ( canonical, static_cast<int>( scenes.size() ) )
         << "some scenes are not canonical, so reading and writing them back would rewrite bytes that "
            "did not change - run Tools/SceneMigrator over Editor/Resources/Assets/Scenes";
}

TEST( ForeignKeysCorpus, EverySceneOnDiskIsUnchangedByAWholeDocumentRoundTrip )
{
    // The weaker half, over the WHOLE file rather than the Settings block, and the one that pins key
    // ORDER and number formatting: parse it, merge it with itself, write it. Any drift here would
    // make the byte-identity claim above meaningless, because the noise floor would not be zero.
    const auto scenes = Corpus();
    ASSERT_GE( scenes.size(), 40u ) << "the scene corpus was not found";

    for ( const auto& path : scenes )
    {
        SCOPED_TRACE( path.string() );
        const std::string source   = ReadAll( path );
        const auto        document = Parse( source );
        EXPECT_EQ( Write( MergeSceneDocument( document, document, NothingIsOurs() ) ), Write( document ) );
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
