#pragma once

#include <Common/Core/DevInstruments.hpp>

// THE DELIBERATE CRASH OF A PACKAGED GAME, AND THE MOMENT IT HAPPENS (PKG1c). `--crash-test <kind>[@stage]`
// proves the handler end to end on the binary a player runs, and the two stages are the two report
// locations the Runtime has:
//
//   @early    after the archive is mounted and BEFORE the .deproj is read — the game's Name is unknown,
//             so the report lands under the engine's per-user root (%LOCALAPPDATA%/DesertEngine/Crashes,
//             $HOME/.desertengine/Crashes) and says game=unread.
//   @mounted  after the report root has moved to GameUserDirectory(<Name>)/Crashes and the Name is in the
//             report. THE DEFAULT: `--crash-test segv` means `segv@mounted`, which is what the flag did
//             before it had stages, so no existing invocation changes meaning.
//
// A development instrument, cut from Shipping the way `--shot` is (RuntimeShot.hpp): under Shipping there
// is no parser and Main.cpp reads no `--crash-test`, which Desert/Tests/Runtime/ShippingBoundary checks.
#if DESERT_DEV_INSTRUMENTS

#include <Common/Core/CrashHandler.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace Desert::Player
{
    enum class CrashTestStage : std::uint8_t
    {
        Early,   // before the .deproj is read
        Mounted, // after the report root follows the game
    };

    struct CrashTestRequest
    {
        Common::Crash::TestKind Kind  = Common::Crash::TestKind::Segv;
        CrashTestStage          Stage = CrashTestStage::Mounted;
    };

    // The words the flag accepts, for the refusal message.
    inline constexpr const char* kCrashTestUsage =
         "--crash-test <segv|abort|purecall|stackoverflow|stackoverflow-worker>[@early|@mounted]  (no stage "
         "means @mounted)";

    // `nullopt` for an unknown kind, an unknown stage or an empty stage after '@' — refused, never
    // defaulted, since a typo that crashed at the other stage would file its report somewhere else.
    [[nodiscard]] inline std::optional<CrashTestRequest> ParseCrashTest( std::string_view inArg )
    {
        const std::size_t      at   = inArg.find( '@' );
        const std::string_view kind = inArg.substr( 0, at );
        CrashTestRequest       request;
        const auto             parsed = Common::Crash::ParseTestKind( kind );
        if ( !parsed.has_value() )
        {
            return std::nullopt;
        }
        request.Kind = *parsed;
        if ( at == std::string_view::npos )
        {
            return request;
        }
        const std::string_view stage = inArg.substr( at + 1 );
        if ( stage == "early" )
        {
            request.Stage = CrashTestStage::Early;
        }
        else if ( stage == "mounted" )
        {
            request.Stage = CrashTestStage::Mounted;
        }
        else
        {
            return std::nullopt;
        }
        return request;
    }
} // namespace Desert::Player

#endif // DESERT_DEV_INSTRUMENTS
