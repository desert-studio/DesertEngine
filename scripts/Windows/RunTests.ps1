# Run every test binary the build is supposed to have produced, several at a time, and fail if any
# one of them fails or is missing. Windows counterpart of scripts/MacOS/RunTests.sh.
#
# Invoked by the generated run_tests.bat (written by the postbuild block in Desert/Tests/premake5.lua)
# and runnable by hand from the repository root:
#
#   pwsh -File scripts/Windows/RunTests.ps1 -Config Debug
#
# ---------------------------------------------------------------------------------------------------
# THIS FILE'S FIRST OBLIGATION IS TO BE ABLE TO SAY "FAILED".
#
# The runner it replaces could not. Until 2026-08-15 the generated batch contained `set ERROR=0>>`,
# which cmd reads as a redirection of descriptor 0 rather than as the digit zero, so the variable the
# whole verdict hung on was never set and Windows Debug was green by construction. Running tests
# concurrently is the most direct way to reintroduce that class of defect — a background process's
# exit code is trivial to lose — so every exit path below is explicit, and the two branches that must
# be able to go red (a test that fails, a binary that is not there) are proven by reproduction, not by
# argument. If you change this file, reproduce them again.
#
# WHY CONCURRENTLY AT ALL. Measured on CI run 34223623196 (Windows Debug, 2026-09-08): 176 suites,
# 24 min 02 s serial, median suite 0.059 s, and the six slowest suites are 78 % of the total. That
# shape is what parallelism is for.
#
# WHY THERE IS NO COST MODEL. Replaying those measured durations through this scheduler gives 6.9 min
# on four workers, against 6.0 min for a perfect longest-job-first schedule. Nine-tenths of a minute
# is the entire prize for adding a table of suite durations, and such a table goes stale silently,
# which costs more than it saves. Launch order is manifest order.
#
# AND WHY 6.9 MINUTES IS AN OPTIMISTIC NUMBER RATHER THAN A PREDICTION. That replay treats a suite as
# one unit of one resource, and half the expensive ones are not. Measured as (user+sys)/real per
# suite, run alone, 2026-09-08:
#
#     CloudNoiseVolume 6.88   NewCloudAsset 6.63   CloudPlacementSpectrum 5.41
#     CloudProceduralField 4.25   CloudField 2.57
#     MeshVertexPath 1.00   PureVirtualCensus 0.99   CloudType 0.90
#     ShaderCacheKey 0.88   PBRSceneFrame 0.85
#
# The parallel ones size their pools from `std::thread::hardware_concurrency()`
# (Common/Core/JobSystem.cpp, Engine/Assets/CloudNoiseVolumeGenerator.cpp), so on a four-core runner
# they were ALREADY using the whole machine during the SERIAL run. Their wall time cannot shrink, and
# three suites beside them makes it grow. Costing the CI wall times at min(ratio, 4) cores gives about
# 2770 CPU-seconds, so a four-core runner cannot finish in under ~11.5 min however it is scheduled.
# Expect 11 to 16 minutes against the 24 measured, not 6.9 — and note that the same change also
# stopped the suite running TWICE per Windows job, which was 28 min 39 s on its own.
#
# WHAT WAS ACTUALLY RUN, not modelled: all 176 suites through this runner on macOS, serial and
# four-way. 461 s -> 273 s, both exiting 0, both printing zero `[  FAILED  ]` lines, and the list of
# suites in the log identical line for line. Concurrency changed the clock and nothing else.
#
# WHY THE OUTPUT IS BUFFERED AND REPLAYED IN LAUNCH ORDER. Four gtest binaries writing to one console
# interleave line-by-line into something no one can read, and an unreadable Windows log defeats the
# purpose of having a Windows job. Each suite's output goes to its own file and is replayed whole,
# in manifest order, so the log is byte-comparable with the serial runner's.
#
# WHY EACH SUITE GETS ITS OWN TEMP DIRECTORY. Roughly fifty suites build scratch files under
# `std::filesystem::temp_directory_path()`, and today they are namespaced only by a hand-written
# literal — `desert_pak_test`, `desert_asset_eviction`, `DesertStaticProbe_...`. A census of the tree
# on 2026-09-08 found 95 such literals and zero collisions, so serial execution was safe by luck and
# diligence. Concurrency turns a future copy-pasted literal from a harmless duplicate into two
# processes racing on one directory — a flake that would read as a defect in whatever change happened
# to be under test. Pointing TMPDIR/TMP/TEMP at a per-suite directory makes the collision
# unrepresentable instead of merely absent, which is worth more than a census that has to be kept
# green. It also means a failing suite's scratch files are still on disk afterwards, under
# build/TestScratch/<Config>/<Suite>/Temp, instead of mixed into the machine's temp directory.
#
# WHY EACH SUITE ALSO GETS ITS OWN WORKING DIRECTORY. This runner used to start every binary in the
# repository root, and suites that wrote relative to the working directory left Assets/, DerivedDataCache/
# and RegistryProbe/ in the checkout (owner, 2026-09-27; TST1). Each suite now starts in
# build/TestScratch/<Config>/<Suite>, emptied before the run — UE's Saved/Automation pattern. Suites that
# read tracked files walk up from there to the checkout.
# ---------------------------------------------------------------------------------------------------

