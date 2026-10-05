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
     *    and the UI view by the same step (UIViewContext::FixedStep), so frame N is at N/Fps of both clocks
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
        bool any = false, haveMap = false, haveOut = false, haveRes = false, haveFps = false, haveDuration = false;

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
                char*        end     = nullptr;
                const double seconds = std::strtod( value.c_str(), &end );
                if ( end == value.c_str() || *end != '\0' || !( seconds > 0.0 ) || seconds > 3600.0 )
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
