// BuildPool contract: what a client and the daemon say to each other. One connection = one client process;
// the connection closing is itself a message (the daemon releases everything that client held).
// Encode/Decode are implemented by package B in Source/Wire/; the daemon (A) and the client (C) only build and
// read the structs below.
// CONTRACT HEADER: written by the architect, edited by no package.
#pragma once

#include "BuildPool/Platform.hpp"
#include "BuildPool/Result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace buildpool
{
    // Bumped on any change of this file. A daemon that receives another number answers Refused and, when the
    // client's number is higher, drains (admits nothing new, exits when its last lease is released) so the
    // client can start the newer daemon on the same endpoint.
    inline constexpr uint32_t kProtocolVersion = 1;

    // The exit code a Back client returns to the cache when the daemon answered Retry. The Front never lets it
    // reach the build tool: it runs the cache once more (a hit) and returns that run's code.
    inline constexpr int kRetryExitCode = 113;

    // The environment a Front sets for the cache chain it starts; a Back reads it. Nobody else sets these.
    inline constexpr const char* kEnvStage = "BUILDPOOL_STAGE";       // "back"
    inline constexpr const char* kEnvSession = "BUILDPOOL_SESSION";   // decimal session id
    inline constexpr const char* kEnvCompiler = "BUILDPOOL_CC";       // the real compiler, absolute path
    inline constexpr const char* kEnvRetried = "BUILDPOOL_RETRIED";   // "1" on the second cache run
    inline constexpr const char* kEnvHome = "BUILDPOOL_HOME_DIR";     // the pool home of this build
    inline constexpr const char* kEnvSourceRoot = "BUILDPOOL_SOURCE_ROOT";
    inline constexpr const char* kEnvBuildRoot = "BUILDPOOL_BUILD_ROOT";
    // Set by whoever starts a build (scripts/Dev/pool.sh run); read by every client of that build.
    inline constexpr const char* kEnvClass = "BUILDPOOL_CLASS";       // a class name of <home>/pool.ini
    inline constexpr const char* kEnvRun = "BUILDPOOL_RUN";           // a label for runs.tsv; may be absent

    enum class Stage : uint8_t
    {
        Front,   // started by the build tool (ninja); owns the session
        Back,    // started by the cache as "the compiler"; joins the Front's session
        Control, // status / window / stop
    };

    enum class ActionKind : uint8_t
    {
        Preprocess, // the cache's preprocessor run
        Compile,    // one object
        PchCreate,  // a precompiled header (and its object)
        Link,       // an executable, a shared library or a static archive
        Other,      // anything else the launcher was put in front of
    };

    // ---- client -> daemon ------------------------------------------------------------------------------------------
    struct Hello
    {
        uint32_t protocol = kProtocolVersion;
        Stage stage = Stage::Control;
        ProcessIdentity process;
        uint64_t session = 0;    // Front, Control: 0 (the daemon assigns one). Back: the Front's (kEnvSession).
        std::string buildRoot;   // the build directory of the tree, canonical; the identity of "a build"
        std::string className;   // kEnvClass; empty = the config's default class
        std::string runLabel;    // kEnvRun; may be empty
    };

    struct Acquire
    {
        ActionKind kind = ActionKind::Other;
        // What the action costs is remembered under this key; equal in every tree for "the same" action:
        // "<tool>|<action>|<configuration>|<primary output relative to the build root>" (Invocation.hpp CostKey).
        std::string costKey;
        // Non-empty: join the in-flight table under this key (Invocation.hpp InflightKey). Empty: no dedup.
        std::string inflightKey;
        // The action can be killed at any moment and run again with the same result (see Invocation::restartable).
        bool restartable = false;
        // kEnvRetried was set: this is the second cache run of the session; the daemon never answers Await/Retry.
        bool retried = false;
    };

    // The digest of the normalised preprocessed text a Back's Preprocess action produced. Stored on the session
    // and handed to the session's later Back in Welcome::note.
    struct Note
    {
        std::string text;
    };

    // Sent by a client holding a lease every PoolConfig::sampleMs.
    struct Heartbeat
    {
        uint64_t currentBytes = 0;
    };

    struct Released
    {
        int exitCode = 0;
        uint64_t peakBytes = 0;
        uint64_t wallMs = 0;
        uint64_t cpuMs = 0;
        bool preempted = false; // the answer to Preempt: the job was killed, the client waits for a new Grant
    };

    // Front only: the cache chain exited with this code. Answered by Verdict.
    struct CacheExited
    {
        int exitCode = 0;
    };

    struct Query
    {
        enum class What : uint8_t
        {
            Status, // answered by Text: the human-readable table of builds, leases, waiters, memory
            Window, // answered by Text: one decimal number, the -j a build tool should use
            Stop,   // drain and exit; answered by Text once the last lease is released
        };
        What what = What::Status;
    };

    // ---- daemon -> client ------------------------------------------------------------------------------------------
    struct Welcome
    {
        uint64_t session = 0;
        std::string note; // the session's Note so far (empty when none)
    };

    // The client may start its process now, at this OS priority, and must send Heartbeats, then Released.
    struct Grant
    {
        uint64_t lease = 0;
        OsPriority priority = OsPriority::Normal;
    };

    // The answer to an Acquire whose inflightKey is already being compiled by another session. Informational;
    // the client keeps waiting and later receives Retry (the leader's result is in the cache) or Grant (the
    // leader failed or vanished: this client is the leader now).
    struct Await
    {
        std::string leaderBuildRoot;
    };

    // Back: exit with kRetryExitCode without running anything.
    struct Retry
    {
    };

    // Kill the job of this lease now and answer Released{preempted = true}. Only ever sent for a lease whose
    // Acquire said restartable.
    struct Preempt
    {
        uint64_t lease = 0;
    };

    // The answer to CacheExited.
    struct Verdict
    {
        bool runCacheAgain = false; // true exactly when this session's Back was answered Retry
    };

    struct Text
    {
        std::string text;
    };

    // The daemon will not serve this client; the reason is printed and the build step fails with it.
    struct Refused
    {
        std::string reason;
    };

    using Message = std::variant<Hello, Acquire, Note, Heartbeat, Released, CacheExited, Query, Welcome, Grant, Await,
                                 Retry, Preempt, Verdict, Text, Refused>;

    // Wire form: u8 variant index, then the fields in declaration order (integers little-endian fixed width,
    // bool as u8, enums as their underlying type, a string as u32 length + UTF-8 bytes). Decode rejects a
    // truncated message, an unknown index, trailing bytes and an out-of-range enum.
    std::vector<std::byte> Encode(const Message& message);
    Result<Message> Decode(std::span<const std::byte> bytes) noexcept;
} // namespace buildpool
