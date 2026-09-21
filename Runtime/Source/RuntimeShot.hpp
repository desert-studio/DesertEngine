#pragma once

#include <Common/Core/Core.hpp>
#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Player
{
    /**
     * @brief `--shot <out.png> [--shot-frames N]` for the SHIPPING host.
     *
     * WHY THIS EXISTS AT ALL, GIVEN THE EDITOR ALREADY HAS ONE. The editor's `--shot` reads
     * `Scene::GetFinalImage()` — the offscreen image the scene renders into — and that image is by
     * construction the part of the picture this host does NOT present while it is loading. A loading
     * screen is drawn by `Render2D` straight into the swapchain render pass, exactly like the editor's
     * panels, and no capture of the final image has ever held a pixel of either.
     *
     * So this one reads the SWAPCHAIN, through `VulkanSwapChain::RecordFrameCapture` /
     * `TakeCapturedFrameRGBA8`: the copy is recorded into the same command buffer as the frame's draws
     * and collected in `OnFramePresented`, which is the only instant at which "the picture the player is
     * looking at" exists as bytes. That also removes the editor capture's oldest trap — below the number
     * of frames in flight it writes a blank PNG, because its readback beats the first present — since
     * the copy here travels WITH the frame rather than racing it.
     *
     * Before this, the process a player actually starts could not be photographed by anything in this
     * repository. Its picture was argued about from the editor's.
     */
    struct RuntimeShot
    {
        /// Where the PNG goes. Empty means no capture, which is the normal way a game runs.
        std::string Output;
        /// WHICH PRESENTED FRAME is captured, counting from 1. The default matches the editor's so the
        /// two hosts' shots of the same scene are comparable.
        uint32_t Frames = 90;

        [[nodiscard]] bool Active() const
        {
            return !Output.empty();
        }

        static RuntimeShot& Get()
        {
            static RuntimeShot s;
            return s;
        }
    };

    /**
     * @brief Pure parse of the capture flags out of @p args (argv[1..], flags and values already split).
     *
     * Unknown tokens are IGNORED here rather than refused: this parser is one of three that walk the same
     * argv (`--project` and `--scene` are read beside it), and a strict one would have to know about
     * every flag the others own. What it does refuse is its own flags used wrongly — a `--shot` with no
     * path, a `--shot-frames` that is not a positive number — because those are silent otherwise: the
     * capture simply never happens and the process runs forever with nobody to see it.
     */
    [[nodiscard]] inline Common::BoolResultStr ParseRuntimeShot( const std::vector<std::string>& args,
                                                                 RuntimeShot&                    out )
    {
        for ( size_t i = 0; i < args.size(); ++i )
        {
            if ( args[i] == "--shot" )
            {
                if ( i + 1 >= args.size() || args[i + 1].starts_with( "--" ) )
                    return Common::MakeError<bool>( "--shot needs an output path, e.g. --shot out.png" );
                out.Output = args[++i];
            }
            else if ( args[i] == "--shot-frames" )
            {
                if ( i + 1 >= args.size() )
                    return Common::MakeError<bool>( "--shot-frames needs a frame count" );
                const std::string& value = args[++i];
                if ( value.empty() || value.find_first_not_of( "0123456789" ) != std::string::npos )
                    return Common::MakeFormattedError<bool>( "--shot-frames '{}' is not a whole number of frames",
                                                             value );
                const unsigned long long parsed = std::stoull( value );
                if ( parsed == 0 || parsed > 100000 )
                    return Common::MakeFormattedError<bool>(
                         "--shot-frames {} is out of range; frames are counted from 1", value );
                out.Frames = static_cast<uint32_t>( parsed );
            }
        }
        return BOOLSUCCESS;
    }
} // namespace Desert::Player
