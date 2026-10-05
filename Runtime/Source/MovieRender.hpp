#pragma once

#include <Common/Core/ResultStr.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Desert::Player
{
    /**
     * @brief `--render-movie <map.desce> --movie-out <dir> --resolution WxH --fps N --duration S` — the
     * engine renders a movie of a level itself (UE's Movie Render Queue), it does not film a window.
     *
     * WHAT MAKES IT A RENDER AND NOT A SCREEN RECORDING, three properties, each owned by one place:
     *  - THE TARGET is an offscreen framebuffer of exactly Width x Height (RuntimeLayer), so a 4K movie is
     *    4K on a laptop whose window is a postage stamp; the swapchain only gets an empty pass.
     *  - TIME is fixed: the application steps the world by 1/Fps per frame (Application::SetFixedDeltaTime)
     *    and the UI view is handed that same step (BeginUIFrame; its first frame spends none), so frame N is
     *    at N/Fps of the UI clock
     *    however long the GPU took — two runs give the same bytes.
     *  - FRAME 0 is the first frame after the content gate opened (ContentGate), so no frame shows a world
     *    or a font that is still loading, and the frames that follow are numbered without gaps or repeats.
     *
     * Every flag is REQUIRED once `--render-movie` is given: a movie rendered at a guessed resolution or
     * rate is a wrong movie that looks right, and nobody re-checks a file that came out fine.
     */
    struct MovieRenderRequest
    {
        std::string Map;    // the level rendered (a .desce, resolved like --scene)
        std::string OutDir; // receives 00000.png, 00001.png, ...
        uint32_t    Width    = 0;
        uint32_t    Height   = 0;
        uint32_t    Fps      = 0;
        double      Duration = 0.0; // seconds

        /// Frames in the movie: Duration * Fps, rounded to the nearest whole frame.
        [[nodiscard]] uint32_t FrameCount() const
        {
            return static_cast<uint32_t>( std::llround( Duration * static_cast<double>( Fps ) ) );
        }

        /// The one fixed step both clocks advance by.
        [[nodiscard]] float FrameStep() const
        {
            return 1.0f / static_cast<float>( Fps );
        }

        /// Where frame @p index goes: five digits, zero-padded, the pattern ffmpeg reads as `%05d.png`.
        [[nodiscard]] std::filesystem::path FramePath( uint32_t index ) const
        {
            return std::filesystem::path( OutDir ) / std::format( "{:05}.png", index );
        }
    };

    namespace MovieRenderDetail
    {
        [[nodiscard]] inline bool ParseUnsigned( const std::string& text, uint32_t& out )
        {
            if ( text.empty() || text.size() > 9 || text.find_first_not_of( "0123456789" ) != std::string::npos )
                return false;
            out = static_cast<uint32_t>( std::stoul( text ) );
            return true;
        }

        /**
         * @brief Seconds written as `DIGITS[.DIGITS]` (at most 15 digits in all), parsed explicitly — the way
         * UE's FParse reads a command-line number — rather than by std::strtod, whose decimal separator is the
         * process's LC_NUMERIC: under a comma locale strtod stops at the '.' of "3.0" and the request is
         * refused. (std::from_chars would be the library form, but Apple clang 15's libc++ has no floating
         * from_chars.) The value is DIGITS-as-integer / 10^fraction-digits: both are exact doubles, so the one
         * division is correctly rounded — the same double strtod gives in the C locale.
         */
        [[nodiscard]] inline bool ParseSeconds( const std::string& text, double& out )
        {
            const auto dot      = text.find( '.' );
            const auto intPart  = text.substr( 0, dot );
            const auto fracPart = dot == std::string::npos ? std::string{} : text.substr( dot + 1 );
            if ( intPart.empty() || ( dot != std::string::npos && fracPart.empty() ) ||
                 intPart.size() + fracPart.size() > 15 ||
                 ( intPart + fracPart ).find_first_not_of( "0123456789" ) != std::string::npos )
                return false;
            uint64_t mantissa = 0;
            for ( const char c : intPart + fracPart )
                mantissa = mantissa * 10 + static_cast<uint64_t>( c - '0' );
            double scale = 1.0;
            for ( size_t i = 0; i < fracPart.size(); ++i )
                scale *= 10.0;
            out = static_cast<double>( mantissa ) / scale;
            return true;
        }
    } // namespace MovieRenderDetail

    /**
     * @brief Pure parse of the movie flags out of @p args (argv[1..]). No `--render-movie` and no other movie
     * flag = no request (nullopt). Unknown tokens are ignored — other parsers own them (see ParseRuntimeShot).
     */
    [[nodiscard]] inline Common::ResultStr<std::optional<MovieRenderRequest>>
    ParseMovieRender( const std::vector<std::string>& args )
    {
        using Result = std::optional<MovieRenderRequest>;
        MovieRenderRequest request;
        bool any          = false;
        bool haveMap      = false;
        bool haveOut      = false;
        bool haveRes      = false;
        bool haveFps      = false;
        bool haveDuration = false;

        for ( size_t i = 0; i < args.size(); ++i )
        {
            const std::string& flag = args[i];
            if ( flag != "--render-movie" && flag != "--movie-out" && flag != "--resolution" && flag != "--fps" &&
                 flag != "--duration" )
                continue;
            any = true;
            if ( i + 1 >= args.size() || args[i + 1].starts_with( "--" ) )
                return Common::MakeFormattedError<Result>( "{} needs a value", flag );
            const std::string& value = args[++i];

            if ( flag == "--render-movie" )
            {
                request.Map = value;
                haveMap     = true;
            }
            else if ( flag == "--movie-out" )
            {
                request.OutDir = value;
                haveOut        = true;
            }
            else if ( flag == "--resolution" )
            {
                const auto x = value.find( 'x' );
                if ( x == std::string::npos ||
                     !MovieRenderDetail::ParseUnsigned( value.substr( 0, x ), request.Width ) ||
                     !MovieRenderDetail::ParseUnsigned( value.substr( x + 1 ), request.Height ) ||
                     request.Width == 0 || request.Height == 0 || request.Width > 16384 || request.Height > 16384 )
                    return Common::MakeFormattedError<Result>(
                         "--resolution '{}' is not WIDTHxHEIGHT in 1..16384, e.g. 3840x2160", value );
                haveRes = true;
            }
            else if ( flag == "--fps" )
            {
                if ( !MovieRenderDetail::ParseUnsigned( value, request.Fps ) || request.Fps == 0 ||
                     request.Fps > 1000 )
                    return Common::MakeFormattedError<Result>( "--fps '{}' is not a whole rate in 1..1000",
                                                               value );
                haveFps = true;
            }
            else // --duration
            {
                double seconds = 0.0;
                if ( !MovieRenderDetail::ParseSeconds( value, seconds ) || !( seconds > 0.0 ) || seconds > 3600.0 )
                    return Common::MakeFormattedError<Result>( "--duration '{}' is not seconds in (0, 3600]",
                                                               value );
                request.Duration = seconds;
                haveDuration     = true;
            }
        }

        if ( !any )
            return Common::MakeSuccess( Result{} );
        if ( !haveMap )
            return Common::MakeError<Result>( "movie flags given without --render-movie <map.desce>" );
        if ( !haveOut || !haveRes || !haveFps || !haveDuration )
            return Common::MakeFormattedError<Result>( "--render-movie needs every one of --movie-out, "
                                                       "--resolution, --fps, --duration; missing:{}{}{}{}",
                                                       haveOut ? "" : " --movie-out",
                                                       haveRes ? "" : " --resolution", haveFps ? "" : " --fps",
                                                       haveDuration ? "" : " --duration" );
        if ( request.FrameCount() == 0 )
            return Common::MakeFormattedError<Result>( "--duration {} at --fps {} is less than one frame",
                                                       request.Duration, request.Fps );
        return Common::MakeSuccess( Result{ std::move( request ) } );
    }
} // namespace Desert::Player
