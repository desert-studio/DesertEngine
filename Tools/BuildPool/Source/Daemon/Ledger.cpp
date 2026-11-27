#include "Ledger.hpp"

#include "Config.hpp"

#include "BuildPool/Platform.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace buildpool::daemon
{
    std::string_view CostClassOf( std::string_view costKey ) noexcept
    {
        const size_t first = costKey.find( '|' );
        if ( first == std::string_view::npos )
            return costKey;
        const size_t second = costKey.find( '|', first + 1 );
        if ( second == std::string_view::npos )
            return costKey;
        return costKey.substr( 0, second );
    }

    namespace
    {
        uint64_t ToMbRoundedUp( uint64_t bytes ) noexcept
        {
            return ( bytes + kMiB - 1 ) / kMiB;
        }

        bool ParseField( std::string_view text, uint64_t& out ) noexcept
        {
            const auto* end    = text.data() + text.size();
            const auto  parsed = std::from_chars( text.data(), end, out );
            return parsed.ec == std::errc{} && parsed.ptr == end;
        }
    } // namespace

    uint64_t Ledger::Entry::PeakEstimateBase() const noexcept
    {
        const uint64_t valid   = std::min<uint64_t>( samples, 3 );
        uint64_t       largest = 0;
        for ( uint64_t i = 0; i < valid; ++i )
            largest = std::max( largest, peaks[i] );
        return largest;
    }

    uint64_t Ledger::EntryEstimate( const Entry& entry ) const noexcept
    {
        const auto planned = static_cast<uint64_t>( static_cast<double>( entry.PeakEstimateBase() ) * m_margin );
        return std::max( planned, entry.floorBytes );
    }

    void Ledger::CostClass::Refresh( double margin ) const
    {
        if ( aggregatesValid )
            return;
        maxEstimate = 0;
        maxWallMs   = 0;
        seen        = false;
        for ( const auto& [key, entry] : keys )
        {
            const auto planned = static_cast<uint64_t>( static_cast<double>( entry.PeakEstimateBase() ) * margin );
            maxEstimate        = std::max( { maxEstimate, planned, entry.floorBytes } );
            maxWallMs          = std::max( maxWallMs, entry.wallMs );
            seen               = seen || entry.samples > 0;
        }
        aggregatesValid = true;
    }

    Ledger::Ledger( std::filesystem::path file, double estimateMargin )
         : m_file( std::move( file ) ), m_margin( estimateMargin )
    {
    }

    Result<Ledger> Ledger::Load( std::filesystem::path file, double estimateMargin ) noexcept
    {
        Ledger          ledger( file, estimateMargin );
        std::error_code ec;
        const bool      exists = std::filesystem::exists( file, ec );
        if ( ec )
            return Fail( file.string() + ": " + ec.message() );
        if ( !exists )
            return ledger;

        std::ifstream in( file, std::ios::binary );
        if ( !in )
            return Fail( file.string() + ": cannot be opened" );
        std::string line;
        size_t      lineNumber = 0;
        while ( std::getline( in, line ) )
        {
            ++lineNumber;
            if ( !line.empty() && line.back() == '\r' )
                line.pop_back();
            if ( line.empty() )
                continue;
            std::vector<std::string_view> fields;
            size_t                        start = 0;
            while ( true )
            {
                const size_t tab = line.find( '\t', start );
                fields.emplace_back( line.data() + start,
                                     ( tab == std::string::npos ? line.size() : tab ) - start );
                if ( tab == std::string::npos )
                    break;
                start = tab + 1;
            }
            if ( fields.size() != 7 || fields[0].empty() )
                return Fail(
                     file.string() + ":" + std::to_string( lineNumber ) +
                     ": expected 7 tab-separated fields (cost_key peak_mb_1 peak_mb_2 peak_mb_3 wall_ms cpu_ms "
                     "samples), found " +
                     std::to_string( fields.size() ) );
            uint64_t numbers[6] = {};
            for ( size_t i = 0; i < 6; ++i )
                if ( !ParseField( fields[i + 1], numbers[i] ) )
                    return Fail( file.string() + ":" + std::to_string( lineNumber ) + ": field " +
                                 std::to_string( i + 2 ) + " is not a whole number: '" +
                                 std::string( fields[i + 1] ) + "'" );
            Entry& entry   = ledger.Touch( fields[0] );
            entry.peaks[0] = numbers[0] * kMiB;
            entry.peaks[1] = numbers[1] * kMiB;
            entry.peaks[2] = numbers[2] * kMiB;
            entry.wallMs   = numbers[3];
            entry.cpuMs    = numbers[4];
            entry.samples  = numbers[5];
        }
        ledger.m_dirty = false;
        return ledger;
    }

    Ledger::Entry& Ledger::Touch( std::string_view costKey )
    {
        const std::string_view className = CostClassOf( costKey );
        auto                   classIt   = m_classes.find( className );
        if ( classIt == m_classes.end() )
            classIt = m_classes.emplace( std::string( className ), CostClass{} ).first;
        CostClass& costClass      = classIt->second;
        costClass.aggregatesValid = false;
        auto keyIt                = costClass.keys.find( costKey );
        if ( keyIt == costClass.keys.end() )
        {
            keyIt = costClass.keys.emplace( std::string( costKey ), Entry{} ).first;
            ++m_keyCount;
        }
        m_dirty = true;
        return keyIt->second;
    }

    const Ledger::Entry* Ledger::Find( std::string_view costKey, const CostClass** costClass ) const
    {
        const auto classIt = m_classes.find( CostClassOf( costKey ) );
        if ( classIt == m_classes.end() )
        {
            *costClass = nullptr;
            return nullptr;
        }
        *costClass       = &classIt->second;
        const auto keyIt = classIt->second.keys.find( costKey );
        return keyIt == classIt->second.keys.end() ? nullptr : &keyIt->second;
    }

    void Ledger::Record( std::string_view costKey, uint64_t peakBytes, uint64_t wallMs, uint64_t cpuMs )
    {
        Entry& entry   = Touch( costKey );
        entry.peaks[2] = entry.peaks[1];
        entry.peaks[1] = entry.peaks[0];
        entry.peaks[0] = peakBytes;
        entry.wallMs   = wallMs;
        entry.cpuMs    = cpuMs;
        entry.samples += 1;
        entry.floorBytes = 0;
    }

    void Ledger::Raise( std::string_view costKey, uint64_t bytes )
    {
        Entry& entry     = Touch( costKey );
        entry.floorBytes = std::max( entry.floorBytes, bytes );
    }

    uint64_t Ledger::Estimate( std::string_view costKey ) const
    {
        const CostClass* costClass = nullptr;
        const Entry*     entry     = Find( costKey, &costClass );
        if ( entry != nullptr && ( entry->samples > 0 || entry->floorBytes > 0 ) )
            return EntryEstimate( *entry );
        if ( costClass != nullptr )
        {
            costClass->Refresh( m_margin );
            if ( costClass->seen )
                return std::max( costClass->maxEstimate, entry != nullptr ? entry->floorBytes : 0 );
        }
        uint64_t largest = 0;
        for ( const auto& [name, other] : m_classes )
        {
            other.Refresh( m_margin );
            largest = std::max( largest, other.maxEstimate );
        }
        return largest;
    }

    bool Ledger::ClassSeen( std::string_view costKey ) const
    {
        const auto classIt = m_classes.find( CostClassOf( costKey ) );
        if ( classIt == m_classes.end() )
            return false;
        classIt->second.Refresh( m_margin );
        return classIt->second.seen;
    }

    uint64_t Ledger::WallMs( std::string_view costKey ) const
    {
        const CostClass* costClass = nullptr;
        const Entry*     entry     = Find( costKey, &costClass );
        if ( entry != nullptr && entry->samples > 0 )
            return entry->wallMs;
        if ( costClass != nullptr )
        {
            costClass->Refresh( m_margin );
            if ( costClass->seen )
                return costClass->maxWallMs;
        }
        uint64_t longest = 0;
        for ( const auto& [name, other] : m_classes )
        {
            other.Refresh( m_margin );
            longest = std::max( longest, other.maxWallMs );
        }
        return longest;
    }

    Status Ledger::Save()
    {
        std::ostringstream text;
        for ( const auto& [className, costClass] : m_classes )
            for ( const auto& [key, entry] : costClass.keys )
            {
                if ( entry.samples == 0 )
                    continue;
                const uint64_t valid = std::min<uint64_t>( entry.samples, 3 );
                text << key;
                for ( uint64_t i = 0; i < 3; ++i )
                    text << '\t' << ( i < valid ? ToMbRoundedUp( entry.peaks[i] ) : 0 );
                text << '\t' << entry.wallMs << '\t' << entry.cpuMs << '\t' << entry.samples << '\n';
            }
        Status written = WriteFileAtomic( m_file, text.str() );
        if ( !written )
            return Fail( m_file.string() + ": " + written.error().message );
        m_dirty = false;
        return Ok();
    }
} // namespace buildpool::daemon
