// ENG-LEVEL: Core::OpenLevel is the one way a game changes level, and it changes it AT THE FRAME BOUNDARY.
//
// What is pinned, against the real resolver (a temp .deproj opened through ProjectContext) and the real Lua
// binding (Scripting::RegisterLevelBindings on the Luau runtime):
//   * a request from C++ or from Lua only QUEUES -- nothing is loaded until the host's TickTravel;
//   * the boundary applies exactly once, and a travel asked for DURING the load waits for the next one;
//   * the last request of a frame wins (UEngine::SetClientTravel overwrites TravelURL);
//   * a level that does not exist is refused with the path that was tried, nothing is queued, and a travel
//     already queued is not disturbed -- there is no fallback map;
//   * an empty name is the project's default map (an empty FURL map is GameDefaultMap in UE).

#include <Engine/Core/LevelTravel.hpp>
#include <Editor/Core/PlayWorldTravel.hpp>
#include <Engine/Project/ProjectContext.hpp>
#include <Engine/Scripting/ScriptEngine.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <format>
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
        Touch( dir / "Game.deproj",
               std::format( R"({{"FileVersion":1,"Name":"Game","AssetsRoot":"Content","DefaultScene":"{}",)"
                            R"("Description":"","EngineVersion":""}})",
                            defaultScene ) );
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

    // The real script engine (Luau): its console keeps globals between lines, so a statement's result is read
    // back as an expression.
    struct Lua
    {
        Desert::Scripting::ScriptEngine Engine{ nullptr };

        void Run( const std::string& code )
        {
            const auto r = Engine.RunString( code );
            if ( !r.IsSuccess() )
                ADD_FAILURE() << r.GetError();
        }

        std::string Get( const std::string& name )
        {
            std::string out;
            const auto  r = Engine.EvalToString( name, out );
            EXPECT_TRUE( r.IsSuccess() ) << r.GetError();
            while ( !out.empty() && out.back() == '\n' )
                out.pop_back();
            return out;
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
    const auto pending = Travel::Get().Pending();
    if ( !pending.has_value() )
        FAIL() << "the queued travel is gone";
    EXPECT_EQ( *pending, Abs( dir, "Content/Scenes/Arena.desce" ) );
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
    EXPECT_EQ( lua.Get( "ok" ), "true" );
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
    EXPECT_EQ( lua.Get( "ok" ), "false" );
    const std::string why = lua.Get( "why" );
    EXPECT_NE( why.find( Abs( dir, "Content/Scenes/Nowhere.desce" ) ), std::string::npos ) << why;
    EXPECT_FALSE( Travel::Get().HasPending() );
}

// ── LEVEL-PIE: OpenLevel in Play-in-editor ─────────────────────────────────────────────────────────────
// The editor's boundary (PlaySession::ServiceTravel) is PlayWorldTravel over the editor's scene. The world here is
// the two strings that matter: what the document holds and what Play shows.
namespace
{
    struct PieWorld
    {
        std::string                     Played = "authored";
        std::vector<std::string>        Loads;
        Desert::Editor::PlayWorldTravel Pie;

        Common::BoolResultStr Tick()
        {
            return Pie.Tick(
                 [this]( const std::string& path )
                 {
                     Loads.push_back( path );
                     Played = path;
                     return Common::MakeSuccess( true );
                 } );
        }
        Common::BoolResultStr Stop()
        {
            return Pie.End(
                 [this]( const std::string& snapshot )
                 {
                     Played = snapshot;
                     return Common::MakeSuccess( true );
                 } );
        }
    };

    struct PokeCommand final : Desert::Editor::ICommand
    {
        bool Undo() override
        {
            return true;
        }
        bool Redo() override
        {
            return true;
        }
    };
} // namespace

TEST( PlayInEditorTravel, ATravelInPlayLoadsTheLevelIntoThePlayedWorldAtTheBoundary )
{
    const fs::path dir = OpenProject( "Content/Scenes/Menu.desce" );
    PieWorld       w;
    w.Pie.Begin( "authored" );
    ASSERT_TRUE( Desert::Core::OpenLevel( "Content/Scenes/Arena.desce" ) );
    EXPECT_TRUE( w.Loads.empty() ) << "applied inside the frame that asked";
    const auto r = w.Tick();
    ASSERT_TRUE( r ) << r.GetError();
    EXPECT_TRUE( r.GetValue() );
    EXPECT_EQ( w.Played, Abs( dir, "Content/Scenes/Arena.desce" ) );
    EXPECT_EQ( w.Pie.CurrentMap(), Abs( dir, "Content/Scenes/Arena.desce" ) );
    EXPECT_EQ( w.Pie.AuthoredSnapshot(), "authored" )
         << "a travel replaced the authored level, not the played one";
}

TEST( PlayInEditorTravel, StopReturnsTheAuthoredLevelWhicheverMapWasPlayedLast )
{
    OpenProject( "Content/Scenes/Menu.desce" );
    PieWorld w;
    w.Pie.Begin( "authored" );
    ASSERT_TRUE( Desert::Core::OpenLevel( "Content/Scenes/Arena.desce" ) );
    ASSERT_TRUE( w.Tick() );
    ASSERT_TRUE( Desert::Core::OpenLevel( "Content/Scenes/Menu.desce" ) );
    ASSERT_TRUE( w.Tick() );
    ASSERT_EQ( w.Loads.size(), 2u );
    // A travel queued in the last frame of Play dies with it.
    ASSERT_TRUE( Desert::Core::OpenLevel( "Content/Scenes/Arena.desce" ) );
    const auto stopped = w.Stop();
    ASSERT_TRUE( stopped ) << stopped.GetError();
    EXPECT_EQ( w.Played, "authored" );
    EXPECT_FALSE( Travel::Get().HasPending() );
    EXPECT_FALSE( w.Pie.Active() );
    EXPECT_TRUE( w.Pie.CurrentMap().empty() );
}

TEST( PlayInEditorTravel, ATravelDoesNotRaiseTheAuthoredDocumentsDirtyStar )
{
    OpenProject( "Content/Scenes/Menu.desce" );
    auto& history = Desert::Editor::CommandHistory::Get();
    history.PushCommand( std::make_unique<PokeCommand>() ); // an edit made before Play...
    const uint64_t saved = history.Revision();              // ...and saved: the star is out
    PieWorld       w;
    w.Pie.Begin( "authored" );
    ASSERT_TRUE( Desert::Core::OpenLevel( "Content/Scenes/Arena.desce" ) );
    ASSERT_TRUE( w.Tick() );
    EXPECT_TRUE( history.UndoStack().empty() ) << "the left world's undo stack would fire into the new one";
    ASSERT_TRUE( w.Stop() );
    EXPECT_EQ( history.Revision(), saved ) << "travelling in Play marked the authored level as edited";
}

TEST( PlayInEditorTravel, AFailedTravelKeepsPlayAndStopStillRestores )
{
    OpenProject( "Content/Scenes/Menu.desce" );
    PieWorld w;
    w.Pie.Begin( "authored" );
    ASSERT_TRUE( Desert::Core::OpenLevel( "Content/Scenes/Arena.desce" ) );
    const auto r =
         w.Pie.Tick( []( const std::string& ) { return Common::MakeFormattedError<bool>( "broken level" ); } );
    ASSERT_FALSE( r );
    EXPECT_NE( r.GetError().find( "broken level" ), std::string::npos );
    EXPECT_TRUE( w.Pie.Active() );
    w.Played = "half-torn-down";
    ASSERT_TRUE( w.Stop() );
    EXPECT_EQ( w.Played, "authored" );
}

TEST( PlayInEditorTravel, OutsidePlayATravelIsRefusedWithItsTargetAndNothingIsLoaded )
{
    const fs::path dir = OpenProject( "Content/Scenes/Menu.desce" );
    PieWorld       w; // never began: the editor is authoring
    ASSERT_TRUE( Desert::Core::OpenLevel( "Content/Scenes/Arena.desce" ) );
    const auto r = w.Tick();
    ASSERT_FALSE( r );
    EXPECT_NE( r.GetError().find( Abs( dir, "Content/Scenes/Arena.desce" ) ), std::string::npos ) << r.GetError();
    EXPECT_TRUE( w.Loads.empty() );
    EXPECT_EQ( w.Played, "authored" );
    EXPECT_FALSE( Travel::Get().HasPending() );
}
