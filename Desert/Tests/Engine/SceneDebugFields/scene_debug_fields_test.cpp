// NO DEBUG VISUALIZATION IS SERIALIZED INTO A LEVEL. Not in the struct, not in a file on disk, ever again.
//
// This is the third census in this repository built the same way, and it exists because the other two
// could not have caught what К2 fixed. `SettingConsumers` asks "does anything READ this field?" —
// `ShowColliders` passed it for as long as it existed, because EditorColliderPass really did read it.
// `SceneVersionGate` asks "will this file LOAD?" — every one of them did. Neither can ask the question
// that mattered: *should this be in the scene file at all?* A flag saying what a viewport is drawing is
// not a property of the world, and while it was one, `ShowColliders` defaulted to true and shipped `true`
// in 55 of the 80 committed scenes, so green physics wireframes travelled through git to everybody.
//
// WHAT IS ASSERTED, AND WHY IT IS A RELATION RATHER THAN A LIST. A hand-written list of ten forbidden
// names would go stale the day somebody adds an eleventh debug flag — which is precisely the edit this
// suite has to survive. So the forbidden set is DERIVED, at test time, from the source text of the struct
// that owns these fields now:
//
//   Graphic::DebugViewState  --(its own field names)-->  forbidden in Core::SceneSettings
//                            --(the same names)------->  forbidden as a key in any .desce on disk
//                            --(the same names)------->  must equal Migration::kDebugViewKeys
//
// Add a field to DebugViewState and all three checks extend themselves. Move one back into SceneSettings
// and the first goes red. Hand-edit one into a scene file and the second does. Add one to DebugViewState
// without teaching the migration to strip it and the third does.
//
// TEXTUAL, on purpose, exactly like SettingConsumers reads its consumers as text: it lets one suite hold a
// declaration in the engine, a table in a TOOL and 83 data files side by side without linking any of them.
// The parse is deliberately strict — every extraction asserts it found something — because the failure
// mode of a census that parses nothing is a green run that certifies nothing (SceneVersionGate learned the
// same lesson and pins its corpus size for the same reason).

#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Reflection/ReflectionTypes.hpp>

#include <Common/Json/Document.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using Desert::Reflection::FieldInfo;
using Desert::Reflection::ReflectionRegistry;
using Desert::Reflection::TypeInfo;

namespace
{
    constexpr const char* kDebugViewHeader = "Desert/Desert/Source/Engine/Graphic/DebugViewState.hpp";
    constexpr const char* kMigrationHeader = "Tools/SceneMigrator/Source/SceneMigration.hpp";
    constexpr const char* kSceneSettings   = "Desert/Desert/Source/Engine/Core/SceneSettings.hpp";
    constexpr const char* kSceneDirectory  = "Editor/Resources/Assets/Scenes";

