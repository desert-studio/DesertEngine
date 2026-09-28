// UIL1: a script's ui.list_* calls reach the collection a UIListView reads.
//
// Runs the REAL registration (Scripting::RegisterUIBindings) in a real sol2 state, so what is pinned is
// the table a gameplay script sees, not a re-statement of it: the 1-based index a script passes lands on
// the 0-based record the list draws, a record's fields keep their types, and a bad write comes back to the
// script as ( false, reason ) instead of vanishing.

#include <Engine/Scripting/Internal/ScriptRuntime.hpp>
#include <Engine/UI/UIDataStore.hpp>

#include <gtest/gtest.h>

#include <string>

namespace
{
    namespace UI = Desert::UI;

    struct Lua
    {
        Desert::Scripting::ScriptEngine::Impl Impl;

        Lua()
        {
            UI::UIDataStore::Get().Clear();
            Impl.Lua.open_libraries( sol::lib::base, sol::lib::string );
            Desert::Scripting::RegisterUIBindings( Impl );
        }

        void Run( const std::string& code )
        {
            const auto r = Impl.Lua.safe_script( code, sol::script_pass_on_error );
            if ( !r.valid() )
            {
                // sol2's documented conversion; MSVC rejects the functional cast sol::error( r ) (C2440).
                const sol::error err = r;
                FAIL() << err.what();
            }
        }
    };

    const UI::UICollection& Chat()
    {
        const UI::UICollection* c = UI::UIDataStore::Get().FindCollection( "chat" );
        EXPECT_NE( c, nullptr );
        return *c;
    }
} // namespace

TEST( UIScriptCollections, ARecordWrittenByAScriptIsTheRecordTheListReads )
{
    Lua lua;
    lua.Run( R"(ui.list_add( "chat", { from = "Ann", n = 3, seen = true, tint = { 1, 0.5, 0 } } ))" );
    ASSERT_EQ( Chat().Size(), 1 );
    const UI::UIDataStore& r = Chat().Record( 0 );
    EXPECT_EQ( r.Text( "from" ).value_or( "" ), "Ann" );
    EXPECT_EQ( r.Number( "n" ).value_or( 0.0 ), 3.0 );
    EXPECT_EQ( r.Bool( "seen" ).value_or( false ), true );
    EXPECT_EQ( r.Color( "tint" ).value_or( glm::vec3( 0.0f ) ), glm::vec3( 1.0f, 0.5f, 0.0f ) );
}

TEST( UIScriptCollections, LuaIndicesAreOneBasedAndTranslatedOnce )
{
    Lua lua;
    lua.Run( R"(
        for i = 1, 3 do ui.list_add( "chat", { n = i } ) end
        ui.list_insert( "chat", 1, { n = 0 } )  -- becomes the first record
        ui.list_set( "chat", 2, "n", 10 )       -- the second record: the one that was n = 1
        ui.list_remove( "chat", 4 )             -- the fourth: n = 3
        count = ui.list_count( "chat" )
    )" );
    EXPECT_EQ( lua.Impl.Lua.get<int>( "count" ), 3 );
    ASSERT_EQ( Chat().Size(), 3 );
    EXPECT_EQ( Chat().Record( 0 ).Number( "n" ).value_or( -1 ), 0.0 );
    EXPECT_EQ( Chat().Record( 1 ).Number( "n" ).value_or( -1 ), 10.0 );
    EXPECT_EQ( Chat().Record( 2 ).Number( "n" ).value_or( -1 ), 2.0 );
}

TEST( UIScriptCollections, ABadWriteIsReportedToTheScriptAndChangesNothing )
{
    Lua lua;
    lua.Run( R"(
        ui.list_add( "chat", { n = 1 } )
        okRemove, whyRemove = ui.list_remove( "chat", 5 )
        okAdd, whyAdd = ui.list_add( "chat", { bad = { 1, 2 } } )
        ui.list_clear( "chat" )
        afterClear = ui.list_count( "chat" )
        missing = ui.list_count( "nobody" )
    )" );
    auto& L = lua.Impl.Lua;
    EXPECT_FALSE( L.get<bool>( "okRemove" ) );
    EXPECT_NE( L.get<std::string>( "whyRemove" ).find( "4 is outside [0, 1)" ), std::string::npos )
         << L.get<std::string>( "whyRemove" );
    EXPECT_FALSE( L.get<bool>( "okAdd" ) );
    EXPECT_NE( L.get<std::string>( "whyAdd" ).find( "'bad'" ), std::string::npos )
         << L.get<std::string>( "whyAdd" );
    EXPECT_EQ( L.get<int>( "afterClear" ), 0 );
    EXPECT_EQ( L.get<int>( "missing" ), 0 );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
