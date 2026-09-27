// CR1 — the crash handler is proven by CRASHING, not by calling its helpers.
//
// A unit test cannot install a fault handler and then fault: the fault kills the test process and
// the suite reports nothing. So this binary is BOTH the test and the subject. Given
// `--crash-child <kind> <report-root>` it installs the handler with its report root pointed at a
// throwaway directory, crashes in the named way, and dies; given no arguments it is an ordinary gtest
// runner that spawns itself in child mode once per crash kind and reads what the child left behind.
//
// WHY THE REPORT ROOT IS OVERRIDDEN AND NOT INHERITED: without it the child would write into
// %LOCALAPPDATA%/DesertEngine/Crashes — the developer's real crash folder — and the assertions could
// not tell this run's report from yesterday's. InstallOptions::reportRootOverride exists for this and
// nothing else.

#include <Common/Core/CrashHandler.hpp>
#include <Common/Core/Logger.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined( _WIN32 )
#include <Windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace
{
    std::filesystem::path g_SelfPath;

    std::filesystem::path MakeScratchRoot( const char* inLabel )
    {
        const auto            stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        std::filesystem::path root  = std::filesystem::temp_directory_path() /
                                     ( std::string( "desert-cr1-" ) + inLabel + "-" + std::to_string( stamp ) );
        std::filesystem::create_directories( root );
        return root;
    }

    // Returns the child's exit code, or -1 when it could not be started.
    int RunChild( const std::filesystem::path& inReportRoot, const char* inKind,
                  const char* inMode = "--crash-child" )
    {
#if defined( _WIN32 )
        std::wstring command = L"\"" + g_SelfPath.wstring() + L"\" ";
        command += std::wstring( inMode, inMode + std::strlen( inMode ) ) + L" ";
        command += std::wstring( inKind, inKind + std::strlen( inKind ) );
        command += L" \"" + inReportRoot.wstring() + L"\"";

        std::vector<wchar_t> mutableCommand( command.begin(), command.end() );
        mutableCommand.push_back( L'\0' );

        STARTUPINFOW startup        = {};
        startup.cb                  = sizeof( startup );
        PROCESS_INFORMATION process = {};
        if ( ::CreateProcessW( nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                               nullptr, &startup, &process ) == 0 )
        {
            return -1;
        }
        ::WaitForSingleObject( process.hProcess, 60000 );
        DWORD code = 0;
        ::GetExitCodeProcess( process.hProcess, &code );
        ::CloseHandle( process.hThread );
        ::CloseHandle( process.hProcess );
        return static_cast<int>( code );
#else
        // posix_spawn takes char* const[]; owned copies give it writable storage without casting const away.
        std::string self   = g_SelfPath.string();
        std::string root   = inReportRoot.string();
        std::string mode   = inMode;
        std::string kind   = inKind;
        char*       argv[] = { self.data(), mode.data(), kind.data(), root.data(), nullptr };
        pid_t       child  = 0;
        if ( ::posix_spawn( &child, self.c_str(), nullptr, nullptr, argv, environ ) != 0 )
        {
            return -1;
        }
        int status = 0;
        ::waitpid( child, &status, 0 );
        return WIFEXITED( status ) ? WEXITSTATUS( status ) : 128 + WTERMSIG( status );
#endif
    }

    // The single report directory the child created under the scratch root. Empty when there is none,
    // which the caller asserts on rather than silently treating as "nothing to check".
    std::filesystem::path SoleReportDirectory( const std::filesystem::path& inRoot )
    {
        std::filesystem::path found;
        std::size_t           count = 0;
        std::error_code       error;
        for ( const auto& entry : std::filesystem::directory_iterator( inRoot, error ) )
        {
            if ( entry.is_directory() )
            {
                found = entry.path();
                ++count;
            }
        }
        return count == 1 ? found : std::filesystem::path{};
    }

    std::string ReadWholeFile( const std::filesystem::path& inPath )
    {
        const std::ifstream stream( inPath, std::ios::binary );
        std::stringstream   buffer;
        buffer << stream.rdbuf();
        return buffer.str();
    }

    // The value of one `key=` line of crash.txt, or "" when the key is absent.
    std::string FieldValue( const std::string& inText, const std::string& inKey )
    {
        const std::string needle = "\n" + inKey + "=";
        const std::size_t at     = inText.find( needle );
        if ( at == std::string::npos )
        {
            return {};
        }
        const std::size_t start = at + needle.size();
        const std::size_t end   = inText.find( '\n', start );
        return inText.substr( start, end == std::string::npos ? std::string::npos : end - start );
    }

    struct CrashCase
    {
        const char* Kind;
        const char* ExpectedFunctionFragment;
    };

    void RunCrashCase( const CrashCase& inCase )
    {
        const std::filesystem::path root = MakeScratchRoot( inCase.Kind );
        const int                   code = RunChild( root, inCase.Kind );

        ASSERT_NE( code, -1 ) << "could not start the crash child " << g_SelfPath.string();
        EXPECT_NE( code, 0 ) << "a deliberate crash must not exit successfully";

        const std::filesystem::path report = SoleReportDirectory( root );
        ASSERT_FALSE( report.empty() ) << "no single report directory under " << root.string();

        const std::filesystem::path text = report / "crash.txt";
        ASSERT_TRUE( std::filesystem::exists( text ) ) << "crash.txt missing in " << report.string();

        const std::string contents = ReadWholeFile( text );
        EXPECT_EQ( contents.rfind( "DESERTCRASH 1", 0 ), 0u ) << "crash.txt must open with its format line";
        EXPECT_NE( contents.find( "\n[end]\nwritten=complete\n" ), std::string::npos )
             << "the report was truncated: " << text.string();

        EXPECT_EQ( FieldValue( contents, "host" ), "CrashHandlerTestChild" );
        EXPECT_FALSE( FieldValue( contents, "codename" ).empty() );
        EXPECT_FALSE( FieldValue( contents, "version" ).empty() );
        EXPECT_FALSE( FieldValue( contents, "os" ).empty() );
        EXPECT_EQ( FieldValue( contents, "scene" ), "Scenes/CrashHandlerSuite.desce" );
        EXPECT_NE( contents.find( "\n[stack]\n" ), std::string::npos );
        EXPECT_NE( contents.find( "\nlog=" ), std::string::npos ) << "the log ring produced no lines";

        const std::string function = FieldValue( contents, "function" );
        EXPECT_NE( function, "unknown" )
             << "no symbol resolved for the faulting frame; is the PDB beside " << g_SelfPath.string() << "?";
        EXPECT_NE( contents.find( inCase.ExpectedFunctionFragment ), std::string::npos )
             << "crash.txt does not name " << inCase.ExpectedFunctionFragment << "; function=" << function;

#if defined( _WIN32 )
        const std::filesystem::path dump = report / "crash.dmp";
        ASSERT_TRUE( std::filesystem::exists( dump ) ) << "crash.dmp missing in " << report.string();
        EXPECT_GT( std::filesystem::file_size( dump ), 4096u ) << "crash.dmp is too small to be a minidump";
#endif

        std::error_code cleanup;
        std::filesystem::remove_all( root, cleanup );
    }
} // namespace

