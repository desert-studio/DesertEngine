// The daemon's memory of what an action costs: <home>/ledger.tsv (format in BuildPool/Daemon.hpp). The ledger is
// the single owner of every estimate; the scheduler asks it and never keeps its own copy of a past peak.
#pragma once

#include "BuildPool/Result.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace buildpool::daemon
{
    // "<tool>|<action>" of a costKey "<tool>|<action>|<configuration>|<output>" (the whole key when it has fewer
    // fields).
    std::string_view CostClassOf( std::string_view costKey ) noexcept;

    class Ledger
    {
    public:
        // A missing file is an empty ledger; a line that does not parse is an Error naming the line.
        static Result<Ledger> Load( std::filesystem::path file, double estimateMargin ) noexcept;

        // A finished (not preempted) action. Keeps the last three peaks, newest first.
        void Record( std::string_view costKey, uint64_t peakBytes, uint64_t wallMs, uint64_t cpuMs );
        // Guard: the key's estimate becomes max(estimate, bytes) until its next Record.
        void Raise( std::string_view costKey, uint64_t bytes );

        // The rule Estimate of BuildPool/Daemon.hpp: the key's (largest of its last 3 peaks) * margin; a key never
        // recorded: the largest estimate of its cost class; a class never recorded: the largest estimate in the
        // ledger (0 when it is empty).
        uint64_t Estimate( std::string_view costKey ) const;
        // True once any key of the cost class has a recorded peak (the end of the class's slow start).
        bool ClassSeen( std::string_view costKey ) const;
        // The wall time to plan an action at: its key's last wall, else its class's longest, else the ledger's
        // longest (0 when it is empty).
        uint64_t WallMs( std::string_view costKey ) const;

        size_t KeyCount() const noexcept
        {
            return m_keyCount;
        }
        bool Dirty() const noexcept
        {
            return m_dirty;
        }
        // Rewrites the whole file atomically; clears Dirty on success.
        Status Save();

    private:
        struct Entry
        {
            uint64_t peaks[3]   = { 0, 0, 0 }; // bytes, newest first; the first min(samples, 3) are valid
            uint64_t wallMs     = 0;
            uint64_t cpuMs      = 0;
            uint64_t samples    = 0;
            uint64_t floorBytes = 0; // Raise; not persisted
            uint64_t PeakEstimateBase() const noexcept;
        };
        struct CostClass
        {
            std::map<std::string, Entry, std::less<>> keys;
            mutable bool                              aggregatesValid = false;
            mutable uint64_t                          maxEstimate     = 0;
            mutable uint64_t                          maxWallMs       = 0;
            mutable bool                              seen            = false;
            void                                      Refresh( double margin ) const;
        };

        Ledger( std::filesystem::path file, double estimateMargin );
        Entry&       Touch( std::string_view costKey );
        const Entry* Find( std::string_view costKey, const CostClass** costClass ) const;
        uint64_t     EntryEstimate( const Entry& entry ) const noexcept;

        std::filesystem::path                         m_file;
        double                                        m_margin = 1.0;
        std::map<std::string, CostClass, std::less<>> m_classes;
        size_t                                        m_keyCount = 0;
        bool                                          m_dirty    = false;
    };
} // namespace buildpool::daemon
