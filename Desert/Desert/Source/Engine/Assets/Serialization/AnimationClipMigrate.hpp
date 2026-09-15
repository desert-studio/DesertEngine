#pragma once

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <string>

namespace Desert::Assets::Serialization
{
    /**
     * @brief What a `.anim` conversion did, so the tool can say it and a test can assert it.
     *
     * `KeysMoved` counts the key times that did NOT land on a whole project tick and had to be rounded.
     * For a clip authored in seconds at 24000 ticks per second that is zero for every time expressible in
     * multiples of 1/24000 s — which is every time the corpus holds. A non-zero count is not a failure; it
     * is the number this conversion owes the reader.
     */
    struct AnimationMigrationReport
    {
        int         FromVersion            = 0;
        std::size_t KeysMoved              = 0;
        int64_t     WorstMicro             = 0;
        int32_t     DisplayRateNumerator   = 0;
        int32_t     DisplayRateDenominator = 1;
        bool        DisplayRateIsAFallback = false;
    };

    /**
     * @brief `.anim` generation 0 (float seconds under a fictional `TicksPerSecond`) -> generation 1.
     *
     * PURE: a string in, a string out, no filesystem and no asset system, so the rules below are testable
     * without a file — which is the contract's requirement for a migration step and the reason this is not
     * written inside the tool.
     *
     * WHAT GENERATION 0 ACTUALLY WAS. A key carried `float Time` in units of `TicksPerSecond` ticks, and
     * every clip this repository shipped set that rate to 1.0 — so a "tick" was a second and 210 of the
     * corpus's 267 key times were fractional. The conversion is therefore `seconds = Time / TicksPerSecond`
     * followed by `tick = round( seconds * 24000 )`, and the rounding is counted rather than assumed away.
     *
     * THE DISPLAY RATE IS DERIVED, NOT DEFAULTED. A migrated clip gets the COARSEST standard grid that
     * every one of its own key times already lies on exactly — 8 fps for the corpus, whose keys are at
     * multiples of an eighth of a second. Handing them the default 30 instead would leave every key off
     * the grid the Sequencer snaps to, so the first drag of any key would move every other key's neighbour
     * onto a different instant than the one the animator authored. When no standard grid fits, the report
     * says so and the default is used.
     *
     * NOT IDEMPOTENT, AND IT SAYS SO. A file already at generation 1 is REFUSED rather than converted a
     * second time: running this twice over a v1 file would read integer ticks as if they were seconds. The
     * migrator's own re-run guard is the version stamp, which is the lesson the sigil-doubling bug taught
     * (`#menu.play` -> `##menu.play`).
     */
    [[nodiscard]] Common::ResultStr<std::string> MigrateAnimationJson( const std::string&        json,
                                                                       AnimationMigrationReport& report );
} // namespace Desert::Assets::Serialization
