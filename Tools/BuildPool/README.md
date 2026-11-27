# BuildPool

One pool of compiler and linker slots per machine. Every build that a CMake project configured with `buildpool_enable` starts, from any number of trees at once, asks one daemon per *home* for a slot before each compile, link and archive. The daemon admits an action only when the machine's free memory, the action's measured peak (`ledger.tsv`) and the reserve all fit. So N builds run side by side without exhausting the commit limit, instead of waiting in a queue for each other.

A project of its own: C++20, the standard library and the OS API (Windows, Posix+Mac, Posix+Linux). It includes no header of any client project. The contract is `Include/BuildPool/*.hpp`.

## Attaching a CMake project

```cmake
project(MyProject LANGUAGES C CXX)
include(path/to/BuildPool/cmake/BuildPool.cmake)
buildpool_enable(CACHE ccache                      # optional; only ccache is supported
                 CACHE_WRAPPER <cmd...>            # optional; a script run in place of ccache (it calls ccache)
                 CACHE_ENV NAME=VALUE...           # optional; the project's ccache settings (CCACHE_MAXSIZE, CCACHE_BASEDIR...)
                 HOME <dir>                        # optional; see below
                 LANGUAGES C CXX OBJC OBJCXX       # optional; default: every enabled one of these
                 SOURCE_ROOT <dir> BUILD_ROOT <dir>)
```

- Call it after `project()`. The generator must be Ninja or Makefiles.
- What it sets:
  - `CMAKE_<LANG>_COMPILER_LAUNCHER` becomes `buildpool cc` (Front) → ccache → Back → compiler.
  - `CMAKE_<LANG>_LINKER_LAUNCHER` becomes `buildpool link`.
  - The static-archive rules (`CMAKE_<LANG>_CREATE_STATIC_LIBRARY`, `_ARCHIVE_CREATE/APPEND`, their `_IPO` forms) are prefixed with `buildpool link`.
- What it exports:
  - `BUILDPOOL_EXECUTABLE`
  - `BUILDPOOL_HOME`
  - `BUILDPOOL_CACHE` (true when ccache is in the chain).
- The project's own job pools (`JOB_POOL_LINK`, `-j` limits) are no longer needed: concurrency is the pool's decision.
- The module owns these ccache settings, and `CACHE_ENV` refuses them:
  - `CCACHE_DIR=<home>/ccache`
  - `CCACHE_TEMPDIR`
  - `CCACHE_CONFIGPATH` (so no user `ccache.conf` decides anything)
  - `CCACHE_COMPILERTYPE`
  - `CCACHE_COMPILERCHECK`
- Every tree of a home shares one cache.

## The home

One directory per machine, on the fastest volume. Builds share a pool only when they share a home. The configure picks it in this order:

1. `HOME` given to `buildpool_enable`;
2. the environment variable `BUILDPOOL_HOME`;
3. the cache variable `BUILDPOOL_HOME` of an earlier configure;
4. a private home in `${CMAKE_BINARY_DIR}/buildpool` (a pool of one tree; the configure says so).

The home holds:

| Path | Contents |
|---|---|
| `pool.ini` | the pool's settings and its build classes (see below); written with defaults when missing |
| `daemon.log` | start, stop, refusals, preemptions and every config value in effect |
| `ledger.tsv` | the measured peak memory and time of every action (cost key) |
| `runs.tsv` | one line per finished build: actions, cache reruns, waits, preemptions, `max_running`, `peak_commit_used_mb`, `min_headroom_mb` |
| `ccache/` | the one compile cache |
| `bin/<rev>/buildpool[.exe]` | the binary of one source revision |
| `build/<rev>` | the binary's own build |
| `trees/<id>/cmake` | build trees moved onto the home's volume (`pool.sh attach`) |

## pool.ini

```ini
[pool]
reserve_mb      = 4096        ; memory the pool never plans to use
cpu_reserve     = 1           ; logical CPUs left to everything that is not a build
window_factor   = 2           ; `buildpool window` = (logical CPUs - cpu_reserve) * window_factor
estimate_margin = 1.15
sample_ms       = 500
default_class   = normal
[class.lead]
priority = 1                  ; lower runs first
guaranteed_slots = 2          ; slots this class keeps while higher classes wait
os_priority = normal          ; normal | below_normal | idle
[class.normal]
priority = 5
guaranteed_slots = 1
os_priority = below_normal
```

A build names its class with `BUILDPOOL_CLASS`; a build that names none gets `default_class`. Class names are data: no name appears in the code.

## Revision and rebuild

The binary is identified by `rev`, a hash over `Include/`, `Source/`, `CMakeLists.txt` and `cmake/BuildPoolBootstrap.cmake` (`cmake/BuildPoolRevision.cmake`).

- The first configure of a new revision builds it once per machine, under a lock (`cmake/BuildPoolBootstrap.cmake`).
- Trees of the same sources reuse the binary.
- An edited source re-runs the configure (CONFIGURE_DEPENDS) and builds the new revision beside the old one.

The binary builds on its own:

```sh
cmake -S Tools/BuildPool -B <dir> -G Ninja
cmake --build <dir>
```

## scripts/Dev/pool.sh (DesertEngine)

| Command | What it does |
|---|---|
| `pool.sh bootstrap` | builds or finds this tree's binary in the home and prints its path |
| `pool.sh start \| status \| window \| stop` | the home's daemon; `status` and `window` start it when none runs |
| `pool.sh run [--class C] [--label L] -- <cmd...>` | runs one build as a pool build: exports `BUILDPOOL_CLASS`, `BUILDPOOL_RUN` (the `runs.tsv` label) and `CMAKE_BUILD_PARALLEL_LEVEL` (= `buildpool window`) |
| `pool.sh attach [<tree>]` | moves `<tree>/build/cmake` to `<home>/trees/<id>/cmake` once and leaves a junction (Windows) or symlink in its place, when the tree lives on another volume; the tree must not be building |
| `pool.sh adopt-cache <dir>` | moves an existing ccache directory into `<home>/ccache` once, leaving its `ccache.conf` behind |

`dev_build` (`_common.sh`) and `compile_check.sh` build through `pool.sh run`.
