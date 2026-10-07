// ENG-LEVEL: Core::OpenLevel is the one way a game changes level, and it changes it AT THE FRAME BOUNDARY.
//
// What is pinned, against the real resolver (a temp .deproj opened through ProjectContext) and the real Lua
// binding (Scripting::RegisterLevelBindings in a real sol2 state):
//   * a request from C++ or from Lua only QUEUES -- nothing is loaded until the host's TickTravel;
//   * the boundary applies exactly once, and a travel asked for DURING the load waits for the next one;
//   * the last request of a frame wins (UEngine::SetClientTravel overwrites TravelURL);
//   * a level that does not exist is refused with the path that was tried, nothing is queued, and a travel
//     already queued is not disturbed -- there is no fallback map;
//   * an empty name is the project's default map (an empty FURL map is GameDefaultMap in UE).

#include <Engine/Core/LevelTravel.hpp>
#include <Engine/Project/ProjectContext.hpp>
#include <Engine/Scripting/Internal/ScriptRuntime.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    namespace fs   = std::filesystem;
    using Travel   = Desert::Core::LevelTravel;
    using Project_ = Desert::Project::ProjectContext;

    void Touch( const fs::path& p, const std::string& text = "{}" )
    {
        fs::create_directories( p.parent_path() );
        std::ofstream( p ) << text;
    }

    // A project folder with two levels and `defaultScene` as its default map.
    fs::path OpenProject( const std::string& defaultScene )
    {
        const fs::path dir = fs::temp_directory_path() / "desert-level-travel";
        fs::remove_all( dir );
        Touch( dir / "Content/Scenes/Menu.desce" );
        Touch( dir / "Content/Scenes/Arena.desce" );
        Touch( dir / "Game.deproj", R"({"FileVersion":1,"Name":"Game","AssetsRoot":"Content","DefaultScene":")" +
                                         defaultScene + R"(","Description":"","EngineVersion":""})" );
        EXPECT_TRUE( Project_::Open( ( dir / "Game.deproj" ).string(), Project_::RecordInRecent::No ) );
        Travel::Get().Cancel();
        return dir;
    }

    std::string Abs( const fs::path& dir, const char* rel )
    {
        return ( dir / rel ).string();
    }

    // The host's boundary, recording what it was handed.
    struct Host
    {
        std::vector<std::string> Loaded;

        bool Tick()
        {
            const auto r = Travel::Get().TickTravel(
                 [this]( const std::string& path )
                 {
                     Loaded.push_back( path );
                     return Common::MakeSuccess( true );
                 } );
            EXPECT_TRUE( r ) << r.GetError();
            return r && r.GetValue();
        }
    };

    struct Lua
    {
        Desert::Scripting::ScriptEngine::Impl Impl;

        Lua()
        {
            Impl.Lua.open_libraries( sol::lib::base, sol::lib::string );
            Desert::Scripting::RegisterLevelBindings( Impl );
        }

        sol::protected_function_result Run( const std::string& code )
        {
            auto r = Impl.Lua.safe_script( code, sol::script_pass_on_error );
            if ( !r.valid() )
            {
                const sol::error err = r;
                ADD_FAILURE() << err.what();
            }
            return r;
        }
    };
} // namespace

TEST( LevelTravel, ACppRequestIsAppliedAtTheNextBoundaryAndOnlyThere )
{
    const fs::path dir = OpenProject( "Content/Scenes/Menu.desce" );
    Host           host;

    ASSERT_TRUE( Desert::Core::OpenLevel( "Content/Scenes/Arena.desce" ) );
    EXPECT_TRUE( Travel::Get().HasPending() );
    EXPECT_TRUE( host.Loaded.empty() ) << "OpenLevel loaded mid-frame instead of queueing";

    EXPECT_TRUE( host.Tick() );
    ASSERT_EQ( host.Loaded.size(), 1u );
    EXPECT_EQ( host.Loaded[0], Abs( dir, "Content/Scenes/Arena.desce" ) );

    EXPECT_FALSE( host.Tick() ) << "the boundary applied the same travel twice";
    EXPECT_EQ( host.Loaded.size(), 1u );
}

TEST( LevelTravel, TheLastRequestOfAFrameWins )
{
    const fs::path dir = OpenProject( "Content/Scenes/Menu.desce" );
    Host           host;
    ASSERT_TRUE( Desert::Core::OpenLevel( "Content/Scenes/Arena.desce" ) );
    ASSERT_TRUE( Desert::Core::OpenLevel( "Content/Scenes/Menu.desce" ) );
    EXPECT_TRUE( host.Tick() );
    ASSERT_EQ( host.Loaded.size(), 1u );
    EXPECT_EQ( host.Loaded[0], Abs( dir, "Content/Scenes/Menu.desce" ) );
}

