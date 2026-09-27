#include "CrashReport.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

namespace CrashReporter
{
    namespace
    {
        constexpr const char* kMagic = "DESERTCRASH";

        // The writer replaces CR and LF inside values, so a line never carries one - but a file
        // written on Windows and read in text mode can still arrive with a trailing CR.
        void StripTrailingCarriageReturn( std::string& ioLine )
        {
            if ( !ioLine.empty() && ioLine.back() == '\r' )
            {
                ioLine.pop_back();
            }
        }

        // Splits the value of a frame= line into its five pipe-separated fields. The field count is
        // fixed at five by the contract, so anything else is a garbled line and is reported as one.
        bool ParseStackValue( const std::string& inValue, StackFrame& outFrame )
        {
            std::string fields[5];
            std::size_t field = 0;
            for ( const char c : inValue )
            {
                if ( c == '|' )
                {
                    ++field;
                    if ( field >= 5 )
                    {
                        return false;
                    }
                    continue;
                }
                fields[field] += c;
            }
            if ( field != 4 )
            {
                return false;
            }
            outFrame.index    = fields[0];
            outFrame.address  = fields[1];
            outFrame.module   = fields[2];
            outFrame.function = fields[3];
            outFrame.source   = fields[4];
            return true;
        }
    } // namespace

    Report ParseCrashText( const std::string& inText, const std::string& inSourcePath )
    {
        Report report;
        report.sourcePath = inSourcePath;
        report.rawText    = inText;

        const std::size_t magicLength = std::string( kMagic ).size();

        std::istringstream stream( inText );
        std::string        line;

        if ( !std::getline( stream, line ) )
        {
            report.error = "the crash report is empty (0 bytes): " + inSourcePath;
            return report;
        }
        StripTrailingCarriageReturn( line );

        // The magic line is the whole validity test. Everything after it may legally be truncated.
        if ( line.size() < magicLength || line.compare( 0, magicLength, kMagic ) != 0 )
        {
            report.error = "not a DESERTCRASH report - its first line reads " + line + " : " + inSourcePath;
            return report;
        }
        report.formatVersion = std::atoi( line.substr( magicLength ).c_str() );
        if ( report.formatVersion != 1 )
        {
            report.error = "unsupported DESERTCRASH format version " + std::to_string( report.formatVersion ) +
                           " (this reporter reads version 1): " + inSourcePath;
            return report;
        }

        std::string section;
        std::size_t garbledStackLines = 0;

        while ( std::getline( stream, line ) )
        {
            StripTrailingCarriageReturn( line );
            if ( line.empty() )
            {
                continue;
            }
            if ( line.front() == '[' && line.back() == ']' )
            {
                section = line.substr( 1, line.size() - 2 );
                continue;
            }

            const std::size_t equals = line.find( '=' );
            if ( equals == std::string::npos )
            {
                // An unknown line shape. Ignored by contract: fields are only ever appended.
                continue;
            }
            const std::string key   = line.substr( 0, equals );
            const std::string value = line.substr( equals + 1 );

            // An unknown section or key is ignored, which is what makes a newer writer readable.
            if ( section == "report" )
            {
                if ( key == "host" )
                    report.host = value;
                else if ( key == "pid" )
                    report.pid = value;
                else if ( key == "tid" )
                    report.tid = value;
                else if ( key == "started" )
                    report.started = value;
                else if ( key == "crash_epoch" )
                    report.crashEpoch = value;
            }
            else if ( section == "exception" )
            {
                if ( key == "kind" )
                    report.kind = value;
                else if ( key == "code" )
                    report.code = value;
                else if ( key == "codename" )
                    report.codename = value;
                else if ( key == "address" )
                    report.address = value;
                else if ( key == "synthesized" )
                    report.synthesized = value;
                else if ( key == "module" )
                    report.module = value;
                else if ( key == "module_offset" )
                    report.moduleOffset = value;
                else if ( key == "function" )
                    report.function = value;
                else if ( key == "fault_frame" )
                    report.faultFrame = value;
            }
            else if ( section == "build" )
            {
                if ( key == "version" )
                    report.version = value;
                else if ( key == "sha" )
                    report.sha = value;
                else if ( key == "branch" )
                    report.branch = value;
                else if ( key == "dirty" )
                    report.dirty = value;
            }
            else if ( section == "context" )
            {
                if ( key == "machine" )
                    report.machine = value;
                else if ( key == "scene" )
                    report.scene = value;
                else if ( key == "os" )
                    report.os = value;
                else if ( key == "gpu" )
                    report.gpu = value;
            }
            else if ( section == "stack" && key == "frame" )
            {
                StackFrame frame;
                if ( ParseStackValue( value, frame ) )
                {
                    report.frames.push_back( frame );
                }
                else
                {
                    ++garbledStackLines;
                }
            }
            else if ( section == "log" && key == "log" )
            {
                report.log.push_back( value );
            }
            else if ( section == "end" && key == "written" )
            {
                report.complete = ( value == "complete" );
            }
        }

        if ( garbledStackLines != 0 )
        {
            // Not fatal: the summary is still worth showing. But it is named, with a count, because
            // a stack that quietly lost frames is the one thing a reader would trust wrongly.
            report.error = std::to_string( garbledStackLines ) +
                           " [stack] line(s) did not carry the five pipe-separated fields the format "
                           "requires and were dropped: " +
                           inSourcePath;
        }

        report.valid = true;
        return report;
    }

    Report LoadReport( const std::filesystem::path& inDirectory )
    {
        Report report;

        if ( inDirectory.empty() )
        {
            report.error = "no crash report directory was given on the command line "
                           "(usage: DesertCrashReporter <report directory>)";
            return report;
        }

        const std::filesystem::path file = inDirectory / "crash.txt";
        report.sourcePath                = file.string();

        std::error_code existsError;
        if ( !std::filesystem::exists( file, existsError ) )
        {
            if ( existsError )
            {
                report.error = "could not look for the crash report (" + existsError.message() + ", error " +
                               std::to_string( existsError.value() ) + "): " + file.string();
            }
            else
            {
                report.error = "no crash report at this path - the file does not exist: " + file.string();
            }
            return report;
        }

        const std::ifstream input( file, std::ios::binary );
        if ( !input )
        {
            report.error = "the crash report exists but could not be opened for reading: " + file.string();
            return report;
        }

        std::ostringstream buffer;
        buffer << input.rdbuf();
        return ParseCrashText( buffer.str(), file.string() );
    }

    std::string FormatCrashTime( const Report& inReport )
    {
        if ( inReport.crashEpoch.empty() )
        {
            return "unknown";
        }
        char*           end   = nullptr;
        const long long epoch = std::strtoll( inReport.crashEpoch.c_str(), &end, 10 );
        if ( end == inReport.crashEpoch.c_str() || epoch <= 0 )
        {
            return inReport.crashEpoch + " (not a timestamp)";
        }

        const auto asTime = static_cast<std::time_t>( epoch );
        std::tm    local{};
#if defined( _WIN32 )
        if ( ::localtime_s( &local, &asTime ) != 0 )
        {
            return inReport.crashEpoch + " (could not be converted to a local time)";
        }
#else
        if ( ::localtime_r( &asTime, &local ) == nullptr )
        {
            return inReport.crashEpoch + " (could not be converted to a local time)";
        }
#endif
        char text[64] = {};
        if ( std::strftime( text, sizeof( text ), "%Y-%m-%d %H:%M:%S", &local ) == 0 )
        {
            return inReport.crashEpoch;
        }
        return std::string( text ) + " local";
    }
} // namespace CrashReporter
