// Does every component type a system touches have its pool created BEFORE the parallel phase?
//
// THE DEFECT THIS PINS. EnTT creates a component pool on the FIRST touch of a type, and every path that
// touches one -- view<T...>(), has<T>(), get<T>(), try_get<T>() -- goes through
// basic_registry::assure<T>(). assure MUTATES, through the const overload as well, because `pools` is
// `mutable`. A first touch does three unsynchronised writes: it takes an index from ONE global counter,
// it grows `pools` (a std::vector), and it stores a new pool at that index.
//
// Scene::ExecuteSystems runs maximal runs of CanRunParallel() systems on several threads at once. Two
// collectors that first-touch any type in the same frame therefore race on a std::vector grow, and the
// symptom is `std::length_error: vector` -- a vector reading a torn size -- thrown out of the FIRST
// frame, roughly one headless run in fifty. It is not reproducible under a debugger and macOS writes no
// report for it, which is why it survived a whole phase as an open defect.
//
// Scene::PrepareComponentPools closes it by creating every pool serially before any group opens. That
// only works while the list there is COMPLETE, and completeness is not something the compiler can check:
// a `has<NewComponent>` added to a collector tomorrow compiles, runs, and silently reopens the race in
// one frame out of fifty.
//
// So this suite reads the system headers as TEXT, extracts every component type they touch, and requires
// each one to be prepared. It is the same shape as the SettingConsumers audit next door and for the same
// reason: the relation between two files is what is wrong, so the assertion has to be about both.

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    // The repository root, found by walking up from wherever the binary was started - the same approach
    // the SettingConsumers audit uses, so neither has to be run from one exact directory.
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/ECS/Components.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadFile( const std::string& path )
    {
        std::ifstream in( path );
        if ( !in )
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // Every `<Something>Component` that appears as the template argument of a registry call in @p source.
    //
    // WHY A TEXT SCAN AND NOT THE TYPE SYSTEM. The set being audited is "types this file asks the
    // registry about", and that is a property of the call sites, not of any type. Linking the systems and
    // reflecting over them would need a GPU, a scene and a job pool; reading them finds the same call
    // sites in a millisecond and fails for the right reason when someone adds one.
    std::set<std::string> TouchedComponents( const std::string& source )
    {
        static const std::vector<std::string> calls = { "view<",   "has<",    "get<",    "try_get<",
                                                        "any_of<", "all_of<", "remove<", "emplace<" };

        std::set<std::string> found;
        for ( const auto& call : calls )
        {
            for ( std::size_t at = source.find( call ); at != std::string::npos; at = source.find( call, at + 1 ) )
            {
                // `get<` also matches `try_get<`; harmless, both name the same type.
                std::size_t cursor = at + call.size();

                // A call may name several types: view<A, B>. Walk the argument list to the closing '>'.
                while ( cursor < source.size() && source[cursor] != '>' && source[cursor] != ';' &&
                        source[cursor] != '(' )
                {
                    while ( cursor < source.size() &&
                            ( std::isspace( static_cast<unsigned char>( source[cursor] ) ) ||
                              source[cursor] == ',' ) )
                        ++cursor;

                    const std::size_t start = cursor;
                    while ( cursor < source.size() &&
                            ( std::isalnum( static_cast<unsigned char>( source[cursor] ) ) ||
                              source[cursor] == '_' || source[cursor] == ':' ) )
                        ++cursor;

                    if ( cursor == start )
                        break;

                    std::string name = source.substr( start, cursor - start );
                    if ( const auto colon = name.rfind( ':' ); colon != std::string::npos )
                        name = name.substr( colon + 1 );

                    if ( name.size() > 9 && name.compare( name.size() - 9, 9, "Component" ) == 0 )
                        found.insert( name );
                }
            }
        }
        return found;
    }

    constexpr const char* kSystemDir = "Desert/Desert/Source/Engine/ECS/System";
    constexpr const char* kScene     = "Desert/Desert/Source/Engine/Core/Scene.cpp";
} // namespace

