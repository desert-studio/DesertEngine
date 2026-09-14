#pragma once

#include <Common/Core/ResultStr.hpp>

#include <Editor/Core/ShotOptions.hpp>

#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Editor
{
    /**
     * @brief The editor's command line, resolved to values or to a NAMED failure. Pure: no files, no
     *        globals, no GPU — argv in, options out, which is what makes it assertable by a test.
     *
     * WHY THIS IS A FUNCTION AND NOT A LOOP IN main(). It used to be the loop, and the loop had the same
     * defect three times over: an argument it did not recognise was DROPPED IN SILENCE. `--sceen X` (one
     * transposed letter), `--shot-frames` written last with its value forgotten, `--camera 0,200` with a
     * component missing — each of those parsed to "nothing happened", and "nothing happened" for `--scene`
     * means the editor loads the PROJECT'S DEFAULT scene, renders it, writes a plausible PNG under the name
     * of the scene that was asked for, and exits 0.
     *
     * That is the silent fallback the delivery contract forbids (DC 1.4), and it is worse than a crash: a
     * capture that fails loudly costs a re-run, while a capture that succeeds with the wrong subject enters
     * a document as evidence. One row of measurements was already lost to it, and three developers spent a
     * programme cross-checking every frame against its own log by hand to be sure of what had rendered.
     *
     * So the rule here is total: EVERY token on the command line is either recognised and consumed, or the
     * run stops with a message naming the token. There is no third outcome, and in particular there is no
     * "ignore it and carry on".
     *
     * @note The engine reads argv nowhere else — `CreateApplication` is its only consumer — so this
     *       function may reject what it does not know without stepping on a flag somebody else handles.
     *
     * WHERE THE BOUNDARY OF THIS FILE NOW RUNS, because four flags left it and one arrived.
     *
     * The flags that remain all do the same kind of thing: they put the editor in a KNOWN STATE AT BOOT
     * and then have nothing further to say — `--project` and `--scene` decide what is loaded, and the
     * `--shot*` / `--camera*` / `--look*` / `--play` family decides what is captured and from where. Those
     * are for runs with nobody watching AND nobody connected: the editor renders, writes a PNG, and exits
     * with a status a script can trust. Nothing about them wants a session.
     *
     * `--select`, `--open-panel`, `--open-menu` and `--preview-orbit` were a different animal wearing the
     * same coat. Each was added because macOS refuses this machine synthetic input, so a panel, a menu, a
     * selection or a preview angle could not be reached by a click — and each could only be spent ONCE, at
     * boot, because that is all a flag can do. They are gone, and what replaces them is a session: the
     * control channel (`--control-socket`), which runs the command palette's own entries at any moment,
     * as many times as asked. Two ways to open a panel would have been the defect this project spends its
     * days removing; the flag family is the side that lost, because it was the side that could not grow.
     */

    /// One accepted flag. The table exists so the error message that lists the known flags cannot drift
    /// from the set the parser actually accepts; a test walks it and asserts every entry parses.
    struct CommandLineFlag
    {
        const char* Name;
        bool        TakesValue;
        /// A value that parses, for the table-driven test. Null for a flag that takes none.
        const char* ExampleValue;

        /// Flags this one cannot be given without, spelled as the rest of a command line (`"--ui-pointer
        /// 640,360"`). Null for the ordinary case of a flag that stands alone.
        ///
        /// WHY THE TABLE CARRIES IT. `--ui-press` without `--ui-pointer` is refused, because a button held
        /// at no position presses the top-left corner of the frame and produces a picture that looks like
        /// evidence. The table-driven test parses each flag ON ITS OWN, so an undeclared dependency makes
        /// that test red for the flag doing the right thing — and the repair anybody reaches for first is
        /// to drop the refusal. Declaring it keeps both: the dependency is data the test can honour, and it
        /// is written beside the flag rather than inside a test nobody reads when adding one.
        const char* AlsoNeeds = nullptr;
    };

    inline constexpr CommandLineFlag kCommandLineFlags[] = {
         { "--project", true, "Desert.deproj" },
         { "--scene", true, "Scene.desce" },
         { "--shot", true, "out.png" },
         { "--shot-frames", true, "90" },
         { "--camera", true, "0,200,0" },
         { "--look", true, "0,0.9,-1" },
         { "--camera-to", true, "0,200,-100" },
         { "--look-to", true, "0,0.5,-1" },
         { "--shot-sequence", true, "/tmp/seq" },
         { "--shot-every", true, "1" },
         { "--control-socket", true, "/tmp/desert-editor.sock" },
         { "--gpu-profile", false, nullptr },
         { "--no-gpu-timing", false, nullptr },
         { "--gpu-profile-frame-only", false, nullptr },
         { "--play", false, nullptr },
         { "--ui-pointer", true, "640,360" },
         { "--ui-press", true, "right", "--ui-pointer 640,360" },
    };

    /// Everything the command line resolved to. Held by value and copied into the process-wide singletons
    /// by the caller, so that the parsing itself stays a pure function of its input.
    struct CommandLineOptions
    {
        /// `--project <path.deproj>`. Whether the path OPENS is the caller's business — this function never
        /// touches the disk, which is exactly what lets it be tested without one.
        std::string Project;
        ShotOptions Shot;

        /// `--control-socket <path>`: listen for the control channel there. Empty — the default — means
        /// the editor listens for NOTHING, which is the only safe default for a socket that can run every
        /// command the palette offers, including saving over the user's scene.
        ///
        /// The path is named rather than fixed so two editors on one machine each get their own; a shared
        /// one would have them answering each other's clients, and the client could not tell.
        std::string ControlSocket;
    };

    namespace CommandLineDetail
    {
        /// A float that consumes its ENTIRE text. `strtof` alone stops at the first character it cannot
        /// use and reports success for what it read, so "0.5abc" and "0,200" both look like wins; the
        /// end-pointer check is what turns a partial read into the error it is.
        inline bool ParseFloatStrict( const std::string& text, float& out )
        {
            if ( text.empty() )
                return false;

            errno             = 0;
            char*       end   = nullptr;
            const float value = std::strtof( text.c_str(), &end );

            if ( end != text.c_str() + text.size() )
                return false;
            if ( errno == ERANGE )
                return false;
            // "inf"/"nan" parse cleanly and are not camera coordinates. A NaN position produces a frame
            // that is uniformly one colour, which reads as a broken renderer rather than a bad argument.
            if ( !std::isfinite( value ) )
                return false;

            out = value;
            return true;
        }

        /// Exactly TWO comma-separated floats — a point on the screen. Same strictness as the vec3 below
        /// and for the same reason: `--ui-pointer 640,360,0` is somebody who meant something else.
        inline bool ParseVec2Strict( const std::string& text, glm::vec2& out )
        {
            const std::size_t comma = text.find( ',' );
            if ( comma == std::string::npos )
                return false;
            if ( text.find( ',', comma + 1 ) != std::string::npos )
                return false;

            float x = 0.0f;
            float y = 0.0f;
            if ( !ParseFloatStrict( text.substr( 0, comma ), x ) )
                return false;
            if ( !ParseFloatStrict( text.substr( comma + 1 ), y ) )
                return false;

            out = glm::vec2( x, y );
            return true;
        }

        /// Exactly three comma-separated floats. Not "at least three": `--camera 0,200,0,7` is a person
        /// who meant something the flag cannot express, and guessing which three they meant is the silent
        /// fallback in miniature.
        inline bool ParseVec3Strict( const std::string& text, glm::vec3& out )
        {
            const std::size_t first = text.find( ',' );
            if ( first == std::string::npos )
                return false;
            const std::size_t second = text.find( ',', first + 1 );
            if ( second == std::string::npos )
                return false;
            if ( text.find( ',', second + 1 ) != std::string::npos )
                return false;

            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            if ( !ParseFloatStrict( text.substr( 0, first ), x ) )
                return false;
            if ( !ParseFloatStrict( text.substr( first + 1, second - first - 1 ), y ) )
                return false;
            if ( !ParseFloatStrict( text.substr( second + 1 ), z ) )
                return false;

            out = glm::vec3( x, y, z );
            return true;
        }

        /// An integer that consumes its entire text. `atoi` — which this replaces — answers 0 for "abc"
        /// and for "90x" alike, so a mistyped frame count silently became "capture immediately".
        inline bool ParseIntStrict( const std::string& text, int& out )
        {
            const char* begin = text.data();
            const char* end   = text.data() + text.size();

            int        value  = 0;
            const auto result = std::from_chars( begin, end, value );
            if ( result.ec != std::errc() || result.ptr != end )
                return false;

            out = value;
            return true;
        }

        /// The known flags, comma separated — for the message a rejected token gets. Built from the table
        /// rather than written out, so a flag added above appears here without anyone remembering to.
        inline std::string KnownFlagList()
        {
            std::string list;
            for ( const CommandLineFlag& flag : kCommandLineFlags )
            {
                if ( !list.empty() )
                    list += ", ";
                list += flag.Name;
            }
            return list;
        }
    } // namespace CommandLineDetail

    /**
     * @brief Resolve @p args (argv WITHOUT the program name) into options, or fail naming the token.
     *
     * Failure modes, all of which used to be silent no-ops:
     *   - a token that is not a known flag;
     *   - a flag whose value is missing because the flag was written last;
     *   - a vector or an integer that does not parse, or parses only in part.
     */
    inline Common::ResultStr<CommandLineOptions> ParseCommandLine( const std::vector<std::string>& args )
    {
        using namespace CommandLineDetail;

        CommandLineOptions options;

        for ( std::size_t i = 0; i < args.size(); ++i )
        {
            const std::string& arg = args[i];

            // Find the flag in the table first. This is what makes an unknown token an ERROR rather than
            // something that falls off the end of an if/else chain, which is how the old loop lost them.
            const CommandLineFlag* flag = nullptr;
            for ( const CommandLineFlag& candidate : kCommandLineFlags )
            {
                if ( arg == candidate.Name )
                {
                    flag = &candidate;
                    break;
                }
            }

            if ( flag == nullptr )
            {
                return Common::MakeFormattedError<CommandLineOptions>(
                     "unrecognised argument '{}'. A token this parser does not know used to be dropped in "
                     "silence, which for a mistyped '--scene' meant capturing the DEFAULT scene under the "
                     "name of the one that was asked for. Known flags: {}",
                     arg, KnownFlagList() );
            }

            if ( !flag->TakesValue )
            {
                if ( arg == "--gpu-profile" )
                    options.Shot.GpuProfile = true;
                else if ( arg == "--no-gpu-timing" )
                    options.Shot.GpuTiming = false;
                else if ( arg == "--gpu-profile-frame-only" )
                    options.Shot.GpuFrameOnly = true;
                else if ( arg == "--play" )
                    options.Shot.Play = true;
                continue;
            }

            if ( i + 1 >= args.size() )
            {
                return Common::MakeFormattedError<CommandLineOptions>(
                     "'{}' needs a value and is the last token on the command line. Written this way it "
                     "used to be dropped entirely, leaving the setting at its default with nothing said.",
                     arg );
            }

            const std::string& value = args[++i];

            if ( arg == "--project" )
                options.Project = value;
            else if ( arg == "--scene" )
                options.Shot.Scene = value;
            else if ( arg == "--shot" )
                options.Shot.Output = value;
            else if ( arg == "--shot-sequence" )
                options.Shot.Sequence = value;
            else if ( arg == "--control-socket" )
                options.ControlSocket = value;
            else if ( arg == "--shot-frames" )
            {
                int frames = 0;
                if ( !ParseIntStrict( value, frames ) || frames < 1 )
                {
                    return Common::MakeFormattedError<CommandLineOptions>(
                         "--shot-frames '{}' is not a frame count (a whole number, at least 1).", value );
                }
                options.Shot.Frames = frames;
            }
            else if ( arg == "--shot-every" )
            {
                int every = 0;
                if ( !ParseIntStrict( value, every ) || every < 1 )
                {
                    return Common::MakeFormattedError<CommandLineOptions>(
                         "--shot-every '{}' is not an interval (a whole number, at least 1).", value );
                }
                options.Shot.SequenceEvery = every;
            }
            else if ( arg == "--ui-pointer" )
            {
                if ( !ParseVec2Strict( value, options.Shot.UIPointer ) )
                {
                    return Common::MakeFormattedError<CommandLineOptions>(
                         "--ui-pointer '{}' is not a point (two comma-separated numbers in framebuffer "
                         "pixels, e.g. 640,360).",
                         value );
                }
                options.Shot.HasUIPointer = true;
            }
            else if ( arg == "--ui-press" )
            {
                if ( value == "left" )
                    options.Shot.UIPress = ShotOptions::UIButtonHeld::Left;
                else if ( value == "right" )
                    options.Shot.UIPress = ShotOptions::UIButtonHeld::Right;
                else
                {
                    return Common::MakeFormattedError<CommandLineOptions>(
                         "--ui-press '{}' is not a button ('left' or 'right').", value );
                }
            }
            else if ( arg == "--camera" )
            {
                if ( !ParseVec3Strict( value, options.Shot.Position ) )
                {
                    return Common::MakeFormattedError<CommandLineOptions>(
                         "--camera '{}' is not a position (three comma-separated numbers, e.g. 0,200,0).", value );
                }
                options.Shot.HasCamera = true;
            }
            else if ( arg == "--look" )
            {
                if ( !ParseVec3Strict( value, options.Shot.Forward ) )
                {
                    return Common::MakeFormattedError<CommandLineOptions>(
                         "--look '{}' is not a direction (three comma-separated numbers, e.g. 0,0.9,-1).", value );
                }
                options.Shot.HasCamera = true;
            }
            // The far end of a moving shot. Each also implies --camera, because a path that nothing places
            // is a path the scene's own camera ignores.
            else if ( arg == "--camera-to" )
            {
                if ( !ParseVec3Strict( value, options.Shot.PositionTo ) )
                {
                    return Common::MakeFormattedError<CommandLineOptions>(
                         "--camera-to '{}' is not a position (three comma-separated numbers).", value );
                }
                options.Shot.HasPositionTo = true;
                options.Shot.HasCamera     = true;
            }
            else if ( arg == "--look-to" )
            {
                if ( !ParseVec3Strict( value, options.Shot.ForwardTo ) )
                {
                    return Common::MakeFormattedError<CommandLineOptions>(
                         "--look-to '{}' is not a direction (three comma-separated numbers).", value );
                }
                options.Shot.HasForwardTo = true;
                options.Shot.HasCamera    = true;
            }
        }

        // A held button with nowhere to hold it is a caller error and not a default. Without a pointer the
        // button would be pressed at 0,0 and every such capture would quietly click the top-left corner of
        // the frame — a silent wrong answer with a picture attached, which is the worst kind.
        if ( options.Shot.UIPress != ShotOptions::UIButtonHeld::None && !options.Shot.HasUIPointer )
        {
            return Common::MakeError<CommandLineOptions>(
                 "--ui-press needs --ui-pointer: a button held at no position would press the top-left "
                 "corner of the frame." );
        }

        return Common::MakeSuccess( std::move( options ) );
    }

    /**
     * @brief IS NOBODY SITTING AT THIS EDITOR? — asked once, here, and answered for every consequence.
     *
     * THREE PLACES USED TO ASK `shot.Active()` AND MEAN THIS INSTEAD, and their own comments say so: the
     * recent-projects registry ("those runs happen in agent worktrees that are reclaimed within the hour,
     * and each one used to file itself at the top of the developer's list"), the engine-install registry
     * ("reclaiming it a day later would leave the launcher offering to start something that is gone") and
     * the project tile written on exit ("the picture would be of a scene nobody chose, written into a
     * project nobody will open"). Not one of those sentences is about capture. Every one of them is about
     * a session a MACHINE drove.
     *
     * WHICH MEANT THE GUARD MISSED THE DOOR THAT REPLACED THE ONE IT WATCHES. The control channel exists
     * so that an unattended run does not need `--shot` at all — it takes its pictures over the socket —
     * and such a run therefore filed the throwaway worktree in the developer's recent-projects list,
     * registered it as an installed engine, and overwrote the project's tile with a picture of whatever
     * happened to be on screen. The three lines were written for exactly that and could not see it.
     *
     * A6-1 point 5: a capture flag must not be the switch for things that are not capture. Named once,
     * taken as VALUES so a suite can drive it, and deliberately NOT a fourth thing to remember on the
     * command line — a session is unattended because of how it was started, not because it said so.
     *
     * ── THE ONE COST THIS ACCEPTS, AND WHY IT IS NOT A BUG TO BE FIXED BACK ──────────────────────────
     *
     * A person driving their OWN project through the MCP client now gets no refreshed project tile and
     * no entry in their recent-projects list, because their session is a channel session. That was
     * raised as a trade-off and DECIDED (owner, A6-2 brief): leave it.
     *
     * The argument is not that the cost is zero. It is that a channel session is a TOOL driving the
     * editor, and a tool does not write into a developer's personal registries — the same rule that
     * keeps anything from writing `~/.desertengine/editor.json` on somebody's behalf. And the two
     * failures are not symmetric: a tile that did not refresh is a LOUD, obvious loss the person can
     * see and correct; registries silted up with hour-old agent worktrees is a QUIET one, and the
     * launcher already carries an "unopenable entry" state because that is what happened.
     *
     * So if the human case ever hurts enough to matter, it is cured by an EXPLICIT flag of consent —
     * "yes, file this session in my history" — and not by widening this predicate back to `shot.Active()`
     * or by dropping the channel from it. Anybody about to do the latter is undoing a decision, not
     * fixing an oversight.
     */
    [[nodiscard]] inline bool IsUnattendedSession( const ShotOptions& shot, bool controlChannelRequested )
    {
        return shot.Active() || controlChannelRequested;
    }

    /**
     * @brief The one capture precondition that needs the disk, written so it can be tested WITHOUT one:
     *        given the resolved options and whether the `--scene` file was found, either the run may go on
     *        or it must stop with a named reason.
     *
     * THE RELATION IS "capture is active" AGAINST "the scene is there", and it is the whole of defect 1.
     * The scene loader logs a missing file and leaves the current scene standing, which is right for an
     * editor — a person sees the message and picks another scene from the menu — and wrong for a capture,
     * where nobody is watching and the run goes on to write a PNG named after the scene that was asked for
     * holding the picture of a different one. That frame is worse than no frame, because it looks exactly
     * like evidence, and one row of measurements was lost to it.
     *
     * Taking the existence as a PARAMETER rather than calling the filesystem is what lets the rule be
     * asserted instead of argued: the caller supplies `std::filesystem::exists`, a test supplies a bool.
     *
     * @param sceneExists Whether `shot.Scene` was found on disk. Ignored when no `--scene` was given.
     */
    inline Common::BoolResultStr ValidateSceneForCapture( const ShotOptions& shot, bool sceneExists )
    {
        // No --scene at all: the project's own default scene loads, and that is not this rule's business.
        if ( shot.Scene.empty() )
            return Common::MakeSuccess( true );

        if ( sceneExists )
            return Common::MakeSuccess( true );

        // Interactive `--scene` keeps the editor's behaviour: the loader complains and the session stays
        // usable. There is a person here, and the message reaches them.
        if ( !shot.Active() )
            return Common::MakeSuccess( true );

        return Common::MakeFormattedError<bool>(
             "--scene '{}' does not exist; refusing to capture a different scene under that name.", shot.Scene );
    }
} // namespace Desert::Editor
