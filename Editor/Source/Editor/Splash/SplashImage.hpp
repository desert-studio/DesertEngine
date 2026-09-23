#pragma once

// THE SPLASH'S PICTURE, READ OUT OF OUR OWN COOKED `.tex` AND NOT OUT OF THE JPEG. Decoding still images
// is the cook's job everywhere else in this engine (T3.3 took the last image decoder out of the runtime),
// and a splash that carried its own JPEG path would be the one place the editor still decoded a source
// at start-up. The cook (`TextureImporter`, intent Colour from `Splash.detex`) writes BC7; this reads the
// container and runs the SAME CPU BC7 decoder the block-compression suite measures the encoder with, on
// the splash's own thread, into the RGBA8 a window system can take.

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace Desert::Editor::Splash
{
    // The editor's splash SOURCE, relative to the engine's working directory (the directory holding
    // `Resources/`). One spelling, read by the cook stage, by the splash and by nothing else.
    inline const std::filesystem::path kSplashSource = "Resources/Splash/Splash.jpg";

    struct SplashPixels
    {
        uint32_t Width  = 0;
        uint32_t Height = 0;
        // Tightly packed RGBA8, top row first — the order both CGImage and a top-down DIB take.
        std::vector<unsigned char> Rgba;
        // How long the decode took, for the one log line that reports it.
        double DecodeMs = 0.0;
    };

    /// Reads @p cookedTex, decodes its full-size level to RGBA8. A refusal names the file and what was
    /// wrong with it; the splash then draws on its dark background and logs that sentence once.
    [[nodiscard]] Common::ResultStr<SplashPixels> LoadSplashPixels( const std::filesystem::path& cookedTex );
} // namespace Desert::Editor::Splash