[CmdletBinding()]
param(
    # Which build configuration's binaries to run. The generated run_tests.bat passes the
    # configuration that was just built.
    [string] $Config = "Debug",

    # Repository root. Defaults to this script's grandparent, which is what makes the generated batch
    # able to pass a path-free command line: a `-Root "...\"` argument would end in a backslash
    # immediately before the closing quote, and Windows command-line parsing reads that as an escaped
    # quote and swallows the rest of the line.
    [string] $Root,

    # Concurrent suites. 0 means "one per logical processor" (four on a GitHub windows-latest runner).
    [int] $Jobs = 0,

    # A suite that never returns would otherwise run the job into its `timeout-minutes` ceiling, and a
    # CANCELLED job reports as neither pass nor fail — it destroys the evidence instead of reporting
    # it. This converts a hang into a red job that names the suite. The ceiling is ~4x the slowest
    # suite measured on CI (CloudField, 5 min 6 s), so it cannot fire on a merely slow run.
    [int] $TimeoutSeconds = 1200,

    # One CI test shard: run only the suites named in this file (written by scripts/CI/TestShards.py
    # from the manifest), in the file's order. Every name must still be in the manifest, and a name
    # with no binary behind it is still a failure. Empty = the whole manifest.
    [string] $ListFile = ""
)

$ErrorActionPreference = "Stop"

# A runner that crashes must not be mistaken for a runner that found nothing wrong.
trap
{
    Write-Host "[ERROR] test runner faulted: $_"
    exit 1
}

if (-not $Root)
{
    $Root = (Resolve-Path ([IO.Path]::Combine($PSScriptRoot, "..", ".."))).Path
}
# Absolute either way: each suite starts in its own scratch directory, and a relative root would resolve
# against that rather than against the caller's directory.
$Root = (Resolve-Path -LiteralPath $Root).Path

# `$IsWindows` does not exist in Windows PowerShell 5.1; this variable does, on every Windows since NT.
# The suffix is derived rather than passed so that the same file is exercised when it is run on a
# developer's macOS checkout against the unix test binaries, which is the only way it can be tested
# outside CI at all.
$exeSuffix = ""
if ($env:OS -eq "Windows_NT") { $exeSuffix = ".exe" }

$testDir    = [IO.Path]::Combine($Root, "build", "Bin", "Tests", $Config)
$reportDir  = [IO.Path]::Combine($Root, "build", "TestReports")
$manifest   = [IO.Path]::Combine($Root, "build", "TestManifest.txt")
$scratchDir = [IO.Path]::Combine($Root, "build", "TestScratch")