TEST( CrashHandler, ParsesTheThreeTestKinds )
{
    EXPECT_EQ( Common::Crash::ParseTestKind( "segv" ), Common::Crash::TestKind::Segv );
    EXPECT_EQ( Common::Crash::ParseTestKind( "abort" ), Common::Crash::TestKind::Abort );
    EXPECT_EQ( Common::Crash::ParseTestKind( "purecall" ), Common::Crash::TestKind::PureCall );
    // An unknown word is refused rather than defaulted: a --crash-test typo that crashed in some
    // other way would file a report describing a fault nobody asked for.
    EXPECT_FALSE( Common::Crash::ParseTestKind( "sigsegv" ).has_value() );
    EXPECT_FALSE( Common::Crash::ParseTestKind( "" ).has_value() );
}

TEST( CrashHandler, SegvWritesAReportNamingTheFaultingFunction )
{
    RunCrashCase( { "segv", "CrashTestSegv" } );
}

TEST( CrashHandler, AbortWritesAReportNamingTheFaultingFunction )
{
    RunCrashCase( { "abort", "CrashTestAbort" } );
}

TEST( CrashHandler, PureCallWritesAReportNamingTheFaultingFunction )
{
    RunCrashCase( { "purecall", "PureCall" } );
}

// PKG1c: the Runtime installs BEFORE the archive mount, under the engine's per-user root, and moves the
// root to GameUserDirectory(Name)/Crashes once the descriptor is read. The children point the engine
// root at a scratch directory through the environment the handler reads (HOME / LOCALAPPDATA).
namespace
{
    std::filesystem::path EngineRootUnder( const std::filesystem::path& inUser )
    {
#if defined( _WIN32 )
        return inUser / "DesertEngine" / "Crashes";
#else
        return inUser / ".desertengine" / "Crashes";
#endif
    }
} // namespace

