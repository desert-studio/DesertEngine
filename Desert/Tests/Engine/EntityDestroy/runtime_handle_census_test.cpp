// Census: every runtime handle an entity holds outside the registry has a release path — see
// runtime_handle_register.hpp for what is registered and why the register exists.
//
// WHAT IT FORBIDS: (1) a component field that holds a system handle by number — its type is a
// `Physics::*Handle`, or its name starts with `Runtime` and its type is not an owning pointer — with no row
// naming the `on_destroy<Component>` listener that releases it; (2) a member table keyed by `entt::entity`
// anywhere in the engine with no row naming how destroyed entities leave it; (3) a row whose component,
// member or evidence is no longer in the file it names.
//
// Both scans read comment-stripped SOURCE TEXT: the question is about the relation between a declaration in
// one file and a release call in another, and no run-time test on a bare registry can see a system it does
// not construct.

#include "runtime_handle_register.hpp"

#include "../SettingConsumers/setting_consumers_reader.hpp"

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
    using namespace Desert::Tests;
    using namespace Desert::Tests::RuntimeHandles;

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 8; ++up )
        {
            if ( fs::exists( prefix + "Desert/Desert/Source/Engine/ECS/Components.hpp" ) )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadAll( const fs::path& path )
    {
        std::ifstream      in( path );
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    std::string Code( const fs::path& path )
    {
        return ConsumerText::StripComments( ReadAll( path ) );
    }

    std::string Trim( std::string s )
    {
        const auto notSpace = []( unsigned char c ) { return !std::isspace( c ); };
        s.erase( s.begin(), std::find_if( s.begin(), s.end(), notSpace ) );
        s.erase( std::find_if( s.rbegin(), s.rend(), notSpace ).base(), s.end() );
        return s;
    }

    struct Field
    {
        std::string Component;
        std::string Type;
        std::string Name;
    };

    // Every data member declared directly in a `struct ...Component {` body of @p src. A member is a
    // depth-1 statement ending in `;` that is not a function, a using, or a nested type.
    std::vector<Field> ComponentFields( const std::string& src )
    {
        std::vector<Field> out;
        for ( std::size_t at : ConsumerText::WordPositions( src, "struct" ) )
        {
            std::size_t       i    = ConsumerText::SkipSpace( src, at + 6 );
            const std::string name = ConsumerText::IdentAt( src, i );
            if ( name.size() < 9 || name.compare( name.size() - 9, 9, "Component" ) != 0 )
                continue;
            i = ConsumerText::SkipSpace( src, i + name.size() );
            if ( i >= src.size() || src[i] != '{' )
                continue;

            int         depth = 1;
            std::string stmt;
            for ( ++i; i < src.size() && depth > 0; ++i )
            {
                const char c = src[i];
                if ( c == '{' || c == '(' )
                {
                    if ( depth == 1 && c == '{' && stmt.find( '=' ) == std::string::npos &&
                         stmt.find( '(' ) == std::string::npos )
                    {
                        // A brace initialiser `T name{...}` is a member; a nested type or a body is not.
                        const std::string t = Trim( stmt );
                        if ( t.rfind( "struct", 0 ) == 0 || t.rfind( "enum", 0 ) == 0 ||
                             t.rfind( "class", 0 ) == 0 )
                            stmt += "\x01"; // poison: nested type
                    }
                    ++depth;
                    stmt += c;
                    continue;
                }
                if ( c == '}' || c == ')' )
                {
                    --depth;
                    stmt += c;
                    if ( depth != 1 )
                        continue;
                    // A closed nested type or function body ends its own statement (no `;` follows a body).
                    if ( c == '}' &&
                         ( stmt.find( '\x01' ) != std::string::npos || stmt.find( '(' ) != std::string::npos ) )
                        stmt.clear();
                    // A bare annotation macro — REFLECT(), PROPERTY( ... ) — carries no `;` either and would
                    // otherwise swallow the member it annotates.
                    if ( c == ')' )
                    {
                        const std::string t     = Trim( stmt );
                        const std::size_t paren = t.find( '(' );
                        const std::string head  = Trim( t.substr( 0, paren ) );
                        const bool        macro =
                             !head.empty() &&
                             std::all_of( head.begin(), head.end(),
                                          []( char h ) {
                                              return std::isupper( static_cast<unsigned char>( h ) ) || h == '_';
                                          } );
                        if ( macro )
                            stmt.clear();
                    }
                    continue;
                }
                if ( depth == 1 && c == ';' )
                {
                    std::string t = Trim( stmt );
                    stmt.clear();
                    // Drop an initialiser: `T name = v` / `T name{ v }` / `T name( v )` is not legal for members.
                    const std::size_t eq  = t.find( '=' );
                    const std::size_t br  = t.find( '{' );
                    std::size_t       cut = std::min( eq, br );
                    if ( cut != std::string::npos )
                        t = Trim( t.substr( 0, cut ) );
                    if ( t.empty() || t.find( '(' ) != std::string::npos || t.rfind( "using", 0 ) == 0 ||
                         t.rfind( "static", 0 ) == 0 || t.rfind( "friend", 0 ) == 0 ||
                         t.find( '\x01' ) != std::string::npos )
                        continue;
                    std::size_t end = t.size();
                    std::size_t beg = end;
                    while ( beg > 0 && ConsumerText::IsIdentChar( t[beg - 1] ) )
                        --beg;
                    if ( beg == end || beg == 0 )
                        continue;
                    out.push_back( { name, Trim( t.substr( 0, beg ) ), t.substr( beg ) } );
                    continue;
                }
                if ( depth == 1 && ( c == ':' ) && Trim( stmt ).rfind( "public", 0 ) == 0 )
                {
                    stmt.clear();
                    continue;
                }
                stmt += c;
            }
        }
        return out;
    }

    bool OwnsItsResource( const std::string& type )
    {
        // An owning pointer (or a container of them) is released by the component's own destructor when EnTT
        // drops the component; nothing needs to listen for it.
        return type.find( "shared_ptr" ) != std::string::npos || type.find( "unique_ptr" ) != std::string::npos ||
               ( type.size() >= 3 && type.find( "Ptr" ) != std::string::npos );
    }

    bool HoldsSystemHandle( const Field& f )
    {
        const bool physicsHandle = f.Type.rfind( "Physics::", 0 ) == 0 && f.Type.size() > 6 &&
                                   f.Type.compare( f.Type.size() - 6, 6, "Handle" ) == 0;
        const bool runtimeNumber = f.Name.rfind( "Runtime", 0 ) == 0 && !OwnsItsResource( f.Type );
        return physicsHandle || runtimeNumber;
    }

    std::vector<fs::path> ComponentHeaders( const std::string& root )
    {
        std::vector<fs::path> out;
        for ( const auto& e : fs::directory_iterator( root + "Desert/Desert/Source/Engine/ECS" ) )
            if ( e.is_regular_file() && e.path().extension() == ".hpp" )
                out.push_back( e.path() );
        std::sort( out.begin(), out.end() );
        return out;
    }

    std::string Collapse( const std::string& s )
    {
        std::string out;
        for ( char c : s )
            if ( !std::isspace( static_cast<unsigned char>( c ) ) )
                out += c;
        return out;
    }

    bool Contains( const std::string& code, std::string_view needle )
    {
        return Collapse( code ).find( Collapse( std::string( needle ) ) ) != std::string::npos;
    }

    // `map<entt::entity, ...> Name;` member declarations in @p src (whitespace-insensitive).
    std::vector<std::string> EntityKeyedMembers( const std::string& src )
    {
        std::vector<std::string> out;
        for ( std::size_t at = src.find( "map<" ); at != std::string::npos; at = src.find( "map<", at + 4 ) )
        {
            std::size_t i = ConsumerText::SkipSpace( src, at + 4 );
            if ( src.compare( i, 12, "entt::entity" ) != 0 )
                continue;
            int depth = 1;
            for ( i = at + 4; i < src.size() && depth > 0; ++i )
            {
                if ( src[i] == '<' )
                    ++depth;
                else if ( src[i] == '>' )
                    --depth;
            }
            i                      = ConsumerText::SkipSpace( src, i );
            const std::string name = ConsumerText::IdentAt( src, i );
            if ( name.empty() )
                continue;
            const std::size_t after = ConsumerText::SkipSpace( src, i + name.size() );
            if ( after < src.size() && ( src[after] == ';' || src[after] == '{' || src[after] == '=' ) )
                out.push_back( name );
        }
        return out;
    }
} // namespace

TEST( RuntimeHandleCensus, EveryComponentHandleFieldHasARegisteredRelease )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";

    std::set<std::pair<std::string, std::string>> found;
    std::set<std::pair<std::string, std::string>> seen;
    std::size_t                                   scanned = 0;
    for ( const auto& header : ComponentHeaders( root ) )
    {
        for ( const Field& f : ComponentFields( Code( header ) ) )
        {
            ++scanned;
            seen.insert( { f.Component, f.Name } );
            if ( !HoldsSystemHandle( f ) )
                continue;
            found.insert( { f.Component, f.Name } );
            const bool registered = std::any_of( kComponentHandles.begin(), kComponentHandles.end(),
                                                 [&]( const ComponentHandleRow& r )
                                                 { return r.Component == f.Component && r.Field == f.Name; } );
            EXPECT_TRUE( registered ) << f.Component << "::" << f.Name << " (" << f.Type << ", "
                                      << header.filename().string()
                                      << ") holds a system handle by number and has no row in "
                                         "runtime_handle_register.hpp. Destroying its entity would leave the "
                                         "resource behind; add an on_destroy<"
                                      << f.Component << "> listener that releases it, and the row.";
        }
    }
    // The scan must SEE the component set, or every assertion above is vacuous: ask it for fields that are
    // known to be there, including ones that sit under a PROPERTY(...) annotation and after a REFLECT().
    EXPECT_GT( scanned, 100u ) << "the field scan found almost nothing; the parser is broken";
    for ( const auto& [component, field] :
          std::vector<std::pair<std::string, std::string>>{ { "TransformComponent", "Translation" },
                                                            { "StaticMeshComponent", "MeshHandle" },
                                                            { "CharacterControllerComponent", "VerticalVelocity" },
                                                            { "RigidBodyComponent", "Data" } } )
        EXPECT_TRUE( seen.count( { component, field } ) )
             << "the field scan no longer sees " << component << "::" << field;

    for ( const ComponentHandleRow& row : kComponentHandles )
    {
        EXPECT_TRUE( found.count( { std::string( row.Component ), std::string( row.Field ) } ) )
             << "stale row: " << row.Component << "::" << row.Field << " is no longer a handle field";
        const std::string code = Code( root + std::string( row.ReleasedIn ) );
        ASSERT_FALSE( code.empty() ) << row.ReleasedIn << " is missing";
        // , not the bare signal name: a Detach() that DISconnects names the signal too, and
        // that alone satisfied this check while nothing was listening (mutation M3, WP6).
        EXPECT_TRUE( Contains( code, "on_destroy<" + std::string( row.Component ) + ">().connect" ) )
             << row.ReleasedIn << " does not connect a listener to on_destroy<" << row.Component << ">";
        EXPECT_TRUE( Contains( code, std::string( row.ReleaseCall ) + "(" ) )
             << row.ReleasedIn << " never calls " << row.ReleaseCall;
    }
}