if (-not (Test-Path -LiteralPath $manifest))
{
    # Refusing here is the point. Globbing $testDir as a fallback would turn "the generator never
    # ran" into a run of whatever happens to be lying in the output directory, reported as a pass.
    Write-Host "[ERROR] test manifest not found: $manifest"
    Write-Host "[ERROR] run premake (scripts\Windows\Setup.bat) before running the tests"
    exit 1
}

$names = @(Get-Content -LiteralPath $manifest | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" })
if ($names.Count -eq 0)
{
    Write-Host "[ERROR] test manifest is empty: $manifest"
    exit 1
}

if ($ListFile)
{
    if (-not (Test-Path -LiteralPath $ListFile))
    {
        Write-Host "[ERROR] shard list not found: $ListFile"
        exit 1
    }
    $shard = @(Get-Content -LiteralPath $ListFile | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" })
    $stray = @($shard | Where-Object { $names -notcontains $_ })
    if ($shard.Count -eq 0 -or $stray.Count -gt 0)
    {
        Write-Host "[ERROR] shard list $ListFile is empty or names suites outside the manifest: $($stray -join ' ')"
        exit 1
    }
    $names = $shard
}

if (-not (Test-Path -LiteralPath $reportDir)) { New-Item -ItemType Directory -Path $reportDir -Force | Out-Null }

if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }
if ($Jobs -le 0) { $Jobs = 1 }

$records = @()
foreach ($name in $names)
{
    $records += [pscustomobject]@{
        Name    = $name
        Exe     = [IO.Path]::Combine($testDir, $name + $exeSuffix)
        Scratch = [IO.Path]::Combine($scratchDir, $Config, $name)
        Xml     = [IO.Path]::Combine($reportDir, $name + ".xml")
        Out     = [IO.Path]::Combine($reportDir, $name + ".out.log")
        Err     = [IO.Path]::Combine($reportDir, $name + ".err.log")
        Proc    = $null
        Started = $null
        Exit    = $null
        Missing = $false
        TimedOut= $false
        Done    = $false
    }
}

Write-Host "===== Starting Tests ====="
Write-Host "[INFO] $($records.Count) suites, $Jobs at a time, from $testDir"

$running = New-Object System.Collections.ArrayList
$failed  = New-Object System.Collections.Generic.List[string]
$next = 0   # index of the next suite to launch
$emit = 0   # index of the next suite whose output is due to be printed

while ($emit -lt $records.Count)
{
    while ($next -lt $records.Count -and $running.Count -lt $Jobs)
    {
        $r = $records[$next]
        if (-not (Test-Path -LiteralPath $r.Exe))
        {
            # A name in the manifest with no binary behind it: the suite failed to link, or was
            # renamed without regenerating. Either way it is a failure and not something to skip.
            $r.Missing = $true
            $r.Done    = $true
        }
        else
        {
            # Set on this process and inherited by the child, which is why the assignment sits
            # immediately before the launch: Start-Process without -UseNewEnvironment hands the
            # child a snapshot of the environment as it stands right now. The directory has to
            # exist first — temp_directory_path() reports an error for a TMP that is not there,
            # and every scratch-using suite would fail identically and uninformatively.
            # Emptied before the run (not after), so a failing suite's leftovers stay to be read.
            if (Test-Path -LiteralPath $r.Scratch)
            {
                Remove-Item -LiteralPath $r.Scratch -Recurse -Force
            }
            $temp = [IO.Path]::Combine($r.Scratch, "Temp")
            New-Item -ItemType Directory -Path $temp -Force | Out-Null
            $env:TMPDIR = $temp   # POSIX (this file is exercised on macOS; see the header)
            $env:TMP    = $temp   # Windows: temp_directory_path() reads TMP, then TEMP
            $env:TEMP   = $temp

            $r.Started = Get-Date
            $r.Proc = Start-Process -FilePath $r.Exe `
                                    -ArgumentList "--gtest_output=xml:`"$($r.Xml)`"" `
                                    -RedirectStandardOutput $r.Out `
                                    -RedirectStandardError $r.Err `
                                    -WorkingDirectory $r.Scratch `
                                    -NoNewWindow -PassThru

            # Touching .Handle is what makes .ExitCode readable later, and it is required, not a
            # nicety. Windows PowerShell 5.1's Start-Process closes the process handle it opened as
            # soon as it hands the object back; System.Diagnostics.Process then has nothing to ask
            # the kernel for, and .ExitCode comes back $null even after HasExited is $true and
            # WaitForExit() has returned. Reading .Handle here caches a handle inside the object
            # while the process is still alive, so the exit status survives the process itself.
            # Without it EVERY suite printed "[FAIL] <Suite> (exit )" -- `$null -ne 0` is true --
            # and the script exited 1 on a fully passing run. PowerShell 7 keeps the handle on its
            # own, where this line is a harmless no-op.
            [void]$r.Proc.Handle

            [void]$running.Add($r)
        }
        $next++
    }

    for ($i = $running.Count - 1; $i -ge 0; $i--)
    {
        $r = $running[$i]
        if ($r.Proc.HasExited)
        {
            # WaitForExit() on an already-exited process is what flushes the redirected streams; the
            # replay below reads those files, so skipping this loses the tail of a failing suite's
            # output — which is the part that says what failed.
            $r.Proc.WaitForExit()
            $r.Exit = $r.Proc.ExitCode
            $r.Done = $true
            $running.RemoveAt($i)
        }
        elseif (((Get-Date) - $r.Started).TotalSeconds -gt $TimeoutSeconds)
        {
            try { $r.Proc.Kill() } catch { }
            try { $r.Proc.WaitForExit(10000) | Out-Null } catch { }
            $r.TimedOut = $true
            $r.Exit     = -1
            $r.Done     = $true
            $running.RemoveAt($i)
        }
    }

    $progressed = $false
    while ($emit -lt $records.Count -and $records[$emit].Done)
    {
        $r = $records[$emit]
        Write-Host "[TEST] $($r.Exe)"

        if ($r.Missing)
        {
            Write-Host "[ERROR] $($r.Name)$exeSuffix not found"
            $failed.Add($r.Name)
        }
        else
        {
            foreach ($stream in @($r.Out, $r.Err))
            {
                if (Test-Path -LiteralPath $stream)
                {
                    $text = Get-Content -LiteralPath $stream -Raw
                    if ($text) { Write-Host $text.TrimEnd() }
                    Remove-Item -LiteralPath $stream -Force -ErrorAction SilentlyContinue
                }
            }

            if ($r.TimedOut)
            {
                Write-Host "[FAIL] $($r.Name) (killed after ${TimeoutSeconds}s)"
                $failed.Add($r.Name)
            }
            elseif ($r.Exit -ne 0)
            {
                # `-ne 0` and not `-gt 0`: a suite killed by an access violation exits with a negative
                # code, and that is precisely a failure.
                Write-Host "[FAIL] $($r.Name) (exit $($r.Exit))"
                $failed.Add($r.Name)
            }
        }

        $emit++
        $progressed = $true
    }

    if (-not $progressed -and $emit -lt $records.Count) { Start-Sleep -Milliseconds 50 }
}

Write-Host "===== Test Results ====="
if ($failed.Count -eq 0)
{
    Write-Host "ALL TESTS PASSED"
    exit 0
}

# The per-suite [FAIL] lines are already above, but they are scattered through 170 blocks of gtest
# output. The verdict has to be readable from the bottom of the log without scrolling.
Write-Host "SOME TESTS FAILED ($($failed.Count) of $($records.Count)):"
foreach ($name in $failed) { Write-Host "  $name" }
exit 1
