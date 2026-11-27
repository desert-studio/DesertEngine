// LoadConfig: <home>/pool.ini. The policy of the pool is data; this file only parses and validates it.
#include "Config.hpp"

#include "BuildPool/Platform.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>

namespace buildpool
{
    namespace
    {
        std::string_view Trim( std::string_view text ) noexcept
        {
            const auto first = text.find_first_not_of( " \t\r\n" );
            if ( first == std::string_view::npos )
                return {};
            const auto last = text.find_last_not_of( " \t\r\n" );
            return text.substr( first, last - first + 1 );
        }

        bool IsNameChar( char c ) noexcept
        {
            return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '_' ||
                   c == '-';
        }

        bool IsName( std::string_view text ) noexcept
        {
            return !text.empty() && std::all_of( text.begin(), text.end(), IsNameChar );
        }

        template <class Integer>
        bool ParseInteger( std::string_view text, Integer& out ) noexcept
        {
            const auto* end    = text.data() + text.size();
            const auto  parsed = std::from_chars( text.data(), end, out );
            return parsed.ec == std::errc{} && parsed.ptr == end;
        }

        bool ParseReal( std::string_view text, double& out )
        {
            // std::strtod in the "C" locale the daemon never changes; from_chars<double> is missing in older
            // libc++.
            const std::string copy( text );
            char*             end = nullptr;
            out                   = std::strtod( copy.c_str(), &end );
            return !copy.empty() && end == copy.c_str() + copy.size();
        }

        bool ParsePriority( std::string_view text, OsPriority& out ) noexcept
        {
            if ( text == "normal" )
                out = OsPriority::Normal;
            else if ( text == "below_normal" )
                out = OsPriority::BelowNormal;
            else if ( text == "idle" )
                out = OsPriority::Idle;
            else
                return false;
            return true;
        }

        const char* PriorityName( OsPriority priority ) noexcept
        {
            switch ( priority )
            {
                case OsPriority::Normal:
                    return "normal";
                case OsPriority::BelowNormal:
                    return "below_normal";
                case OsPriority::Idle:
                    return "idle";
            }
            return "normal";
        }

        std::string DefaultText( uint64_t reserveMb )
        {
            std::ostringstream text;
            text << "# BuildPool policy. Written with the defaults because the file was missing; edit freely.\n"
                 << "# An unknown key is an error: the daemon refuses to start and names it.\n"
                 << "[pool]\n"
                 << "reserve_mb = " << reserveMb << "\n"
                 << "cpu_reserve = 1\n"
                 << "window_factor = 2\n"
                 << "estimate_margin = 1.15\n"
                 << "sample_ms = 500\n"
                 << "default_class = normal\n"
                 << "\n"
                 << "# [class.<name>]: priority (lower runs first), guaranteed_slots, os_priority = "
                    "normal|below_normal|idle\n"
                 << "[class.normal]\n"
                 << "priority = 5\n"
                 << "guaranteed_slots = 1\n"
                 << "os_priority = normal\n";
            return text.str();
        }

        Error LineError( const std::filesystem::path& file, size_t line, const std::string& what )
        {
            return Fail( file.string() + ":" + std::to_string( line ) + ": " + what );
        }
    } // namespace

