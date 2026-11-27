// BuildPool contract: what the client understands of a compiler or linker command line, and the three keys
// derived from it. Implemented by package C in Source/Invocation/. Pure functions: no I/O except the response
// files and compiler files named below, no global state.
// CONTRACT HEADER: written by the architect, edited by no package.
#pragma once

#include "BuildPool/Platform.hpp"
#include "BuildPool/Protocol.hpp"
#include "BuildPool/Result.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace buildpool
{
    enum class ToolKind : uint8_t
    {
        Msvc,      // cl.exe
        ClangCl,   // clang-cl
        GccLike,   // clang, clang++, gcc, g++ (compile or link driver)
        MsvcLink,  // link.exe, lib.exe, lld-link, and "cmake -E vs_link_exe|vs_link_dll ... -- <linker> ..."
        Archiver,  // ar, libtool
        Unknown,
    };

    // The two directories of a tree that appear as absolute paths in its command lines and preprocessed text.
    // Both canonical, forward slashes, no trailing slash. They come from the launcher's own command line
    // (--source-root, --build-root), which the CMake module writes.
    struct TreeRoots
    {
        std::string source;
        std::string build;
    };

    struct Invocation
    {
        ToolKind tool = ToolKind::Unknown;
        ActionKind action = ActionKind::Other;
        // The object, PCH, image or archive the action writes ("" when none was found). As written on the
        // command line (/Fo, /Fp with /Yc, /OUT:, -o).
        std::string primaryOutput;
        // Preprocess only: true when the text goes to stdout, false when it goes to `preprocessOutput`.
        bool preprocessToStdout = false;
        std::string preprocessOutput;
        // True when killing the action at any moment and running it again gives the same files: a Compile or
        // PchCreate that writes no shared program database. False for /Zi, /ZI or /Fd with /FS (one .pdb written
        // by many compiles through mspdbsrv), for every Link, for Other.
        bool restartable = false;
    };

    // `argv` is the tool and its arguments as the launcher received them after "--". Response files ("@file",
    // UTF-8 or UTF-16 with BOM) are read to classify, never rewritten. An Unknown tool is not an error: the
    // action is Other and is admitted as such.
    Result<Invocation> ParseInvocation(std::span<const std::string> argv) noexcept;

    // Every occurrence of a root in `text` becomes a token: the build root "@BUILD@" first (it is usually inside
    // the source root), then the source root "@SOURCE@". Matching ignores the slash direction, treats a doubled
    // backslash as one, and on Windows ignores case.
    std::string NormalizeRoots(std::string_view text, const TreeRoots& roots);

    // Acquire::costKey: "<tool>|<action>|<configuration>|<primary output relative to the build root>", where
    // <configuration> is the last component of the build root (the preset, e.g. win-dev), so one configuration
    // never inherits another's cost and every tree of the same preset shares one.
    std::string CostKey(const Invocation& invocation, const TreeRoots& roots);

    // The identity of a compiler: SHA-256 over (name, size, modification time) of every regular file in the
    // directory of `compiler`, sorted by name. cl.exe is a stub; c1xx.dll and c2.dll beside it are the compiler.
    Result<std::string> CompilerId(const std::filesystem::path& compiler) noexcept;

    // Replaces every "{compiler_id}" in `value` (a --cache-env value of the launcher's command line).
    std::string ExpandCompilerId(std::string_view value, std::string_view compilerId);

    // The digest of preprocessed text that is equal in every tree holding the same sources: fed the raw stream
    // in any chunking, it applies NormalizeRoots to line-marker lines only (a line whose first non-blank
    // characters are "#line " or "# <digit>") and hashes every other line byte for byte.
    class PreprocessedDigest
    {
    public:
        explicit PreprocessedDigest(TreeRoots roots);
        void Feed(std::span<const std::byte> chunk);
        std::string Finish(); // hex; the object may not be used afterwards

    private:
        TreeRoots m_roots;
        Sha256 m_hash;
        std::string m_line; // the unfinished last line of the previous chunk
    };

    // Acquire::inflightKey: hex SHA-256 of (compilerId, every argument through NormalizeRoots, preprocessedDigest).
    // Equal keys MEAN the cache will very probably hit for the second session once the first has stored; they are
    // never trusted for correctness (a wrong "equal" costs one wait and one extra cache lookup, nothing else).
    std::string InflightKey(std::span<const std::string> argv, const TreeRoots& roots, std::string_view compilerId,
                            std::string_view preprocessedDigest);

    // The client's entry (package C, Source/Client/): `buildpool <cc|link|daemon|status|window|stop> ...`.
    //   cc      --home <dir> --source-root <dir> --build-root <dir>
    //           [--cache <program> [<argument>...]] [--cache-env <NAME=VALUE>]... -- <compiler> <arguments...>
    //   link    --home <dir> --source-root <dir> --build-root <dir> -- <link command...>
    //   daemon  --home <dir>            runs RunDaemon in this process (started detached by the first client)
    //   status | window | stop  --home <dir>
    // With BUILDPOOL_STAGE=back the process is a Back whatever its arguments: they are the compiler's.
    // Returns the process exit code: the wrapped tool's, or 1 with "buildpool: <reason>" on stderr.
    int RunClient(int argc, char** argv) noexcept;
} // namespace buildpool
