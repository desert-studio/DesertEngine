#pragma once

// THE ONE READER OF A PROCESS'S STANDARD INPUT for console-driven runs (NET-SCN-VERBS): DesertServer's console,
// DesertNetProbe --mesh-peer and --bot. net_scenario.py's «Консоль <role> <line>» writes a line to the role's
// stdin; a detached thread collects whole lines here and the owner takes them at its own step, so a console line
// is applied between two steps and never inside one. Before this header the same class lived twice (Server
// main.cpp ServerConsole, DesertNetProbe PeerConsole) and a third copy was about to be written for the bot.
//
// Header-only: no build list names it.
//
// EXIT RULE for a process that called Start(): leave through std::_Exit after flushing the log. The thread may
// sit in std::getline on std::cin forever (the runner keeps the pipe open until the run ends); tearing the
// iostreams down under it races (DesertNetProbe --mesh-peer, NET-HANDOVER-SCN-DUP-OFFER fix 1). Nothing here
// joins or stops the thread: there is no portable way to interrupt a blocking read of stdin.

#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Desert::Common
{
    class StdinConsole
    {
    public:
        StdinConsole()                                 = default;
        StdinConsole( const StdinConsole& )            = delete;
        StdinConsole& operator=( const StdinConsole& ) = delete;

        // Starts the reader thread (once per process; a second Start on any instance is a programming error the
        // caller never makes: one console per process).
        void Start()
        {
            std::thread(
                 [this]
                 {
                     std::string line;
                     while ( std::getline( std::cin, line ) )
                     {
                         const std::scoped_lock lock( m_Mutex );
                         m_Lines.push_back( line );
                     }
                 } )
                 .detach();
        }

        // Every whole line read since the last Take, in order; empty when none.
        [[nodiscard]] std::vector<std::string> Take()
        {
            const std::scoped_lock lock( m_Mutex );
            return std::exchange( m_Lines, {} );
        }

    private:
        std::mutex               m_Mutex;
        std::vector<std::string> m_Lines;
    };
} // namespace Desert::Common