// The audit. Every component type any ECS system asks the registry about must be prepared by
// Scene::PrepareComponentPools, so that no pool is ever created while a parallel group is open.
TEST( ComponentPools, EverySystemTouchedComponentIsPreparedBeforeTheParallelPhase )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    const std::string scene = ReadFile( root + kScene );
    ASSERT_FALSE( scene.empty() ) << "could not read " << kScene;

    // The prepare list, read from the function that owns it rather than from the whole file: a type
    // merely MENTIONED elsewhere in Scene.cpp is not a type whose pool was created.
    const std::size_t listStart = scene.find( "void Scene::PrepareComponentPools()" );
    ASSERT_NE( listStart, std::string::npos ) << "Scene::PrepareComponentPools has been renamed or removed "
                                                 "-- the race it closes is described at its definition";
    const std::size_t listEnd = scene.find( "void Scene::ExecuteSystems", listStart );
    ASSERT_NE( listEnd, std::string::npos );

    const std::string prepareBody = scene.substr( listStart, listEnd - listStart );

    std::set<std::string> prepared;
    for ( std::size_t at = prepareBody.find( "prepare<" ); at != std::string::npos;
          at             = prepareBody.find( "prepare<", at + 1 ) )
    {
        // A COMMENTED-OUT LINE PREPARES NOTHING. Without this the audit accepts the one edit most likely
        // to be made in a hurry -- commenting a line out to see what breaks -- and reports the pool as
        // created while the race is open again. Verified by making exactly that edit.
        const std::size_t lineStart = prepareBody.rfind( '\n', at );
        const std::string line      = prepareBody.substr( lineStart == std::string::npos ? 0 : lineStart + 1,
                                                     at - ( lineStart == std::string::npos ? 0 : lineStart + 1 ) );
        if ( line.find( "//" ) != std::string::npos )
            continue;

        const std::size_t start = at + std::string( "prepare<" ).size();
        const std::size_t close = prepareBody.find( '>', start );
        ASSERT_NE( close, std::string::npos );

        std::string name = prepareBody.substr( start, close - start );
        if ( const auto colon = name.rfind( ':' ); colon != std::string::npos )
            name = name.substr( colon + 1 );
        prepared.insert( name );
    }

    ASSERT_FALSE( prepared.empty() ) << "no prepare<T>() calls found -- the pools are created lazily again "
                                        "and the first frame can race";

    std::vector<std::string> failures;
    std::size_t              systemsScanned = 0;

    for ( const auto& entry : std::filesystem::directory_iterator( root + kSystemDir ) )
    {
        if ( entry.path().extension() != ".hpp" )
            continue;

        const std::string name = entry.path().filename().string();
        // System.hpp is the base class and SystemRules.hpp is a policy header; neither touches a pool.
        if ( name == "System.hpp" || name == "SystemRules.hpp" )
            continue;

        ++systemsScanned;
        const std::string source = ReadFile( entry.path().string() );
        ASSERT_FALSE( source.empty() ) << "could not read " << name;

        for ( const auto& component : TouchedComponents( source ) )
        {
            if ( prepared.count( component ) == 0 )
                failures.push_back( name + " touches " + component +
                                    ", which PrepareComponentPools does "
                                    "not create" );
        }
    }

    EXPECT_GT( systemsScanned, 10u ) << "the system directory was not found or was not scanned";

    std::string message;
    for ( const auto& f : failures )
        message += "\n  " + f;

    EXPECT_TRUE( failures.empty() )
         << "A component type is first touched inside the parallel phase, which races on EnTT's pool "
            "vector and aborts with std::length_error in roughly one run in fifty. Add a prepare<T>() "
            "line to Scene::PrepareComponentPools for each:"
         << message;
}

// The counter that hands out component type indices must be atomic. Without ENTT_USE_ATOMIC it is a
// plain `value++` on one global, so two threads first-touching two DIFFERENT types can be handed the
// SAME index -- after which each reads the other's storage and any vector in it has a garbage size.
// Preparing the pools serially means the engine's own collectors never reach that counter concurrently,
// but preview scenes, thumbnail scenes and scripts have registries of their own.
TEST( ComponentPools, TheTypeIndexCounterIsAtomic )
{
#ifndef ENTT_USE_ATOMIC
    FAIL() << "ENTT_USE_ATOMIC is not defined for this translation unit. It is set at WORKSPACE scope in "
              "BuildScripts/Workspace.lua because the macro changes the type of a shared static: it has "
              "to be every project or none, or two objects disagree about that static's layout.";
#else
    SUCCEED();
#endif
}
