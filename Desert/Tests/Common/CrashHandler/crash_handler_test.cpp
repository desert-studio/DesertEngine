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
#include <RuntimeCrashTest.hpp>
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
        // Leaves the scratch root in place (named by g_LastCrashRoot) for a test that asserts more.
        bool KeepReport = false;
    };

    std::filesystem::path g_LastCrashRoot;

    void RunCrashCase( const CrashCase& inCase )
    {
        const std::filesystem::path root = MakeScratchRoot( inCase.Kind );
        const int                   code = RunChild( root, inCase.Kind );

        ASSERT_NE( code, -1 ) << "could not start the crash child " << g_SelfPath.string();
        EXPECT_NE( code, 0 ) << "a deliberate crash must not exit successfully";

        const std::filesystem::path report = SoleReportDirectory( root );
        ASSERT_FALSE( report.empty() ) << "no single report directory under " << root.string()
                                       << " (child exit code " << code << ")";

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
        // CR1c: the GPU keys come from the device the host created, decoded; the game from SetGameName.
        EXPECT_EQ( FieldValue( contents, "gpu" ), "test harness, no device" );
        EXPECT_EQ( FieldValue( contents, "gpu_vendor" ), "0x10DE" );
        EXPECT_EQ( FieldValue( contents, "gpu_device" ), "0x2482" );
        EXPECT_EQ( FieldValue( contents, "gpu_driver" ), "591.86" );
        EXPECT_EQ( FieldValue( contents, "gpu_api" ), "1.4.303" );
        EXPECT_EQ( FieldValue( contents, "game" ), "CrashHandlerSuiteGame" );
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

        if ( inCase.KeepReport )
        {
            g_LastCrashRoot = root;
            return;
        }
        std::error_code cleanup;
        std::filesystem::remove_all( root, cleanup );
    }
} // namespace

// CR1c: each vendor's own packing of VkPhysicalDeviceProperties::driverVersion. A wrong split prints a
// plausible-looking but false driver version, which sends a support ticket after the wrong driver.
TEST( CrashHandler, DecodesDriverVersionsTheWayEachVendorPrintsThem )
{
    // NVIDIA 10/8/8/6: 591.86 is what nvidia-smi and the control panel show.
    EXPECT_EQ( Common::Crash::DescribeDriverVersion( 0x10DE, ( 591u << 22 ) | ( 86u << 14 ) ), "591.86" );
    EXPECT_EQ( Common::Crash::DescribeDriverVersion( 0x10DE, ( 560u << 22 ) | ( 9u << 14 ) ), "560.09" );
    EXPECT_EQ( Common::Crash::DescribeDriverVersion( 0x10DE, ( 470u << 22 ) | ( 57u << 14 ) | ( 2u << 6 ) | 1u ),
               "470.57.2.1" );
#if defined( _WIN32 )
    // Intel on Windows 18/14: the last two groups of "31.0.101.5186".
    EXPECT_EQ( Common::Crash::DescribeDriverVersion( 0x8086, ( 101u << 14 ) | 5186u ), "101.5186" );
#endif
    // AMD (and anyone else) use VK_MAKE_API_VERSION's 3/7/10/12 packing.
    EXPECT_EQ( Common::Crash::DescribeDriverVersion( 0x1002, ( 2u << 22 ) | ( 0u << 12 ) | 302u ), "2.0.302" );
    EXPECT_EQ( Common::Crash::DescribeApiVersion( ( 1u << 22 ) | ( 3u << 12 ) | 280u ), "1.3.280" );
}

#if DESERT_DEV_INSTRUMENTS
// PKG1c: the Runtime's `--crash-test <kind>[@stage]`. No stage keeps the flag's old meaning (@mounted); an
// unknown stage is refused, because a crash at the other stage files its report in the other directory.
TEST( CrashHandler, TheRuntimeCrashTestFlagNamesAKindAndAStage )
{
    using Desert::Player::CrashTestStage;
    const auto plain = Desert::Player::ParseCrashTest( "segv" );
    ASSERT_TRUE( plain.has_value() );
    EXPECT_EQ( plain->Kind, Common::Crash::TestKind::Segv );
    EXPECT_EQ( plain->Stage, CrashTestStage::Mounted );

    const auto early = Desert::Player::ParseCrashTest( "abort@early" );
    ASSERT_TRUE( early.has_value() );
    EXPECT_EQ( early->Kind, Common::Crash::TestKind::Abort );
    EXPECT_EQ( early->Stage, CrashTestStage::Early );

    const auto mounted = Desert::Player::ParseCrashTest( "purecall@mounted" );
    ASSERT_TRUE( mounted.has_value() );
    EXPECT_EQ( mounted->Stage, CrashTestStage::Mounted );

    EXPECT_FALSE( Desert::Player::ParseCrashTest( "segv@" ).has_value() );
    EXPECT_FALSE( Desert::Player::ParseCrashTest( "segv@late" ).has_value() );
    EXPECT_FALSE( Desert::Player::ParseCrashTest( "@early" ).has_value() );
    EXPECT_FALSE( Desert::Player::ParseCrashTest( "sigsegv@early" ).has_value() );
}
#endif