    // Walks up from the working directory looking for a file only the repository has — the test runner's
    // working directory is not fixed. Same shape as Desert/Tests/Engine/SceneVersionGate.
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + kSceneSettings );
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

    bool IsIdentChar( char c )
    {
        return std::isalnum( static_cast<unsigned char>( c ) ) != 0 || c == '_';
    }

    // Comments go first. Half of DebugViewState.hpp is the argument for why it exists, and a sentence in
    // that argument naming a field must not be mistaken for a declaration of one.
    std::string StripComments( const std::string& src )
    {
        std::string out;
        out.reserve( src.size() );
        for ( std::size_t i = 0; i < src.size(); )
        {
            if ( src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/' )
            {
                while ( i < src.size() && src[i] != '\n' )
                    ++i;
            }
            else if ( src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*' )
            {
                i += 2;
                while ( i + 1 < src.size() && !( src[i] == '*' && src[i + 1] == '/' ) )
                    ++i;
                i = i + 2 < src.size() ? i + 2 : src.size();
                out += ' ';
            }
            else
            {
                out += src[i++];
            }
        }
        return out;
    }

    // The brace-matched body of `struct <name>`, comments already gone. Empty when the struct is not there
    // at all, which every caller treats as a failure rather than as "no fields".
    std::string StructBody( const std::string& source, const std::string& name )
    {
        const std::size_t at = source.find( "struct " + name );
        if ( at == std::string::npos )
            return {};
        const std::size_t open = source.find( '{', at );
        if ( open == std::string::npos )
            return {};

        int depth = 0;
        for ( std::size_t i = open; i < source.size(); ++i )
        {
            if ( source[i] == '{' )
                ++depth;
            else if ( source[i] == '}' && --depth == 0 )
                return source.substr( open + 1, i - open - 1 );
        }
        return {};
    }

    // The declared names in a struct body: for each `;`-terminated statement, the identifier immediately
    // before the `=` (or the last one, when the member has no initializer). `glm::vec3 BoundingBoxColor =
    // glm::vec3( ... );` yields BoundingBoxColor, and so does `ShadowDebugMode ShadowDebug =
    // ShadowDebugMode::Off;` — the type's own name is never the last identifier before the assignment.
    std::vector<std::string> DeclaredFields( const std::string& body )
    {
        std::vector<std::string> fields;
        std::size_t              start = 0;
        while ( start < body.size() )
        {
            const std::size_t end = body.find( ';', start );
            const std::string statement =
                 body.substr( start, end == std::string::npos ? std::string::npos : end - start );
            start = end == std::string::npos ? body.size() : end + 1;

            const std::size_t eq   = statement.find( '=' );
            const std::string head = statement.substr( 0, eq == std::string::npos ? statement.size() : eq );

            std::string last;
            for ( std::size_t i = 0; i < head.size(); )
            {
                if ( !IsIdentChar( head[i] ) || std::isdigit( static_cast<unsigned char>( head[i] ) ) != 0 )
                {
                    ++i;
                    continue;
                }
                const std::size_t from = i;
                while ( i < head.size() && IsIdentChar( head[i] ) )
                    ++i;
                last = head.substr( from, i - from );
            }
            if ( !last.empty() )
                fields.push_back( last );
        }
        return fields;
    }

    // The string literals of `std::array kDebugViewKeys = { ... }` in the migration header — the migration's own
    // statement of what it strips, read as data so this suite can compare it with the struct without
    // linking the tool.
    std::vector<std::string> MigrationKeys( const std::string& source )
    {
        std::vector<std::string> keys;
        const std::size_t        at = source.find( "kDebugViewKeys" );
        if ( at == std::string::npos )
            return keys;
        const std::size_t open  = source.find( '{', at );
        const std::size_t close = open == std::string::npos ? std::string::npos : source.find( '}', open );
        if ( open == std::string::npos || close == std::string::npos )
            return keys;

        for ( std::size_t i = open; i < close; ++i )
        {
            if ( source[i] != '"' )
                continue;
            const std::size_t quote = source.find( '"', i + 1 );
            if ( quote == std::string::npos || quote > close )
                break;
            keys.push_back( source.substr( i + 1, quote - i - 1 ) );
            i = quote;
        }
        return keys;
    }

    // The names a scene file must never state, taken from the struct that owns them.
    std::vector<std::string> ForbiddenKeys()
    {
        const std::string body =
             StructBody( StripComments( ReadAll( RepoRoot() + kDebugViewHeader ) ), "DebugViewState" );
        return DeclaredFields( body );
    }

    std::vector<std::filesystem::path> RepositoryScenes()
    {
        std::vector<std::filesystem::path> scenes;
        std::error_code                    ec;
        const std::filesystem::path        root = RepoRoot() + kSceneDirectory;
        for ( const auto& entry : std::filesystem::recursive_directory_iterator( root, ec ) )
        {
            if ( entry.is_regular_file() && entry.path().extension() == ".desce" )
                scenes.push_back( entry.path() );
        }
        return scenes;
    }

    const TypeInfo& SceneSettingsType()
    {
        const TypeInfo* info = ReflectionRegistry::Get().Find( "SceneSettings" );
        EXPECT_NE( info, nullptr ) << "SceneSettings is not reflected at all - this suite cannot check it";
        static TypeInfo empty;
        return info != nullptr ? *info : empty;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// 0. THE SUITE CAN SEE WHAT IT CLAIMS TO CHECK
// ---------------------------------------------------------------------------------------------------

// Without this every assertion below is a loop over an empty set reporting green. A census that parses
// nothing certifies nothing, and it looks exactly like a census that found nothing wrong.
TEST( SceneDebugFields, TheSourcesThisSuiteReadsAreWhereItThinksTheyAre )
{
    ASSERT_FALSE( RepoRoot().empty() ) << "repository root not found from the test's working directory";

    const std::vector<std::string> forbidden = ForbiddenKeys();
    EXPECT_GE( forbidden.size(), 8u ) << "DebugViewState parsed to " << forbidden.size()
                                      << " field(s) - the struct moved, was renamed, or the parse broke";

    EXPECT_FALSE( MigrationKeys( ReadAll( RepoRoot() + kMigrationHeader ) ).empty() )
         << kMigrationHeader << " states no kDebugViewKeys - the migration's key list moved or was renamed";

    EXPECT_GE( RepositoryScenes().size(), 40u ) << "the scene corpus was not found";

    EXPECT_FALSE( SceneSettingsType().Fields.empty() );
}

// ---------------------------------------------------------------------------------------------------
// 1. THE STRUCT — nothing DebugViewState owns may also be a reflected scene setting
// ---------------------------------------------------------------------------------------------------

// A reflected field of SceneSettings is, by construction, a serialized one: the whole block round-trips
// through the generic reflection serializer, so a PROPERTY is a key in every .desce. Moving one of these
// names back into that struct — for any reason, however locally sensible — puts a view flag back in the
// level file, and this is what says so.
TEST( SceneDebugFields, NoFieldOfTheDebugViewIsAlsoASerializedSceneSetting )
{
    const std::vector<std::string> forbidden = ForbiddenKeys();
    ASSERT_FALSE( forbidden.empty() );

    for ( const FieldInfo& field : SceneSettingsType().Fields )
    {
        const bool isDebug = std::find( forbidden.begin(), forbidden.end(), field.Name ) != forbidden.end();
        EXPECT_FALSE( isDebug )
             << "SceneSettings::" << field.Name
             << " is a field of Graphic::DebugViewState, so it is what a VIEW is drawing rather than "
                "anything the world is - and being reflected here means every scene file states it. This "
                "is the shape that put `ShowColliders: true` in 55 of 80 committed scenes.";
    }
}

// THE TRIPWIRE FOR THE FIELD NOBODY HAS WRITTEN YET. The check above only knows the names that already
// live in DebugViewState; a brand-new `ShowLightRadii` added straight to SceneSettings would pass it.
//
// This is a HEURISTIC and is stated as one: it recognises the shapes this project actually names debug
// visualizations with. It is deliberately biased towards red — a legitimate scene property that trips it
// wants renaming or an argument here, and either is a conversation worth having, whereas a debug flag
// that slips in silently is the defect. `EnableShadows` and `ShadowBias` are the near misses, and neither
// begins with "Show" nor contains "Debug".
TEST( SceneDebugFields, NoSceneSettingIsSHAPEDLikeADebugVisualization )
{
    const auto contains = []( const std::string& haystack, const char* needle )
    { return haystack.find( needle ) != std::string::npos; };

    for ( const FieldInfo& field : SceneSettingsType().Fields )
    {
        const bool named = field.Name.rfind( "Show", 0 ) == 0 || contains( field.Name, "Debug" ) ||
                           contains( field.Name, "Wireframe" ) || contains( field.Name, "BoundingBox" );
        EXPECT_FALSE( named ) << "SceneSettings::" << field.Name
                              << " is named like a viewport debug visualization. If it is one, it belongs "
                                 "in Graphic::DebugViewState; if it is genuinely level data, rename it or "
                                 "argue the exception in this test.";

        EXPECT_NE( field.Meta.Category, "Debug" )
             << "SceneSettings::" << field.Name
             << " sits in a \"Debug\" category. That category was emptied deliberately - a level file has "
                "no debug section, because debugging is something a viewer does, not something a level is.";
    }
}

// ---------------------------------------------------------------------------------------------------
// 2. THE MIGRATION — it strips exactly the set the struct owns
// ---------------------------------------------------------------------------------------------------

// The two lists are in different TARGETS (the engine's header and the migrator tool's), which is exactly
// the distance across which two statements of one set drift. A field added to DebugViewState but not to
// kDebugViewKeys would leave that key in every file the tool touches, and nothing else would notice.
TEST( SceneDebugFields, TheMigrationStripsExactlyTheFieldsTheViewStateOwns )
{
    std::vector<std::string> fromStruct = ForbiddenKeys();
    std::vector<std::string> fromTool   = MigrationKeys( ReadAll( RepoRoot() + kMigrationHeader ) );
    ASSERT_FALSE( fromStruct.empty() );
    ASSERT_FALSE( fromTool.empty() );

    std::sort( fromStruct.begin(), fromStruct.end() );
    std::sort( fromTool.begin(), fromTool.end() );

    EXPECT_EQ( fromStruct, fromTool )
         << "Graphic::DebugViewState and Migration::kDebugViewKeys disagree about which keys leave a "
            "scene. A name in the struct and not in the tool stays in every file the migrator writes.";
}

// ---------------------------------------------------------------------------------------------------
// 3. THE CORPUS — the load-bearing half, and the one that proves the migration was actually RUN
// ---------------------------------------------------------------------------------------------------

// A FAILURE HERE IS NOT A BROKEN TEST. It means a real file in this tree carries a viewport flag, and the
// fix is one command: Tools/SceneMigrator over it. The sweep is recursive, so it covers Scenes/Autosave —
// gitignored editor crash-recovery files, which are the ones most likely to have been written by a build
// that still serialized these keys, and which the version gate will refuse for the same reason.
TEST( SceneDebugFieldsCorpus, NoSceneOnDiskStatesAnyDebugVisualization )
{
    const std::vector<std::string> forbidden = ForbiddenKeys();
    ASSERT_FALSE( forbidden.empty() );

    const std::vector<std::filesystem::path> scenes = RepositoryScenes();
    ASSERT_GE( scenes.size(), 40u );

    for ( const auto& path : scenes )
    {
        const auto parsed = Common::Json::Parse( ReadAll( path ) );
        ASSERT_TRUE( parsed.IsSuccess() ) << path.string() << " is not readable JSON";

        const Common::Json::Node root = Common::Json::Root( parsed.GetValue() );
        ASSERT_EQ( root.GetKind(), Common::Json::Kind::Object ) << path.string() << " is not a JSON object";

        const auto settings = root.Find( "Settings" );
        if ( !settings.has_value() )
            continue; // a scene stating no settings at all states no debug flag either

        if ( settings->GetKind() != Common::Json::Kind::Object )
            continue; // malformed, and SceneVersionGate is the suite that fails on that

        for ( const std::string& key : forbidden )
            EXPECT_FALSE( settings->Find( key ).has_value() )
                 << path.string() << " states Settings." << key
                 << " - a viewport debug flag in a level file. Run Tools/SceneMigrator over it.";
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