    Result<PoolConfig> LoadConfig( const std::filesystem::path& home ) noexcept
    {
        Result<MemoryState> memory = QueryMemory();
        if ( !memory )
            return Fail( "pool.ini: the default reserve needs the machine's memory: " + memory.error().message );
        const uint64_t defaultReserve =
             std::max<uint64_t>( 2048 * daemon::kMiB, memory.value().physicalTotal / 10 );

        const std::filesystem::path file = home / "pool.ini";
        std::error_code             ec;
        const bool                  exists = std::filesystem::exists( file, ec );
        if ( ec )
            return Fail( file.string() + ": " + ec.message() );
        if ( !exists )
        {
            Status written = WriteFileAtomic( file, DefaultText( defaultReserve / daemon::kMiB ) );
            if ( !written )
                return Fail( file.string() + ": cannot write the defaults: " + written.error().message );
        }

        std::ifstream in( file, std::ios::binary );
        if ( !in )
            return Fail( file.string() + ": cannot be opened" );
        std::ostringstream content;
        content << in.rdbuf();
        const std::string text = content.str();

        PoolConfig config;
        config.reserveBytes                                    = defaultReserve;
        bool                                  haveDefaultClass = false;
        std::vector<std::string>              seenPoolKeys;
        std::vector<std::vector<std::string>> seenClassKeys;
        enum class Section
        {
            None,
            Pool,
            Class,
        } section = Section::None;

        size_t lineNumber = 0;
        size_t position   = 0;
        while ( position <= text.size() )
        {
            const size_t     newline = text.find( '\n', position );
            const size_t     end     = newline == std::string::npos ? text.size() : newline;
            std::string_view line( text.data() + position, end - position );
            position = end + 1;
            ++lineNumber;

            const size_t comment = line.find_first_of( "#;" );
            if ( comment != std::string_view::npos )
                line = line.substr( 0, comment );
            line = Trim( line );
            if ( line.empty() )
            {
                if ( newline == std::string::npos )
                    break;
                continue;
            }

            if ( line.front() == '[' )
            {
                if ( line.back() != ']' )
                    return LineError( file, lineNumber, "a section header must end with ']'" );
                const std::string_view name = Trim( line.substr( 1, line.size() - 2 ) );
                if ( name == "pool" )
                {
                    if ( std::find( seenPoolKeys.begin(), seenPoolKeys.end(), "[pool]" ) != seenPoolKeys.end() )
                        return LineError( file, lineNumber, "[pool] appears twice" );
                    seenPoolKeys.push_back( "[pool]" );
                    section = Section::Pool;
                }
                else if ( name.substr( 0, 6 ) == "class." )
                {
                    const std::string_view className = name.substr( 6 );
                    if ( !IsName( className ) )
                        return LineError( file, lineNumber,
                                          "a class name is letters, digits, '_' and '-': '" +
                                               std::string( className ) + "'" );
                    for ( const ClassPolicy& existing : config.classes )
                        if ( existing.name == className )
                            return LineError( file, lineNumber,
                                              "[class." + std::string( className ) + "] appears twice" );
                    ClassPolicy policy;
                    policy.name = std::string( className );
                    config.classes.push_back( std::move( policy ) );
                    seenClassKeys.emplace_back();
                    section = Section::Class;
                }
                else
                {
                    return LineError( file, lineNumber,
                                      "unknown section [" + std::string( name ) +
                                           "]; known: [pool], [class.<name>]" );
                }
                if ( newline == std::string::npos )
                    break;
                continue;
            }

            const size_t equals = line.find( '=' );
            if ( equals == std::string_view::npos )
                return LineError( file, lineNumber, "expected 'key = value'" );
            const std::string      key( Trim( line.substr( 0, equals ) ) );
            const std::string_view value = Trim( line.substr( equals + 1 ) );
            if ( value.empty() )
                return LineError( file, lineNumber, "'" + key + "' has no value" );
            const auto bad = [&]( const char* expected )
            {
                return LineError( file, lineNumber,
                                  "'" + key + "' must be " + expected + ", not '" + std::string( value ) + "'" );
            };

            if ( section == Section::None )
                return LineError( file, lineNumber, "'" + key + "' is outside any section" );

            if ( section == Section::Pool )
            {
                if ( std::find( seenPoolKeys.begin(), seenPoolKeys.end(), key ) != seenPoolKeys.end() )
                    return LineError( file, lineNumber, "'" + key + "' appears twice in [pool]" );
                seenPoolKeys.push_back( key );
                if ( key == "reserve_mb" )
                {
                    uint64_t mb = 0;
                    if ( !ParseInteger( value, mb ) )
                        return bad( "a whole number of MB" );
                    config.reserveBytes = mb * daemon::kMiB;
                }
                else if ( key == "cpu_reserve" )
                {
                    if ( !ParseInteger( value, config.cpuReserve ) )
                        return bad( "a whole number" );
                }
                else if ( key == "window_factor" )
                {
                    if ( !ParseInteger( value, config.windowFactor ) || config.windowFactor == 0 )
                        return bad( "a whole number >= 1" );
                }
                else if ( key == "estimate_margin" )
                {
                    if ( !ParseReal( value, config.estimateMargin ) || !( config.estimateMargin >= 1.0 ) )
                        return bad( "a number >= 1" );
                }
                else if ( key == "sample_ms" )
                {
                    if ( !ParseInteger( value, config.sampleMs ) || config.sampleMs == 0 )
                        return bad( "a whole number of ms >= 1" );
                }
                else if ( key == "default_class" )
                {
                    if ( !IsName( value ) )
                        return bad( "a class name" );
                    config.defaultClass = std::string( value );
                    haveDefaultClass    = true;
                }
                else
                {
                    return LineError( file, lineNumber,
                                      "unknown key '" + key +
                                           "' in [pool]; known: reserve_mb, cpu_reserve, window_factor, "
                                           "estimate_margin, sample_ms, default_class" );
                }
            }
            else
            {
                ClassPolicy&              policy = config.classes.back();
                std::vector<std::string>& seen   = seenClassKeys.back();
                if ( std::find( seen.begin(), seen.end(), key ) != seen.end() )
                    return LineError( file, lineNumber,
                                      "'" + key + "' appears twice in [class." + policy.name + "]" );
                seen.push_back( key );
                if ( key == "priority" )
                {
                    if ( !ParseInteger( value, policy.priority ) )
                        return bad( "a whole number" );
                }
                else if ( key == "guaranteed_slots" )
                {
                    if ( !ParseInteger( value, policy.guaranteedSlots ) )
                        return bad( "a whole number" );
                }
                else if ( key == "os_priority" )
                {
                    if ( !ParsePriority( value, policy.osPriority ) )
                        return bad( "normal, below_normal or idle" );
                }
                else
                {
                    return LineError( file, lineNumber,
                                      "unknown key '" + key + "' in [class." + policy.name +
                                           "]; known: priority, guaranteed_slots, os_priority" );
                }
            }
            if ( newline == std::string::npos )
                break;
        }

        if ( config.classes.empty() )
            return Fail( file.string() + ": no [class.<name>] section; at least one class is required" );
        if ( !haveDefaultClass )
            return Fail( file.string() +
                         ": [pool] default_class is missing; it names the class of a build that names none" );
        if ( daemon::FindClass( config, config.defaultClass ) == nullptr )
            return Fail( file.string() + ": default_class = " + config.defaultClass +
                         " names no [class.*] section" );
        const uint32_t cpus = LogicalCpuCount();
        if ( config.cpuReserve >= cpus )
            return Fail( file.string() + ": cpu_reserve = " + std::to_string( config.cpuReserve ) +
                         " leaves no slot on " + std::to_string( cpus ) + " logical CPUs" );
        return config;
    }