TEST( RuntimeHandleCensus, EveryEntityKeyedTableHasARegisteredRelease )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    std::set<std::pair<std::string, std::string>> found;
    for ( const auto& e : fs::recursive_directory_iterator( root + "Desert/Desert/Source/Engine" ) )
    {
        const auto ext = e.path().extension();
        if ( !e.is_regular_file() || ( ext != ".hpp" && ext != ".cpp" && ext != ".h" ) )
            continue;
        const std::string rel = fs::relative( e.path(), root ).generic_string();
        for ( const std::string& member : EntityKeyedMembers( Code( e.path() ) ) )
        {
            found.insert( { rel, member } );
            const bool registered =
                 std::any_of( kEntityTables.begin(), kEntityTables.end(),
                              [&]( const EntityTableRow& r ) { return r.File == rel && r.Member == member; } );
            EXPECT_TRUE( registered ) << rel << ": `" << member
                                      << "` is keyed by entt::entity and has no row in "
                                         "runtime_handle_register.hpp. Say how a destroyed entity leaves it.";
        }
    }

    for ( const EntityTableRow& row : kEntityTables )
    {
        const std::string declared = Code( root + std::string( row.File ) );
        ASSERT_FALSE( declared.empty() ) << row.File << " is missing";
        EXPECT_FALSE( ConsumerText::WordPositions( declared, std::string( row.Member ) ).empty() )
             << "stale row: " << row.File << " no longer declares " << row.Member;
        const std::string evidence = Code( root + std::string( row.EvidenceFile ) );
        EXPECT_TRUE( Contains( evidence, row.Evidence ) )
             << row.File << "::" << row.Member << ": the release evidence `" << row.Evidence << "` is not in "
             << row.EvidenceFile;
        if ( row.How == Release::Exception )
            std::printf( "[ EXCEPTION] %s::%s — %s\n", std::string( row.File ).c_str(),
                         std::string( row.Member ).c_str(), std::string( row.Why ).c_str() );
    }

    // Instrument check: the scan has to find the tables the register already knows about, or a green here
    // would only mean the regex went blind.
    EXPECT_TRUE( found.count( { "Desert/Desert/Source/Engine/ECS/System/AudioECSSystem.hpp", "m_Sources" } ) )
         << "the entity-keyed table scan no longer sees AudioECSSystem::m_Sources";
    EXPECT_GE( found.size(), 9u );
}
