local currentDir = _MAIN_SCRIPT_DIR

os.mkdir(currentDir .. "/build/TestReports")

local test_premake_files = os.matchfiles("./**/premake5.lua")

for _, premake_file in ipairs(test_premake_files) do
    include(path.getdirectory(premake_file))
end

-- ── THE TEST SUITES ARE NOT PART OF THE SHIPPING CONFIGURATION ──────────────────────────────────────
--
-- Shipping's defining property is that the development instruments are not in the binary. A test suite
-- IS a development instrument, and several of these suites exist specifically to hold one of the
-- instruments to its contract -- DrawCounterFunnel, MemoryDetector, GpuTimestampLayout,
-- SyncLoadChokepoint. Building them in the one configuration that removes what they test is a category
-- error with a cost attached in both directions:
--
--   * THE COST NOW, MEASURED ON WINDOWS RATHER THAN ESTIMATED (probe run 35633136353, the throwaway
--     workflow this decision was taken with): `msbuild Desert.sln -p:Configuration=Shipping` over the
--     WHOLE solution ran 1 h 36 min 41 s and then FAILED TO LINK. Without the suites the same
--     configuration builds green in 45 minutes (CI run 35635711944, Windows Shipping). So the suites
--     cost roughly an hour per platform per run and do not produce a binary at the end of it.
--
--     WHY IT FAILED IS THE SECOND HALF OF THE ARGUMENT, and it is not a defect to go and fix. Exactly
--     ONE of the 258 test scripts -- Desert/Tests/Runtime/ShippingBoundary's own -- names Shipping on
--     the filter that selects its libraries. The other 257 still read
--     `filter "configurations:Release"`, and a premake filter that matches no configuration
--     contributes nothing, so those 257 link no reflect-cpp and no gtest: the log is thousands of
--     LNK2001 lines for `rfl::json::Writer` and `yyjson`. Teaching 257 scripts to link a configuration
--     they should never be built in is work spent to make a category error compile.
--   * the cost later, which is worse: it puts a standing obligation on every future suite to compile
--     without the facility it is about, and the cheapest way to satisfy that obligation is to weaken
--     the test. A gate that pushes on tests in that direction is a gate that will eventually be paid.
--
-- Nothing is left unguarded by this. The half of the boundary that a test CAN prove is
-- Desert/Tests/Runtime/ShippingBoundary, which compiles no engine code at all (see its premake5.lua) --
-- it reads the sources as text, so its verdict is the same in every configuration and it runs on every
-- sweep. The other half needs a linked Shipping binary, which no test binary can produce, and that is
-- scripts/CI/ShippingSymbols.sh.
--
-- `removeconfigurations` and not `kind "None"`: the project must be ABSENT from the Shipping build
-- graph, not present-and-empty. A present-and-empty project is still a node the solution builds, still
-- a name in run_tests.bat's manifest, and still something a future `filter "configurations:Shipping"`
-- can accidentally bring back.
for _, premake_file in ipairs(test_premake_files) do
    project( path.getname( path.getdirectory( premake_file ) ) )
        removeconfigurations { "Shipping" }
end

-- THE LIST OF EXPECTED TEST BINARIES IS A FILE, WRITTEN HERE, AT GENERATION TIME.
--
-- It used to be ~1900 `echo` lines in a postbuild event, one per line of a batch file this script
-- typed out character by character (see the postbuild block below for what is left of it). That
-- shape is why the runner was BLIND for months: `echo set ERROR=0>> file` was read by cmd as a
-- redirection of descriptor 0, the digit was eaten, and Windows Debug could not report a failure at
-- all before 2026-08-15. Every construct that survives quoting through Lua, through MSBuild's
-- Command element and through cmd is a construct nobody can read, and unreadable is how that defect
-- lived. The list is data, so it is written as data — no escaping, and `build/TestManifest.txt` can
-- be opened and diffed by hand.
--
-- THE LIST IS ALSO AN OBLIGATION, NOT A CONVENIENCE. The runner fails when a name here has no
-- binary. Globbing the output directory instead — which is what the unix runner still does — cannot
-- tell "this suite was deleted" from "this suite failed to link", so a suite that stopped building
-- would simply stop being run, silently. That is the same class of defect as the one above, and the
-- manifest is what closes it on Windows.
--
-- Configuration-independent on purpose: Debug and Release build the same set of suites, so this is
-- written once at generation time and only the configuration travels through the postbuild below.
local test_names = {}
for _, premake_file in ipairs(test_premake_files) do
    table.insert(test_names, path.getname(path.getdirectory(premake_file)))
end
io.writefile(currentDir .. "/build/TestManifest.txt", table.concat(test_names, "\n") .. "\n")

