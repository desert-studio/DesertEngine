#pragma once

// THE TEST RUNNER'S CONTRACT WITH THE SUITES (BUILD1 P1, BuildScripts/BUILD1-CONTRACT.md §4).
//
// One executable per layer (CommonTests, EngineTests, EditorTests, RuntimeTests, ToolsTests) holds every
// suite of that layer and links the layer's libraries instead of recompiling their sources into each
// suite. TestSupport/RunnerMain.cpp is the only `main` under Desert/Tests; a suite has none.
//
// A SUITE IS A DIRECTORY. `Desert/Tests/<Layer>/<Suite>/...` — the same name the manifest
// (build/TestManifest.txt), the scratch directory and the per-suite report already use. The runner reads
// it from each test's own source path (gtest's TestInfo::file(), i.e. __FILE__), so no registration and
// no second list can disagree with the tree. `--desert-suite=<Suite>[,<Suite>...]` runs those suites;
// an unknown name, or a selection that matches no test, exits 2 naming it — never "0 tests passed".
//
// THE RUNNER'S OWN PATH is Common::Utils::FileSystem::ExecutablePath(), not argv[0]: argv[0] is whatever
// the parent typed (relative, or without the extension), and a suite has no main to read it from anyway.
//
// CHILD PROCESSES. A suite that re-launches its own binary (CrashHandler crashes a child on purpose)
// registers an entry point under a name; `<runner> --desert-child=<name> <args...>` runs that entry
// point INSTEAD of gtest, with argv[0] the runner and argv[1..] the arguments after the flag. The
// registration is a namespace-scope object in the suite's .cpp:
//
//     namespace { const Desert::TestSupport::ChildEntry kCrash{ "crash", &RunCrashChild }; }
//
// Two registrations under one name abort the runner at startup with both names: a child dispatched to
// the wrong suite's entry point would be a test that passes for the wrong reason.
//
// A TEST SOURCE OUTSIDE Desert/Tests. A submodule can carry a test that every host compiles into its own
// suite (ThirdParty/desert-shared/Tests/project_format_test.cpp: the launcher and the engine must agree on
// the format). Its __FILE__ names no suite, so the suite that compiles it ADOPTS it, from one of its own
// .cpp files -- the adopting file's path is what names the suite, so there is still no second list:
//
//     namespace { const Desert::TestSupport::AdoptedTestSource kShared{
//     "desert-shared/Tests/project_format_test.cpp" }; }
//
// The runner premake compiles the adopted file next to the suite's own. A source adopted by two suites
// aborts the runner at startup, as two child entry points do.
//
// SET-UP THAT BELONGS TO ONE SUITE. Before BUILD1 a suite's own main did what its tests needed first (a
// throwaway DDC, HOME pointed away from the developer's, a singleton the engine creates at startup). A
// gtest global environment cannot replace that main in a runner: it would run for EVERY suite of the
// binary. A suite registers its environment instead, from one of its own .cpp files -- the file's path
// names the suite, as for AdoptedTestSource:
//
//     namespace { const Desert::TestSupport::SuiteEnvironment kHost{ &MakeHostEnvironment }; }
//
// The runner hands it to gtest (SetUp before the first test, TearDown after the last) only when that
// suite is selected with --desert-suite, or when the run selects no suite (every test of the runner).
// A suite may register several; they are set up in registration order within a translation unit.
//
// THE PROCESS'S HOST STEPS. Some suites point the process at the engine directory (SetSuiteEngineDir,
// engine_dir.hpp) and open the committed project (OpenSuiteProject, project_scope.hpp) before gtest even
// parses its flags, so a parameter generator already sees them. RunnerMain takes those steps, once, before
// InitGoogleTest, for each selected suite that declares them (every declaring suite when none is selected):
//
//     namespace { const Desert::TestSupport::SuiteHost kHost{ { .EngineDir = true, .Project = true } }; }
//
// A suite declares its steps once; a second declaration in the same suite aborts the runner at startup.

#include <source_location>
#include <string_view>

namespace testing
{
    class Environment;
}

namespace Desert::TestSupport
{
    // What RunnerMain does for a suite before InitGoogleTest: the engine directory first, then the project.
    struct SuiteHostSteps
    {
        bool EngineDir = false;
        bool Project   = false;
    };

    // argc/argv of the child: argv[0] is the runner's path, argv[1..argc-1] what followed
    // `--desert-child=<name>` on the command line. The return value is the process exit code.
    using ChildMain = int ( * )( int argc, char** argv );

    // Registers `main` under `name` for `--desert-child=<name>`. Construct at namespace scope only (static
    // initialisation, before RunnerMain reads the table); `name` must outlive the process (a literal).
    // RunnerMain takes `steps` before it calls `main`, as a suite's own main once did before its child branch.
    class ChildEntry
    {
    public:
        ChildEntry( std::string_view name, ChildMain main, SuiteHostSteps steps = {} );

        ChildEntry( const ChildEntry& )            = delete;
        ChildEntry& operator=( const ChildEntry& ) = delete;
    };

    // Maps every test whose source path ends with `pathSuffix` ('/'-separated) to the suite of the file
    // this object is constructed in. Namespace scope only; `pathSuffix` must be a literal.
    class AdoptedTestSource
    {
    public:
        explicit AdoptedTestSource( std::string_view     pathSuffix,
                                    std::source_location where = std::source_location::current() );

        AdoptedTestSource( const AdoptedTestSource& )            = delete;
        AdoptedTestSource& operator=( const AdoptedTestSource& ) = delete;
    };

    // Declares `steps` for the suite of the file this object is constructed in. Namespace scope only.
    class SuiteHost
    {
    public:
        explicit SuiteHost( SuiteHostSteps steps, std::source_location where = std::source_location::current() );

        SuiteHost( const SuiteHost& )            = delete;
        SuiteHost& operator=( const SuiteHost& ) = delete;
    };

    // Makes the environment when the suite runs; gtest owns what it returns.
    using EnvironmentFactory = ::testing::Environment* (*)();

    // Registers `make` for the suite of the file this object is constructed in. Namespace scope only.
    class SuiteEnvironment
    {
    public:
        explicit SuiteEnvironment( EnvironmentFactory   make,
                                   std::source_location where = std::source_location::current() );

        SuiteEnvironment( const SuiteEnvironment& )            = delete;
        SuiteEnvironment& operator=( const SuiteEnvironment& ) = delete;
    };
} // namespace Desert::TestSupport
