// UIL1: a script's ui.list_* calls reach the collection a UIListView reads.
//
// Runs the REAL registration (Scripting::RegisterUIBindings) on the real Luau runtime, so what is pinned is
// the table a gameplay script sees, not a re-statement of it: the 1-based index a script passes lands on
// the 0-based record the list draws, a record's fields keep their types, and a bad write comes back to the
// script as ( false, reason ) instead of vanishing.

#include <Engine/Scripting/Internal/ScriptRuntime.hpp>
#include <UI/UIDataStore.hpp>

#include <gtest/gtest.h>

#include <map>
#include <string>

namespace
{
    namespace UI        = Desert::UI;
    namespace Scripting = Desert::Scripting;
    namespace Refl      = Desert::Reflection;

    struct Lua
    {
        Scripting::LuauRuntime Runtime{ Scripting::LuauLimits{}, &Scripting::RegisterUIBindings };

        Lua()
        {
            UI::UIDataStore::Get().Clear();
        }

        /// Runs `code` as a script's top level and returns its global `Out` table.
        std::map<std::string, Refl::Value> Run( const std::string& code )
        {
            std::map<std::string, Refl::Value> out;
            auto slot = Runtime.Load( "UIScriptCollections", "Out = {}\n" + code, {} );
            EXPECT_TRUE( slot.IsSuccess() ) << ( slot.IsSuccess() ? "" : slot.GetError() );
            if ( slot.IsSuccess() )
                for ( const Scripting::LuauTableEntry& entry : Runtime.ReadTable( slot.GetValue(), "Out" ) )
                    out.emplace( entry.Key, entry.Value );
            return out;
        }
    };

    const UI::UICollection& Chat()
    {
        const UI::UICollection* c = UI::UIDataStore::Get().FindCollection( "chat" );
        EXPECT_NE( c, nullptr );
        return *c;
    }

    double Number( const std::map<std::string, Refl::Value>& out, const std::string& key )
    {
        const auto    it = out.find( key );
        const double* v  = it == out.end() ? nullptr : it->second.Get<double>();
        return v != nullptr ? *v : -1.0;
    }
    bool Bool( const std::map<std::string, Refl::Value>& out, const std::string& key )
    {
        const auto  it = out.find( key );
        const bool* v  = it == out.end() ? nullptr : it->second.Get<bool>();
        return v != nullptr && *v;
    }
    std::string Text( const std::map<std::string, Refl::Value>& out, const std::string& key )
    {
        const auto         it = out.find( key );
        const std::string* v  = it == out.end() ? nullptr : it->second.Get<std::string>();
        return v != nullptr ? *v : std::string();
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
    Lua        lua;
    const auto out = lua.Run( R"(
        for i = 1, 3 do ui.list_add( "chat", { n = i } ) end
        ui.list_insert( "chat", 1, { n = 0 } )  -- becomes the first record
        ui.list_set( "chat", 2, "n", 10 )       -- the second record: the one that was n = 1
        ui.list_remove( "chat", 4 )             -- the fourth: n = 3
        Out.count = ui.list_count( "chat" )
    )" );
    EXPECT_EQ( Number( out, "count" ), 3.0 );
    ASSERT_EQ( Chat().Size(), 3 );
    EXPECT_EQ( Chat().Record( 0 ).Number( "n" ).value_or( -1 ), 0.0 );
    EXPECT_EQ( Chat().Record( 1 ).Number( "n" ).value_or( -1 ), 10.0 );
    EXPECT_EQ( Chat().Record( 2 ).Number( "n" ).value_or( -1 ), 2.0 );
}

TEST( UIScriptCollections, ABadWriteIsReportedToTheScriptAndChangesNothing )
{
    Lua        lua;
    const auto out = lua.Run( R"(
        ui.list_add( "chat", { n = 1 } )
        Out.okRemove, Out.whyRemove = ui.list_remove( "chat", 5 )
        Out.okAdd, Out.whyAdd = ui.list_add( "chat", { bad = { 1, 2 } } )
        ui.list_clear( "chat" )
        Out.afterClear = ui.list_count( "chat" )
        Out.missing = ui.list_count( "nobody" )
    )" );
    EXPECT_FALSE( Bool( out, "okRemove" ) );
    EXPECT_NE( Text( out, "whyRemove" ).find( "4 is outside [0, 1)" ), std::string::npos )
         << Text( out, "whyRemove" );
    EXPECT_FALSE( Bool( out, "okAdd" ) );
    EXPECT_NE( Text( out, "whyAdd" ).find( "'bad'" ), std::string::npos ) << Text( out, "whyAdd" );
    EXPECT_EQ( Number( out, "afterClear" ), 0.0 );
    EXPECT_EQ( Number( out, "missing" ), 0.0 );
}