    namespace daemon
    {
        const ClassPolicy* FindClass( const PoolConfig& config, std::string_view name ) noexcept
        {
            const std::string_view wanted = name.empty() ? std::string_view( config.defaultClass ) : name;
            for ( const ClassPolicy& policy : config.classes )
                if ( policy.name == wanted )
                    return &policy;
            return nullptr;
        }

        std::vector<std::string> DescribeConfig( const PoolConfig& config )
        {
            std::vector<std::string> lines;
            std::ostringstream       margin;
            margin << config.estimateMargin;
            lines.push_back( "[pool] reserve_mb = " + std::to_string( config.reserveBytes / kMiB ) );
            lines.push_back( "[pool] cpu_reserve = " + std::to_string( config.cpuReserve ) );
            lines.push_back( "[pool] window_factor = " + std::to_string( config.windowFactor ) );
            lines.push_back( "[pool] estimate_margin = " + margin.str() );
            lines.push_back( "[pool] sample_ms = " + std::to_string( config.sampleMs ) );
            lines.push_back( "[pool] default_class = " + config.defaultClass );
            for ( const ClassPolicy& policy : config.classes )
                lines.push_back( "[class." + policy.name + "] priority = " + std::to_string( policy.priority ) +
                                 ", guaranteed_slots = " + std::to_string( policy.guaranteedSlots ) +
                                 ", os_priority = " + PriorityName( policy.osPriority ) );
            return lines;
        }
    } // namespace daemon
} // namespace buildpool
