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

#include <string_view>

namespace Desert::TestSupport
{
    // argc/argv of the child: argv[0] is the runner's path, argv[1..argc-1] what followed
    // `--desert-child=<name>` on the command line. The return value is the process exit code.
    using ChildMain = int ( * )( int argc, char** argv );

    // Registers `main` under `name` for `--desert-child=<name>`. Construct at namespace scope only (static
    // initialisation, before RunnerMain reads the table); `name` must outlive the process (a literal).
    class ChildEntry
    {
    public:
        ChildEntry( std::string_view name, ChildMain main );

        ChildEntry( const ChildEntry& )            = delete;
        ChildEntry& operator=( const ChildEntry& ) = delete;
    };
} // namespace Desert::TestSupport
