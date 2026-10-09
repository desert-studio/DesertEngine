// SCR-LUAU-1: a game script cannot reach the machine around it. Pinned against the REAL ScriptEngine (the
// constructor every game VM goes through), not a hand-opened sol2 state:
//   * os.execute is gone -- the call fails, and the command it carried did not run (the file it would have
//     written is absent);
//   * io, dofile, loadfile and load are absent, and so is the process/file half of os;
//   * os keeps exactly the time functions, and they work (the positive control: a sandbox that removed
//     `os` wholesale would pass every refusal above).

#include <Engine/Scripting/ScriptEngine.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <format>
#include <string>

namespace
{
    namespace fs = std::filesystem;
    using Desert::Scripting::ScriptEngine;
} // namespace

TEST( ScriptSandbox, OsExecuteFailsAndRunsNothing )
{
    const fs::path marker = fs::temp_directory_path() / "desert-script-sandbox-os-execute";
    fs::remove( marker );

    ScriptEngine engine( nullptr );
    const auto   result = engine.RunString( std::format( "os.execute('touch \"{}\"')", marker.generic_string() ) );

    ASSERT_FALSE( result.IsSuccess() ) << "os.execute is callable from a game script";
    EXPECT_NE( result.GetError().find( "execute" ), std::string::npos ) << result.GetError();
    EXPECT_FALSE( fs::exists( marker ) ) << "the shell command ran: " << marker.generic_string();
}

TEST( ScriptSandbox, LoadersIoAndProcessOsAreAbsent )
{
    ScriptEngine engine( nullptr );
    for ( const char* name : { "io", "dofile", "loadfile", "load", "os.exit", "os.getenv", "os.remove",
                               "os.rename", "os.tmpname", "os.setlocale", "os.execute" } )
    {
        const auto result = engine.RunString( std::format( "assert({} == nil, '{} is reachable')", name, name ) );
        EXPECT_TRUE( result.IsSuccess() ) << result.GetError();
    }
}

TEST( ScriptSandbox, OsKeepsExactlyTheTimeFunctions )
{
    ScriptEngine engine( nullptr );
    const auto   result = engine.RunString( R"(
        assert(type(os.time()) == 'number', 'os.time')
        assert(type(os.clock()) == 'number', 'os.clock')
        assert(type(os.date('%Y')) == 'string', 'os.date')
        assert(os.difftime(10, 4) == 6, 'os.difftime')
        local names = {}
        for k in pairs(os) do names[#names + 1] = k end
        table.sort(names)
        assert(table.concat(names, ',') == 'clock,date,difftime,time', table.concat(names, ','))
    )" );
    EXPECT_TRUE( result.IsSuccess() ) << result.GetError();
}
