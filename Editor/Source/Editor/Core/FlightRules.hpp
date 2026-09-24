#pragma once

// A HEADLESS FLIGHT: A CAMERA MOVED ALONG A ROUTE AT A FIXED SPEED, AND WHAT EACH FRAME COST ON THE WAY.
//
// WHY IT EXISTS. A partitioned world streams cells in and out as the camera moves (WP5b), and the question
// that decides its budget is not "how long does a cell take" (the streamer's end-of-Play log line answers
// that) but "which FRAME paid for it, and how much worse than its neighbours was that frame". Until this,
// a flight was driven by hand, and a hitch was a feeling. Here it is a row in a CSV with the cells that were
// activated on it.
//
// PURE, and deliberately so: no filesystem, no GPU, no Scene. The route, the pose along it, the frame
// count, the alignment of a frame's timing with the frame it belongs to, the percentiles and the CSV text
// are all decisions, and a decision this file makes is asserted by Desert/Tests/Editor/FlightRules without
// Vulkan. The editor (EditorLayer) supplies only what it alone has: the profiler's numbers and the
// streamer's report.
//
// UNITS. Positions and distances are centimetres (1 world unit = 1 cm), speed is centimetres per second of
// SIMULATED time: `--play` advances gameplay by a fixed 1/60 s a frame (ShotOptions::PlayStepSeconds), so
// a flight at 1000 cm/s covers 16.67 cm per frame no matter how slow the frame was, and two runs of the
// same flight visit the same positions on the same frame numbers. Frame times are WALL milliseconds.

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace Desert::Editor::Flight
{
    // Frames spent standing still at the route's start before it moves. Pipelines compile, the streamer's
    // start neighbourhood settles and the swapchain warms in these; they are written to the CSV (phase
    // "warmup") and left out of every statistic, because a p99 that measures shader compilation is not a
    // statement about streaming.
    inline constexpr int kWarmupFrames = 120;

    // A circle is flown as a closed polyline of this many chords. The largest distance between chord and
    // arc is r·(1 − cos(π/720)) = 9.5e-6·r — under 1 cm on a 1 km radius — so the route is the circle for
    // every purpose a cell grid of 128 m can notice, and one representation serves all three route kinds.
    inline constexpr int kCircleSegments = 720;

    struct Route
    {
        // At least two points, consecutive points distinct. A closed route returns to Points[0].
        std::vector<glm::vec3> Points;
        bool                   Closed = false;
        // `file:<path>`: the points are read by the caller (it has the disk) and handed to ParseRouteFile.
        std::string FilePath;
        // The text the route was given as, for the report.
        std::string Spec;
    };

    namespace Detail
    {
        inline bool ParseNumber( const std::string& text, double& out )
        {
            if ( text.empty() || std::isspace( static_cast<unsigned char>( text.front() ) ) )
                return false;
            char*        end   = nullptr;
            const double value = std::strtod( text.c_str(), &end );
            if ( end != text.c_str() + text.size() || !std::isfinite( value ) )
                return false;
            out = value;
            return true;
        }

        // Exactly @p count comma-separated numbers.
        inline bool ParseNumbers( const std::string& text, std::size_t count, std::vector<double>& out )
        {
            out.clear();
            std::size_t start = 0;
            while ( true )
            {
                const std::size_t comma = text.find( ',', start );
                double            value = 0.0;
                if ( !ParseNumber( text.substr( start, comma - start ), value ) )
                    return false;
                out.push_back( value );
                if ( comma == std::string::npos )
                    break;
                start = comma + 1;
            }
            return out.size() == count;
        }

        inline bool ParsePoint( const std::string& text, glm::vec3& out )
        {
            std::vector<double> xyz;
            if ( !ParseNumbers( text, 3, xyz ) )
                return false;
            out = glm::vec3( static_cast<float>( xyz[0] ), static_cast<float>( xyz[1] ),
                             static_cast<float>( xyz[2] ) );
            return true;
        }

        // A zero-length segment has no direction to look along and no length to divide by.
        inline Common::BoolResultStr CheckPoints( const Route& route )
        {
            if ( route.Points.size() < 2 )
                return Common::MakeError( "flight route '" + route.Spec + "' has " +
                                          std::to_string( route.Points.size() ) + " point(s); it needs two" );
            for ( std::size_t i = 1; i < route.Points.size(); ++i )
                if ( route.Points[i] == route.Points[i - 1] )
                    return Common::MakeError( "flight route '" + route.Spec + "': points " + std::to_string( i ) +
                                              " and " + std::to_string( i + 1 ) + " are the same place" );
            return Common::MakeSuccess( true );
        }
    } // namespace Detail

    // `line:x,y,z:x,y,z` — from the first point to the second.
    // `circle:x,y,z:r`   — one lap of radius r around (x,y,z), in the horizontal plane, starting at +X and
    //                      turning towards +Z.
    // `file:<path>`      — a polyline; FilePath is set and Points stay empty until ParseRouteFile.
    [[nodiscard]] inline Common::ResultStr<Route> ParseRouteSpec( const std::string& spec )
    {
        const auto refuse = [&spec]( const std::string& why )
        {
            return Common::MakeError<Route>( "--flight '" + spec + "': " + why +
                                             " (expected line:x,y,z:x,y,z, circle:x,y,z:radius or file:<path>)" );
        };
        Route             route;
        const std::size_t colon = spec.find( ':' );
        if ( colon == std::string::npos )
            return refuse( "no route kind" );
        route.Spec              = spec;
        const std::string kind  = spec.substr( 0, colon );
        const std::string rest  = spec.substr( colon + 1 );
        const std::size_t split = rest.find( ':' );
        if ( kind == "file" )
        {
            if ( rest.empty() )
                return refuse( "no file named" );
            route.FilePath = rest;
            return Common::MakeSuccess( std::move( route ) );
        }
        if ( kind != "line" && kind != "circle" )
            return refuse( "unknown route kind '" + kind + "'" );
        if ( split == std::string::npos || rest.find( ':', split + 1 ) != std::string::npos )
            return refuse( "a " + kind + " takes exactly two ':'-separated parts" );
        glm::vec3 first{};
        if ( !Detail::ParsePoint( rest.substr( 0, split ), first ) )
            return refuse( "'" + rest.substr( 0, split ) + "' is not a point" );
        if ( kind == "line" )
        {
            glm::vec3 second{};
            if ( !Detail::ParsePoint( rest.substr( split + 1 ), second ) )
                return refuse( "'" + rest.substr( split + 1 ) + "' is not a point" );
            route.Points = { first, second };
        }
        else
        {
            double radius = 0.0;
            if ( !Detail::ParseNumber( rest.substr( split + 1 ), radius ) || radius <= 0.0 )
                return refuse( "'" + rest.substr( split + 1 ) + "' is not a positive radius" );
            route.Closed = true;
            for ( int i = 0; i < kCircleSegments; ++i )
            {
                const double angle = 2.0 * 3.14159265358979323846 * static_cast<double>( i ) / kCircleSegments;
                route.Points.push_back( first + glm::vec3( static_cast<float>( radius * std::cos( angle ) ), 0.0f,
                                                           static_cast<float>( radius * std::sin( angle ) ) ) );
            }
        }
        if ( auto checked = Detail::CheckPoints( route ); !checked )
            return Common::MakeError<Route>( checked.GetError() );
        return Common::MakeSuccess( std::move( route ) );
    }

    // One point `x,y,z` per line; blank lines and lines starting with '#' are skipped. Open unless the last
    // point repeats the first, which is how a file says "and back to the start" without a separate flag.
    [[nodiscard]] inline Common::ResultStr<Route> ParseRouteFile( Route route, const std::string& text )
    {
        route.Points.clear();
        std::size_t start = 0;
        int         line  = 0;
        while ( start <= text.size() )
        {
            std::size_t end = text.find( '\n', start );
            if ( end == std::string::npos )
                end = text.size();
            std::string row = text.substr( start, end - start );
            ++line;
            start = end + 1;
            if ( !row.empty() && row.back() == '\r' )
                row.pop_back();
            if ( row.empty() || row.front() == '#' )
                continue;
            glm::vec3 point{};
            if ( !Detail::ParsePoint( row, point ) )
                return Common::MakeError<Route>( "flight route file '" + route.FilePath + "', line " +
                                                 std::to_string( line ) + ": '" + row + "' is not x,y,z" );
            route.Points.push_back( point );
        }
        if ( route.Points.size() > 2 && route.Points.back() == route.Points.front() )
        {
            route.Points.pop_back();
            route.Closed = true;
        }
        if ( auto checked = Detail::CheckPoints( route ); !checked )
            return Common::MakeError<Route>( checked.GetError() );
        return Common::MakeSuccess( std::move( route ) );
    }

    [[nodiscard]] inline std::size_t SegmentCount( const Route& route )
    {
        return route.Closed ? route.Points.size() : route.Points.size() - 1;
    }

    [[nodiscard]] inline double RouteLength( const Route& route )
    {
        double length = 0.0;
        for ( std::size_t s = 0; s < SegmentCount( route ); ++s )
            length += glm::length( glm::dvec3( route.Points[( s + 1 ) % route.Points.size()] ) -
                                   glm::dvec3( route.Points[s] ) );
        return length;
    }

    struct Pose
    {
        glm::vec3 Position{ 0.0f };
        glm::vec3 Forward{ 0.0f, 0.0f, -1.0f }; // unit, along the segment being flown
    };

    // The pose @p distance centimetres along the route, clamped to its ends: before the start is the start
    // (the warm-up stands there), past the end is the end.
    [[nodiscard]] inline Pose PoseAt( const Route& route, double distance )
    {
        const std::size_t segments = SegmentCount( route );
        double            left     = std::max( distance, 0.0 );
        for ( std::size_t s = 0; s < segments; ++s )
        {
            const glm::dvec3 a( route.Points[s] );
            const glm::dvec3 b( route.Points[( s + 1 ) % route.Points.size()] );
            const double     length = glm::length( b - a );
            if ( left <= length || s + 1 == segments )
            {
                const double t = std::min( left / length, 1.0 );
                return Pose{ glm::vec3( a + ( b - a ) * t ), glm::vec3( glm::normalize( b - a ) ) };
            }
            left -= length;
        }
        return Pose{ route.Points.front(), glm::vec3( 0.0f, 0.0f, -1.0f ) };
    }

    // How far the flight has gone on counted frame @p frame (0-based): nothing during the warm-up, then
    // one simulated step per frame.
    [[nodiscard]] inline double DistanceAt( int frame, double speedCmPerSecond, double stepSeconds )
    {
        return static_cast<double>( std::max( frame - kWarmupFrames, 0 ) ) * stepSeconds * speedCmPerSecond;
    }

    // Counted frames a flight takes: the warm-up, every frame until the end of the route is reached (the
    // last of them lands on it), and one more — a frame's time is published only at the start of the NEXT
    // frame (FlightLog), so the frame that ends the process can never be timed and must not be one that
    // carries the route's end.
    [[nodiscard]] inline int FlightFrames( double lengthCm, double speedCmPerSecond, double stepSeconds )
    {
        const double moving = std::ceil( lengthCm / ( speedCmPerSecond * stepSeconds ) );
        return kWarmupFrames + static_cast<int>( moving ) + 1 + 1;
    }

    enum class Phase
    {
        Warmup,   // standing at the start; not in the statistics
        Flight,   // a counted frame on the route
        Settling, // a frame the capture did not count (content still arriving); the camera held still on it
    };

    [[nodiscard]] inline const char* PhaseName( Phase phase )
    {
        switch ( phase )
        {
            case Phase::Warmup:
                return "warmup";
            case Phase::Flight:
                return "flight";
            case Phase::Settling:
                return "settling";
        }
        return "?";
    }

    inline constexpr double kNotMeasured = std::numeric_limits<double>::quiet_NaN();

    struct FrameRow
    {
        int       Frame    = 0; // the capture's counted-frame index the camera was placed from
        Phase     Kind     = Phase::Flight;
        double    Distance = 0.0; // cm along the route
        glm::vec3 Position{ 0.0f };

        // Filled one frame LATER (FlightLog::TimeLast): NaN until then.
        double CpuMs    = kNotMeasured; // wall time of the whole frame, BeginFrame to BeginFrame
        double GpuMs    = kNotMeasured; // the GPU frame bracket resolved during this frame; NaN without GPU timing
        double StreamMs = kNotMeasured; // the "WorldStreamer::Tick" scope: residency step plus every activation

        std::size_t Entities = 0; // entities in the ECS after this frame's streaming
        std::size_t ResidentRecords =
             0; // records the streamer holds as entities; 0 for a world that does not stream
        std::size_t UnitsActivated   = 0;
        std::size_t UnitsDeactivated = 0;
        std::size_t RecordsActivated = 0;
        std::size_t RecordsDestroyed = 0;
        double      ActivationMs     = 0.0; // the streamer's own timing of this frame's activations
        std::string ActivatedUnits;         // which cells, e.g. "L1(3,-2) L1(4,-2)"
    };

    // THE ALIGNMENT, which is the whole difficulty of a per-frame timing: the profiler publishes a frame's
    // numbers at the START of the next one (Profiler::BeginFrame with a zero averaging window), so the
    // numbers readable while frame k is being made are frame k−1's. A row is therefore appended with
    // everything known during its own frame and TIMED on the next, and a row the run ended before timing
    // stays NaN and is left out of the statistics rather than given a neighbour's number.
    class FlightLog
    {
    public:
        // Called once at the top of every frame's report, before Append: gives the previous frame's numbers
        // to the row appended on the previous frame. A frame with no row before it (the first) times nothing.
        void TimeLast( double cpuMs, double gpuMs, double streamMs )
        {
            if ( m_Rows.empty() || !std::isnan( m_Rows.back().CpuMs ) )
                return;
            m_Rows.back().CpuMs    = cpuMs;
            m_Rows.back().GpuMs    = gpuMs;
            m_Rows.back().StreamMs = streamMs;
        }

        void Append( FrameRow row )
        {
            m_Rows.push_back( std::move( row ) );
        }

        [[nodiscard]] std::span<const FrameRow> Rows() const
        {
            return m_Rows;
        }

    private:
        std::vector<FrameRow> m_Rows;
    };

    // NEAREST RANK: the smallest value with at least p % of the sample at or below it. No interpolation, so
    // every reported percentile is a frame that happened — a p99 of 23.4 ms can be looked up in the CSV.
    // @p sorted ascending and non-empty.
    [[nodiscard]] inline double Percentile( std::span<const double> sorted, double percent )
    {
        const double      rank  = std::ceil( percent / 100.0 * static_cast<double>( sorted.size() ) );
        const std::size_t index = static_cast<std::size_t>( std::clamp( rank, 1.0, double( sorted.size() ) ) ) - 1;
        return sorted[index];
    }

    struct Summary
    {
        std::size_t Frames         = 0; // timed frames in the statistics (flight + settling)
        std::size_t SettlingFrames = 0;
        std::size_t UntimedFrames  = 0; // rows the run ended before timing, left out
        double      P50            = 0.0;
        double      P95            = 0.0;
        double      P99            = 0.0;
        double      Max            = 0.0;
        double      Mean           = 0.0;
        std::size_t Worst          = 0; // index into the rows of the frame that took Max

        // GPU, when it was measured on every timed frame; otherwise HasGpu is false and these are zero.
        bool   HasGpu = false;
        double GpuP50 = 0.0;
        double GpuP95 = 0.0;
        double GpuMax = 0.0;

        // Streaming, over the same frames.
        std::size_t FramesWithActivation = 0;
        std::size_t UnitsActivated       = 0;
        std::size_t UnitsDeactivated     = 0;
        std::size_t MaxEntities          = 0;
        std::size_t MaxResidentRecords   = 0;
        double      MaxActivationMs      = 0.0;
        std::size_t MaxActivationFrame   = 0; // index into the rows
        // Median frame with an activation against median frame without one: the cost a player feels.
        double P50WithActivation    = 0.0;
        double P50WithoutActivation = 0.0;
    };

    // Refused when no row is both past the warm-up and timed: a summary of nothing would print zeros that
    // read as a very fast flight.
    [[nodiscard]] inline Common::ResultStr<Summary> Summarise( std::span<const FrameRow> rows )
    {
        Summary             summary;
        std::vector<double> cpu;
        std::vector<double> gpu;
        std::vector<double> withActivation;
        std::vector<double> withoutActivation;
        bool                gpuEverywhere = true;
        double              total         = 0.0;
        for ( std::size_t i = 0; i < rows.size(); ++i )
        {
            const FrameRow& row = rows[i];
            if ( row.Kind == Phase::Warmup )
                continue;
            if ( std::isnan( row.CpuMs ) )
            {
                ++summary.UntimedFrames;
                continue;
            }
            if ( cpu.empty() || row.CpuMs > summary.Max )
            {
                summary.Max   = row.CpuMs;
                summary.Worst = i;
            }
            cpu.push_back( row.CpuMs );
            total += row.CpuMs;
            if ( std::isnan( row.GpuMs ) )
                gpuEverywhere = false;
            else
                gpu.push_back( row.GpuMs );
            summary.SettlingFrames += row.Kind == Phase::Settling ? 1 : 0;
            summary.UnitsActivated += row.UnitsActivated;
            summary.UnitsDeactivated += row.UnitsDeactivated;
            summary.MaxEntities        = std::max( summary.MaxEntities, row.Entities );
            summary.MaxResidentRecords = std::max( summary.MaxResidentRecords, row.ResidentRecords );
            if ( row.UnitsActivated > 0 )
            {
                ++summary.FramesWithActivation;
                withActivation.push_back( row.CpuMs );
                if ( row.ActivationMs > summary.MaxActivationMs || summary.FramesWithActivation == 1 )
                {
                    summary.MaxActivationMs    = row.ActivationMs;
                    summary.MaxActivationFrame = i;
                }
            }
            else
            {
                withoutActivation.push_back( row.CpuMs );
            }
        }
        if ( cpu.empty() )
            return Common::MakeError<Summary>( "flight summary: none of the " + std::to_string( rows.size() ) +
                                               " row(s) is a timed frame past the warm-up" );
        std::sort( cpu.begin(), cpu.end() );
        summary.Frames = cpu.size();
        summary.P50    = Percentile( cpu, 50.0 );
        summary.P95    = Percentile( cpu, 95.0 );
        summary.P99    = Percentile( cpu, 99.0 );
        summary.Mean   = total / static_cast<double>( cpu.size() );
        if ( gpuEverywhere )
        {
            std::sort( gpu.begin(), gpu.end() );
            summary.HasGpu = true;
            summary.GpuP50 = Percentile( gpu, 50.0 );
            summary.GpuP95 = Percentile( gpu, 95.0 );
            summary.GpuMax = gpu.back();
        }
        std::sort( withActivation.begin(), withActivation.end() );
        std::sort( withoutActivation.begin(), withoutActivation.end() );
        if ( !withActivation.empty() )
            summary.P50WithActivation = Percentile( withActivation, 50.0 );
        if ( !withoutActivation.empty() )
            summary.P50WithoutActivation = Percentile( withoutActivation, 50.0 );
        return Common::MakeSuccess( summary );
    }

    namespace Detail
    {
        inline std::string Number( double value, int decimals )
        {
            if ( std::isnan( value ) )
                return "";
            char text[64];
            std::snprintf( text, sizeof( text ), "%.*f", decimals, value );
            return text;
        }
    } // namespace Detail

    [[nodiscard]] inline std::string CsvHeader()
    {
        return "frame,phase,distance_cm,x,y,z,cpu_ms,gpu_ms,stream_ms,entities,resident_records,units_activated,"
               "units_deactivated,records_activated,records_destroyed,activation_ms,activated_units\n";
    }

    // A value that is not measured is an EMPTY field, not 0: a zero GPU time would be a very fast frame. The
    // unit names are QUOTED, because a cell's name carries a comma ("L1(3,-2)").
    [[nodiscard]] inline std::string CsvRow( const FrameRow& row )
    {
        using Detail::Number;
        return std::to_string( row.Frame ) + "," + PhaseName( row.Kind ) + "," + Number( row.Distance, 1 ) + "," +
               Number( row.Position.x, 1 ) + "," + Number( row.Position.y, 1 ) + "," +
               Number( row.Position.z, 1 ) + "," + Number( row.CpuMs, 3 ) + "," + Number( row.GpuMs, 3 ) + "," +
               Number( row.StreamMs, 3 ) + "," + std::to_string( row.Entities ) + "," +
               std::to_string( row.ResidentRecords ) + "," + std::to_string( row.UnitsActivated ) + "," +
               std::to_string( row.UnitsDeactivated ) + "," + std::to_string( row.RecordsActivated ) + "," +
               std::to_string( row.RecordsDestroyed ) + "," + Number( row.ActivationMs, 3 ) + ",\"" +
               row.ActivatedUnits + "\"\n";
    }

    [[nodiscard]] inline std::string Csv( std::span<const FrameRow> rows )
    {
        std::string text = CsvHeader();
        for ( const FrameRow& row : rows )
            text += CsvRow( row );
        return text;
    }

    // The log's account of a flight: the distribution, and the worst frame WITH its cause, because a
    // maximum without the frame's contents is a number nobody can act on.
    [[nodiscard]] inline std::string Describe( const Summary& s, std::span<const FrameRow> rows )
    {
        using Detail::Number;
        const FrameRow& worst = rows[s.Worst];
        std::string text = std::to_string( s.Frames ) + " timed frame(s) (" + std::to_string( s.SettlingFrames ) +
                           " settling, " + std::to_string( s.UntimedFrames ) +
                           " untimed left out): CPU frame p50 " + Number( s.P50, 2 ) + " / p95 " +
                           Number( s.P95, 2 ) + " / p99 " + Number( s.P99, 2 ) + " / max " + Number( s.Max, 2 ) +
                           " ms, mean " + Number( s.Mean, 2 ) + " ms";
        if ( s.HasGpu )
            text += "; GPU p50 " + Number( s.GpuP50, 2 ) + " / p95 " + Number( s.GpuP95, 2 ) + " / max " +
                    Number( s.GpuMax, 2 ) + " ms";
        text += ". Worst frame " + std::to_string( worst.Frame ) + " (" + PhaseName( worst.Kind ) + ", " +
                Number( worst.Distance / 100.0, 1 ) + " m along): ";
        text += worst.UnitsActivated > 0
                     ? std::to_string( worst.UnitsActivated ) + " unit(s) activated [" + worst.ActivatedUnits +
                            "], " + std::to_string( worst.RecordsActivated ) + " record(s) in " +
                            Number( worst.ActivationMs, 2 ) + " ms"
                     : std::string( "no activation" );
        text += ", streamer tick " + Number( worst.StreamMs, 2 ) +
                " ms. Streaming: " + std::to_string( s.UnitsActivated ) + " activation(s) and " +
                std::to_string( s.UnitsDeactivated ) + " deactivation(s) on " +
                std::to_string( s.FramesWithActivation ) + " frame(s); median frame with an activation " +
                Number( s.P50WithActivation, 2 ) + " ms, without " + Number( s.P50WithoutActivation, 2 ) + " ms";
        if ( s.FramesWithActivation > 0 )
        {
            const FrameRow& costly = rows[s.MaxActivationFrame];
            text += "; costliest activation frame " + std::to_string( costly.Frame ) + ": [" +
                    costly.ActivatedUnits + "] " + Number( costly.ActivationMs, 2 ) + " ms in a " +
                    Number( costly.CpuMs, 2 ) + " ms frame";
        }
        text += ". Peak " + std::to_string( s.MaxEntities ) + " entities, " +
                std::to_string( s.MaxResidentRecords ) + " streamed record(s) resident.";
        return text;
    }
} // namespace Desert::Editor::Flight