group "Tests"
    project "BuildAllTests"
        kind "Utility"
        -- Same reason as the loop above: an aggregate over projects that do not exist in Shipping.
        removeconfigurations { "Shipping" }
        targetdir "%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}"
        objdir "%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}"

        for _, premake_file in ipairs(test_premake_files) do
            local test_dir = path.getdirectory(premake_file)
            local test_name = path.getname(test_dir)
           -- dependson(test_name)
        end

    project "RunAllTests"
        kind "Utility"
        -- Same reason as the loop above -- and one consequence worth naming: its postbuild is what
        -- writes run_tests.bat, so a Shipping build no longer produces a runner at all. That is the
        -- honest state. A runner that existed and ran nothing would be Ф4's shape: a silent pass.
        removeconfigurations { "Shipping" }

        -- These edges were added because this project's postbuild used to RUN the suite as well as
        -- write the runner, and without them MSBuild was free to schedule it alongside the test
        -- projects — and did: the Windows log shows "[ERROR] JobSystem.exe not found" interleaved
        -- with the linker still emitting ShadowCascades.exe. Every test was reported missing because
        -- it was still being compiled. (The same loop exists in BuildAllTests above with the
        -- dependson commented out, which is presumably where this went.)
        --
        -- The postbuild no longer runs anything (see the block below), so that symptom is now
        -- unreachable and the edges are not load-bearing for it. They are kept because they are what
        -- makes "RunAllTests" mean "every test suite is built": a Utility project with no edges is a
        -- node that claims a dependency it does not have, and the next person to put work back into
        -- this postbuild would inherit the 2026-08 defect all over again.
        for _, premake_file in ipairs(test_premake_files) do
            dependson(path.getname(path.getdirectory(premake_file)))
        end

    if os.target() == "windows" then
        -- TWO LINES OF PAYLOAD, AND THE SHAPE OF BOTH IS DICTATED BY WHAT cmd EATS.
        --
        -- What this writes is a shim: it names the configuration that was just built and hands the
        -- work to scripts/Windows/RunTests.ps1, which is committed, readable, and testable. The
        -- ~1900 echo lines it replaces built the whole runner out of cmd fragments that had to
        -- survive three levels of quoting; the manifest note above records what that cost.
        --
        -- NEITHER PAYLOAD LINE MAY END IN A DIGIT. `echo ... exit /b 1>> file` does not write the
        -- digit: cmd reads `1>>` as a redirection of descriptor 1 and the `1` never reaches the
        -- file. That is character-for-character the 2026-08-15 defect — `set ERROR=0>>` losing its
        -- zero and leaving Windows Debug unable to report a failure — and the first draft of THIS
        -- block reintroduced it while writing the exit check that was supposed to prevent it.
        --
        -- SO THERE IS NO EXIT CHECK, AND THAT IS THE SAFER OF THE TWO OPTIONS. A batch file that
        -- ends without `exit` returns the exit code of its last command, and the last command here
        -- is pwsh. The alternatives all need a literal digit or a `%`: `exit /b 1` is the digit
        -- above, and `exit /b %ERRORLEVEL%` puts a `%` inside an MSBuild <Command>, where `%XX` is
        -- an escape and `%(` is item metadata. Removing cmd from the verdict entirely beats both —
        -- RunTests.ps1 decides, and cmd only carries the number out.
        --
        -- `-Config %{cfg.buildcfg}` is last on that line for the same reason: it ends in a letter.
        --
        -- `pwsh` BY NAME, NOT BY PATH. The runner image installs PowerShell 7 at
        -- `C:\Program Files\PowerShell\7\pwsh.EXE` — visible in the Windows job's own log, because
        -- ci.yml's Vulkan steps use `shell: pwsh` and GitHub resolves that name off PATH in the same
        -- job. Hard-coding the directory would pin us to a `7` that will become an `8`. And the
        -- failure direction is right either way: if PATH ever loses it, cmd answers "'pwsh' is not
        -- recognized" with errorlevel 9009 and the step goes red. A missing runner cannot come back
        -- as a pass, which is the only property this shim absolutely must have.
        --
        -- NOTHING RUNS THE TESTS HERE. Until 2026-09-08 the last postbuild command was
        -- `call run_tests.bat`, so the suite ran once inside `msbuild` and then AGAIN in the CI job's
        -- own "Run tests" step. Measured on run 34223623196: the in-build run cost 28 min 39 s of a
        -- 62.7-minute Windows Debug build step, and the step that follows it repeated the same work
        -- in 24 min 02 s. Writing the runner and running it are different jobs, and the CI step is
        -- the one that owns the verdict — it is where the exit code is read and where the reports
        -- are uploaded from. (Release paid 2 min 20 s for the same duplicate.)
        postbuildcommands {
            "if exist \"%{wks.location}\\run_tests.bat\" del \"%{wks.location}\\run_tests.bat\"",

            "echo @echo off > \"%{wks.location}\\run_tests.bat\"",
            "echo pwsh -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"%{wks.location}\\scripts\\Windows\\RunTests.ps1\" -Config %{cfg.buildcfg}>> \"%{wks.location}\\run_tests.bat\"",
        }
    end

    -- NO `else` BRANCH, AND ITS REMOVAL IS A DELETION OF DEAD CODE, NOT A BEHAVIOUR CHANGE.
    --
    -- There used to be one here that read `postbuildcommands { bash scripts/MacOS/RunTests.sh ... }`
    -- above a comment claiming it "does the same job as the generated run_tests.bat". It never ran a
    -- single test. RunAllTests declares no `targetdir`, so gmake2 leaves both TARGET and TARGETDIR
    -- undefined in RunAllTests.make; `all: $(TARGETDIR) $(TARGET)` collapses to a rule with no
    -- prerequisites and the POSTBUILDCMDS block is never reached. Verified by running it rather than
    -- by reading it — `make -f RunAllTests.make config=debug` prints nothing and exits 0 — and
    -- corroborated on CI, where the macOS jobs show exactly ONE "===== Starting Tests =====" and its
    -- timestamp is the start of the workflow's own "Run tests" step to the second.
    --
    -- So the unix side has always been driven by ci.yml calling scripts/MacOS/RunTests.sh directly,
    -- which is the right place for it, and putting it back in a postbuild would only re-create the
    -- duplicate run that the Windows branch above just stopped paying for.

print("\n=== Test Configuration ===")
print("Found test modules: " .. #test_premake_files)
for i, file in ipairs(test_premake_files) do
    print("  " .. i .. ". " .. path.getdirectory(file))
end
print("Test reports will be saved to: ".. currentDir .. "/build/TestReports")