TEST( CrashHandler, ACrashBeforeTheMoveLandsUnderTheEngineRoot )
{
    const std::filesystem::path user = MakeScratchRoot( "engine" );
    const int                   code = RunChild( user, "segv", "--crash-child-engine" );
    ASSERT_NE( code, -1 ) << "could not start the crash child " << g_SelfPath.string();

    EXPECT_FALSE( SoleReportDirectory( EngineRootUnder( user ) ).empty() )
         << "no report under " << EngineRootUnder( user ).string();
    EXPECT_FALSE( std::filesystem::exists( user / "game" ) ) << "a report went to the game before the move";

    std::error_code cleanup;
    std::filesystem::remove_all( user, cleanup );
}

TEST( CrashHandler, ACrashAfterTheMoveLandsInTheGameDirectory )
{
    const std::filesystem::path user = MakeScratchRoot( "moved" );
    const int                   code = RunChild( user, "segv", "--crash-child-moved" );
    ASSERT_NE( code, -1 ) << "could not start the crash child " << g_SelfPath.string();

    const std::filesystem::path report = SoleReportDirectory( user / "game" / "Crashes" );
    EXPECT_FALSE( report.empty() ) << "no report under " << ( user / "game" / "Crashes" ).string();
    EXPECT_TRUE( std::filesystem::exists( report / "crash.txt" ) ) << "the moved set lost its crash.txt path";
    // The engine root was created at Install and must stay EMPTY: the move is the whole set, not the
    // directory alone.
    std::error_code ec;
    EXPECT_TRUE( std::filesystem::is_empty( EngineRootUnder( user ), ec ) )
         << "a report (or part of one) stayed under the engine root";

    std::error_code cleanup;
    std::filesystem::remove_all( user, cleanup );
}

// A test driver: an exception escaping main terminates the process, and the harness reports that as a failure.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main( int argc, char** argv )
{
    g_SelfPath = std::filesystem::absolute( argv[0] );

    const std::string mode      = argc == 4 ? argv[1] : "";
    const bool        engineRun = mode == "--crash-child-engine" || mode == "--crash-child-moved";
    if ( mode == "--crash-child" || engineRun )
    {
        Common::Logger::LogInit();

        Common::Crash::InstallOptions options;
        options.hostName = "CrashHandlerTestChild";
        if ( engineRun )
        {
            // No project and no override: the engine's per-user root, as the Runtime installs.
#if defined( _WIN32 )
            ::_putenv_s( "LOCALAPPDATA", argv[3] );
#else
            ::setenv( "HOME", argv[3], 1 );
#endif
        }
        else
        {
            options.reportRootOverride = argv[3];
        }
        const Common::BoolResultStr installed = Common::Crash::Install( options );
        if ( !installed.IsSuccess() )
        {
            std::fputs( installed.GetError().c_str(), stderr );
            return 2;
        }
        if ( mode == "--crash-child-moved" )
        {
            const Common::BoolResultStr moved =
                 Common::Crash::MoveReportRoot( std::filesystem::path( argv[3] ) / "game" / "Crashes" );
            if ( !moved.IsSuccess() )
            {
                std::fputs( moved.GetError().c_str(), stderr );
                return 2;
            }
        }

        // Both context setters are exercised, because a value that is never written is a field the
        // report would always show as "none" and nobody would notice.
        Common::Crash::SetScenePath( "Scenes/CrashHandlerSuite.desce" );
        Common::Crash::SetGpuDescription( "test harness, no device" );
        LOG_INFO( "[CrashHandlerTestChild] about to crash on purpose: {}", argv[2] );

        const std::optional<Common::Crash::TestKind> kind = Common::Crash::ParseTestKind( argv[2] );
        if ( !kind.has_value() )
        {
            std::fputs( "unknown crash kind\n", stderr );
            return 2;
        }
        Common::Crash::TriggerTestCrash( *kind );
    }

    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
