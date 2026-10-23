#pragma once

// VFX-13. The ONE editor of a curve-sourced `.dfx` stack input (VFXInputSource::Curve), whatever module owns it:
// UE's stack shows a module input typed as a curve inline (FRichCurve for a float, a Color Curve drawn as a
// gradient for a colour), sampled by the particle's normalised age. Every module's curve input is the same
// VFXModuleInput::Curve - one channel of VFXCurveKey per component - so there is one editor for all of them, and
// the control channel's `set` lands in the same functions the widgets call.
//
// STOPS. Both widgets edit the curve as a list of STOPS: the union of the key times of its channels, each with the
// value of every channel at that time (UE's gradient editor stops; for one channel a stop is exactly a key). A
// stop edit touches only the keys AT that time - a channel without a key there gets a Linear one - so the
// interpolation and tangents of every other key survive (no rebuild from the stops).

#include <Engine/Assets/Serialization/VFXSystem.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace Desert::Editor::VFXCurveEdit
{
    using Channels = std::vector<std::vector<Assets::Serialization::VFXCurveKey>>;

    /// Two stop times closer than this are one stop (the `.dfx` reader requires strictly increasing key times).
    inline constexpr float kStopEpsilon = 1e-4f;

    /// The union of the channels' key times, increasing.
    [[nodiscard]] std::vector<float> StopTimes( const Channels& channels );

    /// Every channel's value at @p time (components past the channel count are zero).
    [[nodiscard]] glm::vec4 ValueAt( const Channels& channels, float time );

    /// Sets the value of every channel at @p time: the key at that time when the channel has one, else a new
    /// Linear key there.
    void SetStop( Channels& channels, float time, const glm::vec4& value );

    /// Moves the keys at @p from to @p to in every channel. Refused (false, nothing changed) when another key of a
    /// channel already sits at @p to, so a drag can never merge two keys.
    bool MoveStop( Channels& channels, float from, float to );

    /// Removes the keys at @p time from every channel. Refused (false) when a channel would be left with no key.
    bool RemoveStop( Channels& channels, float time );

    /**
     * @brief The whole curve from a packed key list: per stop its time then @p components values, times strictly
     * increasing; every key Linear (the control channel's `<input>.Keys`, UE's paste of a key list).
     */
    [[nodiscard]] Common::ResultStr<Channels> FromPacked( std::span<const float> packed, std::size_t components );

    /// The packed form FromPacked reads, of the curve's stops.
    [[nodiscard]] std::vector<float> ToPacked( const Channels& channels );

    /// Which widget of the drawn editor began or ended an edit gesture this frame (one gesture = one undo step).
    struct Gesture
    {
        bool Activated   = false;
        bool Deactivated = false;
    };

    /**
     * @brief Draws the curve input inline: a gradient bar for a colour (four channels), a plot for the others,
     * then one row per stop (time, value, remove). Clicking the bar or the plot adds a stop; dragging a stop's
     * marker moves it in time.
     */
    Gesture Draw( const char* id, Channels& channels, bool colour );
} // namespace Desert::Editor::VFXCurveEdit
