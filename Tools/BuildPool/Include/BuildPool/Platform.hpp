// BuildPool contract: the operating-system seam. Everything the daemon and the client need from the OS, and
// nothing else in the project includes an OS header. Implemented by package B in Source/Platform/
// (Windows; Posix shared by macOS and Linux, with one memory-probe file per kernel).
// CONTRACT HEADER: written by the architect, edited by no package.
#pragma once

#include "BuildPool/Result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace buildpool
{
    // ---- machine ---------------------------------------------------------------------------------------------------
    // The live memory of the machine, in bytes. Read by the daemon on every sample tick; never cached by a caller.
    struct MemoryState
    {
        uint64_t physicalTotal = 0;
        // Memory a new process can take without paging another one out.
        //   Windows  MEMORYSTATUSEX::ullAvailPhys
        //   macOS    (free + inactive + speculative + purgeable pages) * page size, host_statistics64
        //   Linux    MemAvailable of /proc/meminfo
        uint64_t physicalAvailable = 0;
        // Commit the system can still grant; empty where the kernel has no commit limit (macOS, Linux overcommit).
        //   Windows  MEMORYSTATUSEX::ullAvailPageFile (commit limit - commit total)
        //   Linux    CommitLimit - Committed_AS when vm.overcommit_memory == 2, else empty
        std::optional<uint64_t> commitFree;
        // The kernel's own alarm: Windows low-memory resource notification signalled, macOS
        // kern.memorystatus_vm_pressure_level >= warning, Linux PSI "some" memory avg10 > 10.
        bool osPressure = false;
    };
    Result<MemoryState> QueryMemory() noexcept;

    uint32_t LogicalCpuCount() noexcept;
    uint64_t MonotonicMs() noexcept;
    // Wall clock, seconds since the Unix epoch (the timestamps of the pool's .tsv files).
    uint64_t UnixSeconds() noexcept;

    // True when the volume holding `path` reports a seek penalty (a rotating disk).
    //   Windows  IOCTL_STORAGE_QUERY_PROPERTY / StorageDeviceSeekPenaltyProperty
    //   macOS    false (no rotating boot/data volumes are supported by the project)
    //   Linux    /sys/block/<dev>/queue/rotational
    Result<bool> HasSeekPenalty(const std::filesystem::path& path) noexcept;

    Result<std::filesystem::path> SelfExecutable() noexcept;
    std::optional<std::string> GetEnv(std::string_view name) noexcept;

    // ---- processes -------------------------------------------------------------------------------------------------
    enum class OsPriority : uint8_t
    {
        Normal,
        BelowNormal,
        Idle,
    };

    // A process is named by its id AND its start time, so a recycled pid is never mistaken for a dead client.
    struct ProcessIdentity
    {
        uint64_t pid = 0;
        uint64_t startTime = 0;
        bool operator==(const ProcessIdentity&) const = default;
    };
    ProcessIdentity SelfProcess() noexcept;

    struct ChildSpec
    {
        // argv[0] is the executable, an absolute path or a name resolved on PATH. Arguments are passed verbatim
        // (Windows: quoted by the CommandLineToArgvW rules; a "@response" argument is passed as it is).
        std::vector<std::string> argv;
        // Added to (or replacing in) the inherited environment. An empty value removes the variable.
        std::vector<std::pair<std::string, std::string>> environment;
        OsPriority priority = OsPriority::Normal;
        // false: the child inherits this process's stdout. true: its stdout is a pipe read by ChildJob::ReadStdout.
        // stderr and stdin are always inherited.
        bool captureStdout = false;
    };

    struct JobUsage
    {
        uint64_t currentBytes = 0; // commit (Windows: job private bytes; Posix: sum of phys_footprint / RSS+swap)
        uint64_t peakBytes = 0;    // the highest currentBytes the tree ever had (Windows: PeakJobMemoryUsed)
        uint64_t cpuMs = 0;        // user + kernel, the whole tree
    };

    // A child process and every process it starts, as one unit: measured together, killed together.
    //   Windows  a Job Object with KILL_ON_JOB_CLOSE (nested inside whatever job the client itself runs in)
    //   Posix    a process group; descendants are enumerated for Usage()
    // RAII: destroying a ChildJob that still runs terminates the whole tree. Move-only.
    class ChildJob
    {
    public:
        static Result<ChildJob> Spawn(const ChildSpec& spec) noexcept;

        ChildJob(ChildJob&&) noexcept;
        ChildJob& operator=(ChildJob&&) noexcept;
        ChildJob(const ChildJob&) = delete;
        ChildJob& operator=(const ChildJob&) = delete;
        ~ChildJob();

        // The exit code once the direct child has exited; empty after `timeoutMs` without an exit.
        Result<std::optional<int>> Wait(uint32_t timeoutMs) noexcept;
        // Captured stdout: bytes read into `into` (0 = end of stream). Blocks at most `timeoutMs`; a timeout
        // returns an empty optional. Only with ChildSpec::captureStdout.
        Result<std::optional<size_t>> ReadStdout(std::span<std::byte> into, uint32_t timeoutMs) noexcept;
        JobUsage Usage() const noexcept;
        // Kills the whole tree and waits for it. Idempotent.
        void Terminate() noexcept;

    private:
        struct Impl;
        explicit ChildJob(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> m_impl;
    };

    // Starts `argv` detached from this process and its job/process group, with stdout and stderr appended to
    // `logFile`. Used only to start the daemon.
    Status SpawnDetached(const std::vector<std::string>& argv, const std::filesystem::path& logFile) noexcept;

    // ---- the daemon's endpoint -------------------------------------------------------------------------------------
    // One endpoint per pool home, derived from the home's canonical path:
    //   Windows  \\.\pipe\buildpool-<first 16 hex digits of SHA-256(lower-cased home)>
    //   Posix    /tmp/buildpool-<uid>-<same 16 digits>.sock  (short on purpose: sun_path holds 104..108 bytes)
    std::string EndpointFor(const std::filesystem::path& home);

    // Messages are opaque byte strings here (BuildPool/Protocol.hpp encodes them); the platform frames them
    // (u32 little-endian length, then the bytes; a message over 1 MiB is an error) and delivers them whole.

    // The client's end. Move-only; closing it is how the daemon learns that a client is gone.
    class Connection
    {
    public:
        // Fails with "no daemon" in the message when nothing listens on the endpoint (the client then starts one).
        static Result<Connection> Connect(const std::string& endpoint, uint32_t timeoutMs) noexcept;

        Connection(Connection&&) noexcept;
        Connection& operator=(Connection&&) noexcept;
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;
        ~Connection();

        Status Send(std::span<const std::byte> message) noexcept;
        // One whole message; an empty optional after `timeoutMs` without one; an Error when the daemon is gone.
        Result<std::optional<std::vector<std::byte>>> Receive(uint32_t timeoutMs) noexcept;

    private:
        struct Impl;
        explicit Connection(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> m_impl;
    };

    using ClientId = uint64_t; // unique for the life of the Server, never reused

    struct ServerEvent
    {
        enum class Kind : uint8_t
        {
            Connected,
            Message,
            Closed,
        };
        Kind kind = Kind::Closed;
        ClientId client = 0;
        std::vector<std::byte> message; // Kind::Message only
    };

    // The daemon's end: one thread calls Poll in a loop and owns all state (Active Object; no other thread
    // touches the scheduler). Move-only.
    class Server
    {
    public:
        // Fails with "already running" in the message when another daemon holds the endpoint: the endpoint is the
        // single-instance lock (Windows FILE_FLAG_FIRST_PIPE_INSTANCE; Posix bind after a connect probe of a stale
        // socket file).
        static Result<Server> Listen(const std::string& endpoint) noexcept;

        Server(Server&&) noexcept;
        Server& operator=(Server&&) noexcept;
        Server(const Server&) = delete;
        Server& operator=(const Server&) = delete;
        ~Server();

        // Appends what happened within `timeoutMs` (possibly nothing) to `events`, in arrival order per client.
        Status Poll(uint32_t timeoutMs, std::vector<ServerEvent>& events) noexcept;
        // An Error means the client is gone; a Closed event for it follows from Poll.
        Status Send(ClientId client, std::span<const std::byte> message) noexcept;
        void Drop(ClientId client) noexcept;

    private:
        struct Impl;
        explicit Server(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> m_impl;
    };

    // ---- files and digests -----------------------------------------------------------------------------------------
    using Digest = std::array<uint8_t, 32>;
    std::string ToHex(const Digest& digest);

    // SHA-256 from the OS (Windows BCrypt, macOS CommonCrypto, Linux: the kernel's AF_ALG "sha256"). Move-only.
    class Sha256
    {
    public:
        Sha256() noexcept;
        Sha256(Sha256&&) noexcept;
        Sha256& operator=(Sha256&&) noexcept;
        ~Sha256();

        void Update(std::span<const std::byte> bytes) noexcept;
        void Update(std::string_view text) noexcept;
        Digest Finish() noexcept; // the object may not be used afterwards

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

    // Writes `bytes` to a temporary file beside `path` and renames it over `path` (readers see the old file or
    // the new one, never a half).
    Status WriteFileAtomic(const std::filesystem::path& path, std::string_view bytes) noexcept;
} // namespace buildpool
