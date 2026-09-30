#pragma once

// THE UI LIFT's declarations: scene v40's `UIAnim` block (the lift's input and nothing else's) and the
// v40 -> v41 lift into a UIAnimation-host Sequence (the contract table in Engine/Animation/Timeline/Hosts.hpp,
// "UI animation"). The engine reads no v40 block, so the lift is the migrator's alone — beside `LiftClip`
// (ClipGeneration3.hpp), as ANIM-I8a placed the clip's.

#include <Engine/Animation/TimeModel.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace Desert::Animation::Timeline
{
    struct UILiftReport
    {
        uint32_t RoundedKeys        = 0; ///< keys whose float time was not on the tick grid
        float    MaxRoundingSeconds = 0.0F;
        float    MaxEasingDeviation = 0.0F; ///< from Elastic/Bounce bakes (Channel.hpp, EasingResult)
    };

    struct UILiftResult
    {
        Sequence     Lifted;
        UILiftReport Report;
    };

    /**
     * @brief The `UIAnim` block as scene v40 stated it — the INPUT of the v40 -> v41 lift, read by the scene
     * migrator and by nothing at runtime. Enums travel as the integers the block stored: `Property` is
     * `ECS::UITweenProperty` (Offset 0, Size 1, Opacity 2, Color 3), `Easing` is `ECS::UIEasing`.
     */
    struct UIAnimationKeyV40
    {
        float     Time   = 0.0F;
        glm::vec4 Value  = glm::vec4( 0.0F );
        int       Easing = 5; ///< CubicOut, the v40 default
    };
    struct UIAnimationTrackV40
    {
        int                            Property = 0;
        std::vector<UIAnimationKeyV40> Keys;
    };
    struct UIAnimationV40
    {
        std::vector<UIAnimationTrackV40> Tracks;
        float                            Duration = 1.0F;
        bool                             Loop     = false;
        bool                             Playing  = true;
    };

    /**
     * @brief A v40 UI clip → its sequence. @p widgetLocator is the owning element's entity UUID as text.
     *
     * The playback range is [0, Duration] (widened to the keys when a key lies outside it); each track is one
     * Absolute section over that range. Refuses by name: an unknown property, an easing outside `UIEasing`,
     * keys not sorted by time, two keys that round onto one tick.
     */
    [[nodiscard]] Common::ResultStr<UILiftResult> LiftUIAnimation( const UIAnimationV40& legacy,
                                                                   std::string_view      widgetLocator,
                                                                   FrameRate tickRate, FrameRate displayRate );
} // namespace Desert::Animation::Timeline