TEST( LevelTravel, ATravelAskedForDuringTheLoadWaitsForTheNextBoundary )
{
    const fs::path           dir = OpenProject( "Content/Scenes/Menu.desce" );
    std::vector<std::string> loaded;
    ASSERT_TRUE( Desert::Core::OpenLevel( "Content/Scenes/Arena.desce" ) );

    // The new level's BeginPlay opens another one while it is being loaded.
    const auto first = Travel::Get().TickTravel(
         [&]( const std::string& path )
         {
             loaded.push_back( path );
             EXPECT_TRUE( Desert::Core::OpenLevel( "Content/Scenes/Menu.desce" ) );
             return Common::MakeSuccess( true );
         } );
    ASSERT_TRUE( first );
    ASSERT_EQ( loaded.size(), 1u ) << "the travel requested during the load was applied inside it";
    EXPECT_TRUE( Travel::Get().HasPending() ) << "the travel requested during the load was lost";

    Host host;
    EXPECT_TRUE( host.Tick() );
    ASSERT_EQ( host.Loaded.size(), 1u );
    EXPECT_EQ( host.Loaded[0], Abs( dir, "Content/Scenes/Menu.desce" ) );
}

TEST( LevelTravel, AMissingLevelIsRefusedWithItsPathAndNothingIsQueued )
{
    const fs::path dir = OpenProject( "Content/Scenes/Menu.desce" );
    ASSERT_TRUE( Desert::Core::OpenLevel( "Content/Scenes/Arena.desce" ) );

    const auto refused = Desert::Core::OpenLevel( "Content/Scenes/Nowhere.desce" );
    ASSERT_FALSE( refused );
    EXPECT_NE( refused.GetError().find( Abs( dir, "Content/Scenes/Nowhere.desce" ) ), std::string::npos )
         << refused.GetError();

    // The travel already queued is the one still queued: a refusal is not a fallback.
    ASSERT_TRUE( Travel::Get().Pending().has_value() );
    EXPECT_EQ( *Travel::Get().Pending(), Abs( dir, "Content/Scenes/Arena.desce" ) );
}

TEST( LevelTravel, AnEmptyNameIsTheProjectsDefaultMap )
{
    const fs::path dir = OpenProject( "Content/Scenes/Menu.desce" );
    Host           host;
    ASSERT_TRUE( Desert::Core::OpenLevel( "" ) );
    EXPECT_TRUE( host.Tick() );
    ASSERT_EQ( host.Loaded.size(), 1u );
    EXPECT_EQ( host.Loaded[0], Abs( dir, "Content/Scenes/Menu.desce" ) );
}

TEST( LevelTravel, NoDefaultMapIsARefusalNotAGuess )
{
    (void)OpenProject( "" );
    const auto refused = Desert::Core::OpenLevel( "" );
    ASSERT_FALSE( refused );
    EXPECT_NE( refused.GetError().find( "default map" ), std::string::npos ) << refused.GetError();
    EXPECT_FALSE( Travel::Get().HasPending() );
}

TEST( LevelTravel, ALoadFailureAtTheBoundaryIsReturnedToTheHost )
{
    (void)OpenProject( "Content/Scenes/Menu.desce" );
    ASSERT_TRUE( Desert::Core::OpenLevel( "Content/Scenes/Arena.desce" ) );
    const auto r = Travel::Get().TickTravel( []( const std::string& )
                                             { return Common::MakeError<bool>( "teardown refused" ); } );
    ASSERT_FALSE( r );
    EXPECT_EQ( r.GetError(), "teardown refused" );
}

TEST( LevelTravel, LuaLevelOpenQueuesTheSameTravel )
{
    const fs::path dir = OpenProject( "Content/Scenes/Menu.desce" );
    Lua            lua;
    Host           host;
    lua.Run( R"(ok = level.open( "Content/Scenes/Arena.desce" ))" );
    EXPECT_TRUE( lua.Impl.Lua.get<bool>( "ok" ) );
    EXPECT_TRUE( host.Loaded.empty() ) << "level.open loaded inside the script call";

    EXPECT_TRUE( host.Tick() );
    ASSERT_EQ( host.Loaded.size(), 1u );
    EXPECT_EQ( host.Loaded[0], Abs( dir, "Content/Scenes/Arena.desce" ) );

    lua.Run( "level.open()" );
    EXPECT_TRUE( host.Tick() );
    ASSERT_EQ( host.Loaded.size(), 2u );
    EXPECT_EQ( host.Loaded[1], Abs( dir, "Content/Scenes/Menu.desce" ) );
}

TEST( LevelTravel, LuaHearsTheRefusalWithThePath )
{
    const fs::path dir = OpenProject( "Content/Scenes/Menu.desce" );
    Lua            lua;
    lua.Run( R"(ok, why = level.open( "Content/Scenes/Nowhere.desce" ))" );
    EXPECT_FALSE( lua.Impl.Lua.get<bool>( "ok" ) );
    const std::string why = lua.Impl.Lua.get<std::string>( "why" );
    EXPECT_NE( why.find( Abs( dir, "Content/Scenes/Nowhere.desce" ) ), std::string::npos ) << why;
    EXPECT_FALSE( Travel::Get().HasPending() );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
