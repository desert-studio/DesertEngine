#include "Runs.hpp"

#include "Config.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

namespace buildpool::daemon
{
    uint64_t FreeBytes( const MemoryState& memory ) noexcept
    {
        return memory.commitFree ? std::min( memory.physicalAvailable, *memory.commitFree )
                                 : memory.physicalAvailable;
    }

    void RunRecord::ObserveMachine( uint32_t runningLeases, const MemoryState* memory ) noexcept
    {
        maxRunning = std::max( maxRunning, runningLeases );
        if ( memory == nullptr )
            return;
        lowestFreeBytes    = std::min( lowestFreeBytes, FreeBytes( *memory ) );
        physicalTotalBytes = std::max( physicalTotalBytes, memory->physicalTotal );
    }

    std::string FormatRunLine( const RunRecord& run )
    {
        // A build that never saw a memory sample (shorter than one tick) reports no use and no headroom.
        const bool     sampled    = run.lowestFreeBytes != std::numeric_limits<uint64_t>::max();
        const uint64_t lowestFree = sampled ? run.lowestFreeBytes : run.physicalTotalBytes;
        const uint64_t used       = run.physicalTotalBytes > lowestFree ? run.physicalTotalBytes - lowestFree : 0;

        std::ostringstream line;
        line << run.startUnix << '\t' << run.endUnix << '\t' << run.buildRoot << '\t'
             << ( run.runLabel.empty() ? "-" : run.runLabel ) << '\t' << run.className << '\t' << run.actions
             << '\t' << run.cacheReruns << '\t' << run.waitedMsTotal << '\t' << run.preemptions << '\t'
             << run.maxRunning << '\t' << used / kMiB << '\t' << lowestFree / kMiB << '\n';
        return line.str();
    }

    Status AppendRun( const std::filesystem::path& file, const RunRecord& run ) noexcept
    {
        std::string     text;
        std::error_code ec;
        if ( std::filesystem::exists( file, ec ) )
        {
            std::ifstream in( file, std::ios::binary );
            if ( !in )
                return Fail( file.string() + ": cannot be opened" );
            std::ostringstream content;
            content << in.rdbuf();
            text = content.str();
            if ( !text.empty() && text.back() != '\n' )
                text.push_back( '\n' );
        }
        else if ( ec )
        {
            return Fail( file.string() + ": " + ec.message() );
        }
        text += FormatRunLine( run );
        Status written = WriteFileAtomic( file, text );
        if ( !written )
            return Fail( file.string() + ": " + written.error().message );
        return Ok();
    }
} // namespace buildpool::daemon