TEST( CrashHandler, ParsesEveryTestKind )
{
    EXPECT_EQ( Common::Crash::ParseTestKind( "segv" ), Common::Crash::TestKind::Segv );
    EXPECT_EQ( Common::Crash::ParseTestKind( "abort" ), Common::Crash::TestKind::Abort );
    EXPECT_EQ( Common::Crash::ParseTestKind( "purecall" ), Common::Crash::TestKind::PureCall );
    EXPECT_EQ( Common::Crash::ParseTestKind( "stackoverflow" ), Common::Crash::TestKind::StackOverflow );
    EXPECT_EQ( Common::Crash::ParseTestKind( "stackoverflow-worker" ),
               Common::Crash::TestKind::StackOverflowWorker );
    EXPECT_EQ( Common::Crash::ParseTestKind( "stackoverflow-job" ), Common::Crash::TestKind::StackOverflowJob );
    EXPECT_EQ( Common::Crash::ParseTestKind( "verify" ), Common::Crash::TestKind::Verify );
    EXPECT_EQ( Common::Crash::ParseTestKind( "trap" ), Common::Crash::TestKind::Trap );
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

namespace
{
    // Runs one child and returns its crash.txt after RunCrashCase's common assertions, or "" after a fatal one.
    std::string RunCaseAndReadReport( const char* inKind, const char* inFunction )
    {
        RunCrashCase( { inKind, inFunction, true } );
        if ( ::testing::Test::HasFatalFailure() )
        {
            return {};
        }
        std::string     contents = ReadWholeFile( SoleReportDirectory( g_LastCrashRoot ) / "crash.txt" );
        std::error_code cleanup;
        std::filesystem::remove_all( g_LastCrashRoot, cleanup );
        return contents;
    }
} // namespace

// DEV-CRASH1: a failed DESERT_VERIFY with no debugger attached reaches the handler THROUGH std::abort. The
// relation asserted is the codename: SIGABRT means the check skipped its breakpoint (no debugger) and took
// the abort path; SIGTRAP / EXCEPTION_BREAKPOINT would mean it trapped unconditionally, which before this
// change killed the Editor as "trace trap" with no report at all.
TEST( CrashHandler, AFailedVerifyWithoutADebuggerAbortsIntoTheHandler )
{
    const std::string contents = RunCaseAndReadReport( "verify", "CrashTestVerify" );
    if ( !contents.empty() )
    {
        EXPECT_EQ( FieldValue( contents, "codename" ), "SIGABRT" );
        EXPECT_NE( contents.find( "Verify failed: g_VerifyHolds" ), std::string::npos )
             << "the report's log does not carry the failed check";
    }
}

// A raw breakpoint instruction with nobody attached (a third-party __builtin_debugtrap / brk) is a crash
// the handler reports, not a silent "trace trap".
TEST( CrashHandler, ABreakpointWithoutADebuggerWritesAReport )
{
    const std::string contents = RunCaseAndReadReport( "trap", "CrashTestTrap" );
    if ( !contents.empty() )
    {
#if defined( _WIN32 )
        EXPECT_EQ( FieldValue( contents, "codename" ), "EXCEPTION_BREAKPOINT" );
#else
        EXPECT_EQ( FieldValue( contents, "codename" ), "SIGTRAP" );
#endif
    }
}

// CR1b: a stack overflow leaves the faulting thread no stack to write a report on, so the report must come
// from the report thread (Windows) / the alternate signal stack (POSIX). Proven by the relation between
// the crash and the report: the report's stack is the recursion, frame after frame, not just a file.
namespace
{
    const char* const kRecursion = "CrashTestStackOverflowRecurse";

    // Runs one stack-overflow child and returns its crash.txt, or "" after a fatal failure.
    std::string RunStackOverflowCase( const char* inKind )
    {
        return RunCaseAndReadReport( inKind, kRecursion );
    }

    void ExpectReportStackIsTheRecursion( const std::string& contents )
    {

#if defined( _WIN32 )
        EXPECT_EQ( FieldValue( contents, "codename" ), "EXCEPTION_STACK_OVERFLOW" );
        // A real SEH fault: `function` is frame 0, the faulting frame itself, which is the recursion.
        EXPECT_NE( FieldValue( contents, "function" ).find( kRecursion ), std::string::npos )
             << "function=" << FieldValue( contents, "function" );
#else
        const std::string signal = FieldValue( contents, "codename" );
        EXPECT_TRUE( signal == "SIGSEGV" || signal == "SIGBUS" ) << "codename=" << signal;
#endif

        // The recursion fills the walked frames: at least 16 of the report's [stack] lines name it.
        const std::size_t stackAt = contents.find( "\n[stack]\n" );
        const std::size_t logAt   = contents.find( "\n[log]\n" );
        ASSERT_NE( stackAt, std::string::npos );
        ASSERT_NE( logAt, std::string::npos );
        const std::string stack     = contents.substr( stackAt, logAt - stackAt );
        std::size_t       recursive = 0;
        for ( std::size_t at = stack.find( kRecursion ); at != std::string::npos;
              at             = stack.find( kRecursion, at + 1 ) )
        {
            ++recursive;
        }
        EXPECT_GE( recursive, 16u ) << "the report's stack is not the recursion:\n" << stack;
    }
} // namespace

TEST( CrashHandler, StackOverflowWritesAReportWhoseStackIsTheRecursion )
{
    const std::string contents = RunStackOverflowCase( "stackoverflow" );
    if ( !contents.empty() )
    {
        ExpectReportStackIsTheRecursion( contents );
    }
}

namespace
{
    // The child's worker logged its own OS thread id before recursing; the report's tid must be that
    // number, not the main thread's, the report thread's or the pid.
    void ExpectReportTidIsTheLoggedWorker( const std::string& contents )
    {
        const std::string tid = FieldValue( contents, "tid" );
        ASSERT_FALSE( tid.empty() );
        // The [log] line is a spdlog-formatted record whose end-of-line the ring sink turns into a
        // trailing space (newlines would break the one-field-per-line report contract), so the number
        // is not immediately followed by '\n'. Match the number itself and require that what follows
        // is not another digit, so a truncated or extended tid cannot pass for the child's.
        const std::string needle = "stackoverflow worker thread tid=" + tid;
        const std::size_t at     = contents.find( needle );
        ASSERT_NE( at, std::string::npos )
             << "the report's tid=" << tid << " is not the worker thread the child logged";
        const std::size_t afterTid = at + needle.size();
        EXPECT_FALSE( afterTid < contents.size() && contents[afterTid] >= '0' && contents[afterTid] <= '9' )
             << "the report's tid=" << tid << " is not the worker thread the child logged";
    }
} // namespace

// A thread other than the one Install() ran on has neither the main thread's stack guarantee nor its
// alternate signal stack: the report survives only because StartEngineThread prepared the thread
// (POSIX, CR1d) / the report thread writes it (Windows, CR1b). The report's tid must be the worker's.
TEST( CrashHandler, StackOverflowOnAWorkerThreadReportsThatThread )
{
    const std::string contents = RunStackOverflowCase( "stackoverflow-worker" );
    if ( contents.empty() )
    {
        return;
    }
    ExpectReportStackIsTheRecursion( contents );
    ExpectReportTidIsTheLoggedWorker( contents );
}

// The same overflow inside a JobSystem job: the pool's workers are engine threads too, and a job is where
// most off-main-thread recursion (cooks, imports, parallel systems) actually runs.
TEST( CrashHandler, StackOverflowInAJobSystemJobReportsThatWorker )
{
    const std::string contents = RunStackOverflowCase( "stackoverflow-job" );
    if ( contents.empty() )
    {
        return;
    }
    ExpectReportStackIsTheRecursion( contents );
    ExpectReportTidIsTheLoggedWorker( contents );
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
        // LogInit opens the log beside the executable; a test writes nothing into the tree (TST1), so the
        // child's log is moved into its scratch root, the way the editor moves it into <ProjectDir>/Saved/Logs.
        Common::Logger::LogInit();
        Common::Logger::RelocateLogFile( argv[3] );

        Common::Crash::InstallOptions options;
        options.hostName = "CrashHandlerTestChild";
        if ( engineRun )
        {
            // No project and no override: the engine's per-user root, as the Runtime installs.
#if defined( _WIN32 )
            ::_putenv_s( "LOCALAPPDATA", argv[3] );
#else
            // The child process, before any thread starts: nothing else can be reading the environment.
            // NOLINTNEXTLINE(concurrency-mt-unsafe)
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
        Common::Crash::SetGpu( { .name          = "test harness, no device",
                                 .vendorId      = 0x10DE,
                                 .deviceId      = 0x2482,
                                 .driverVersion = ( 591u << 22 ) | ( 86u << 14 ),
                                 .apiVersion    = ( 1u << 22 ) | ( 4u << 12 ) | 303u } );
        Common::Crash::SetGameName( "CrashHandlerSuiteGame" );
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
