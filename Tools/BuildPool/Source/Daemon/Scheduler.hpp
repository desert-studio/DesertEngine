// The pool's state machine: builds, sessions, leases, the queue and the in-flight table. No OS call, no file,
// no clock of its own: events come in (a message, a connection closing, a memory sample, each with the time),
// what to send comes out. One thread owns it (Active Object, BuildPool/Platform.hpp Server). The rules it
// implements are the normative list of BuildPool/Daemon.hpp.
#pragma once

#include "Ledger.hpp"
#include "Runs.hpp"

#include "BuildPool/Daemon.hpp"
#include "BuildPool/Platform.hpp"
#include "BuildPool/Protocol.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace buildpool::daemon
{
    struct Now
    {
        uint64_t ms   = 0; // MonotonicMs
        uint64_t unixSeconds = 0; // UnixSeconds
    };

    // What one event produced. The daemon sends, drops, logs and appends in this order, then clears it.
    struct Outbox
    {
        std::vector<std::pair<ClientId, Message>> sends;
        std::vector<ClientId>                     drops; // after their sends (a Refused goes out first)
        std::vector<std::string>                  log;
        std::vector<RunRecord>                    finishedRuns;
        bool exit = false; // drained after a Stop or a newer protocol: the daemon saves the ledger and returns 0

        void Clear();
    };

    class Scheduler
    {
    public:
        // `config` and `ledger` outlive the scheduler. `identity` heads the status table (pid, start time), so a
        // restarted daemon is told apart from the one before it.
        Scheduler( const PoolConfig& config, Ledger& ledger, uint32_t logicalCpus, std::string identity,
                   uint64_t firstSession );

        void OnConnected( ClientId client, Now now, Outbox& out );
        void OnMessage( ClientId client, const Message& message, Now now, Outbox& out );
        // The connection is gone: everything it held is released at once (rule Loss). Unknown ids are ignored.
        void OnClosed( ClientId client, Now now, Outbox& out );
        // One sample tick with a fresh reading of the machine.
        void OnTick( const MemoryState& memory, Now now, Outbox& out );
        // A sample tick whose reading failed: nothing is admitted until a reading succeeds.
        void OnMemoryUnavailable( const std::string& reason, Now now, Outbox& out );
        // The daemon is leaving (stop, lost endpoint): every build still open is closed into finishedRuns.
        void CloseAllRuns( Now now, Outbox& out );

        uint32_t Slots() const noexcept
        {
            return m_slots;
        }

    private:
        struct Action
        {
            enum class State : uint8_t
            {
                Queued,   // waits for a slot and headroom
                Awaiting, // waits for an in-flight leader; holds no slot
                Running,  // holds a lease
            };
            Acquire  request;
            State    state         = State::Queued;
            int64_t  order         = 0; // arrival; a preempted action gets a key below every arrival
            uint64_t queuedSinceMs = 0;
            uint64_t lease         = 0;
            uint64_t grantMs       = 0;
            uint64_t estimate      = 0; // planned bytes, fixed at Grant
            uint64_t currentBytes  = 0; // last Heartbeat
            uint64_t observedBytes = 0; // largest Heartbeat
            uint32_t preemptions   = 0;
            bool     preemptSent   = false;
        };

        struct Client
        {
            bool                  greeted = false;
            Stage                 stage   = Stage::Control;
            ProcessIdentity       process;
            uint64_t              session = 0; // 0: none (Control, or a Front whose session has ended)
            std::string           buildKey;    // empty for Control
            std::optional<Action> action;
            bool                  waitsForStop = false;
        };

        struct Session
        {
            ClientId    front = 0;
            std::string buildKey;
            std::string note;
            bool        retryAnswered = false; // a Back of this session was answered Retry since the last Verdict
            bool        adopted       = false; // opened by a Back after a daemon restart: no Front on this daemon
            uint32_t    backs         = 0;     // adopted only: Backs connected now; the session ends at 0
            std::vector<std::string> leads;    // inflight keys this session leads
        };

        struct Inflight
        {
            uint64_t    leaderSession = 0;
            std::string leaderBuildRoot;
            bool        leaderSucceeded = false; // the leader's lease under this key was Released with exit code 0
            std::vector<ClientId> waiters;       // Awaiting, oldest first
        };

        struct Build
        {
            size_t    classIndex  = 0;
            uint32_t  clients     = 0;
            uint32_t  running     = 0;
            uint64_t  idleSinceMs = 0;
            uint64_t  startMs     = 0;
            RunRecord run;
        };

        void Refuse( ClientId id, std::string reason, Outbox& out );
        void OnHello( ClientId id, Client& client, const Hello& hello, Now now, Outbox& out );
        void OnAcquire( ClientId id, Client& client, const Acquire& acquire, Now now, Outbox& out );
        void OnReleased( ClientId id, Client& client, const Released& released, Now now, Outbox& out );
        void OnCacheExited( ClientId id, Client& client, const CacheExited& exited, Now now, Outbox& out );
        void OnQuery( ClientId id, Client& client, const Query& query, Now now, Outbox& out );

        void EndSession( uint64_t sessionId, Now now, Outbox& out );
        void ResolveInflight( const std::string& key, Now now, Outbox& out );
        void FreeLease( Client& client );
        void GrantLease( ClientId id, Client& client, uint64_t estimate, Now now, Outbox& out );
        void Admit( Now now, Outbox& out );
        void Guard( Now now, Outbox& out );
        void RetireIdleBuilds( Now now, Outbox& out );
        void StartDrain( const std::string& reason, Outbox& out );
        void CheckDrained( Outbox& out );
        void ObserveAllBuilds();

        const ClassPolicy& ClassOf( const Build& build ) const
        {
            return m_config.classes[build.classIndex];
        }
        Build*      FindBuild( const std::string& key );
        uint64_t    FreeNow() const noexcept;
        int64_t     HeadroomNow() const;
        std::string StatusText( Now now ) const;

        const PoolConfig& m_config;
        Ledger&           m_ledger;
        uint32_t          m_slots = 1;
        std::string       m_identity;

        std::unordered_map<ClientId, Client>      m_clients;
        std::unordered_map<uint64_t, Session>     m_sessions;
        std::unordered_map<std::string, Inflight> m_inflight;
        std::map<std::string, Build>              m_builds;
        std::unordered_map<uint64_t, ClientId>    m_leases;
        uint32_t                                  m_running = 0;

        uint64_t m_nextSession = 1;
        uint64_t m_nextLease   = 1;
        int64_t  m_nextArrival = 1;
        int64_t  m_nextHead    = -1;

        std::optional<MemoryState> m_memory; // empty until the first successful sample, and after a failed one
        std::string                m_memoryError;
        bool                       m_guardClosed       = false;
        uint64_t                   m_lastNoVictimLogMs = 0;
        bool                       m_draining          = false;
        std::string                m_drainReason;
    };
} // namespace buildpool::daemon
