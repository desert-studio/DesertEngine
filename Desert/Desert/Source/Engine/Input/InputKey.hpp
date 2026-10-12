#pragma once

#include <Common/Core/KeyCodes.hpp>
#include <Common/Core/MouseButton.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// THE KEYS AN INPUT MAPPING CONTEXT CAN NAME (GP1a) — UE's FKey, as far as this engine's devices go: a keyboard
// key, a mouse button, or one of the mouse's relative axes. A mapping states its key BY NAME in the asset
// ("W", "Space", "LeftMouseButton", "Mouse2D"), so a file reads the same on every platform and a rename of an
// enumerator never silently rebinds a context. InputKeyFromName is the ONE parser of those names: the asset
// reader refuses a name it does not know, and the subsystem resolves names through it.
namespace Desert::Input
{
    struct InputKey
    {
        enum class Device : uint8_t
        {
            Keyboard,    // Code is a Common::KeyCode
            MouseButton, // Code is a Common::MouseButton
            MouseX,      // relative cursor motion this frame, X (pixels)
            MouseY,      // relative cursor motion this frame, Y (pixels)
            Mouse2D,     // both, as one 2D value (UE's Mouse XY 2D-Axis)
        };

        Device   Kind = Device::Keyboard;
        uint16_t Code = 0;

        [[nodiscard]] bool operator==( const InputKey& ) const = default;
    };

    /// The key a mapping's name states: "A".."Z", "0".."9", Space, Enter, Escape, Tab, Backspace, Up, Down,
    /// Left, Right, F1..F12, Left/Right Shift/Control/Alt, Left/Right/MiddleMouseButton, MouseX, MouseY,
    /// Mouse2D. Anything else is nullopt — never a default key.
    std::optional<InputKey> InputKeyFromName( std::string_view name );

    /// Every name InputKeyFromName accepts, each once (letters, digits, F1..F12, then the named keys): the
    /// editor's key picker lists exactly what a mapping may state.
    [[nodiscard]] std::vector<std::string> InputKeyNames();

    /**
     * @brief One frame of raw device state, as the platform layer sampled it — the subsystem's only input.
     *
     * The subsystem never reads a device itself: a frame is data, so a scripted sequence of frames is the
     * same evaluation the game runs (the triggers are tested that way).
     */
    struct RawInputFrame
    {
        std::vector<Common::KeyCode>     KeysDown;
        std::vector<Common::MouseButton> MouseButtonsDown;
        glm::vec2                        MouseDelta{ 0.0f };
    };

    /// The raw value @p key has in @p frame, before any modifier: 1 or 0 in X for a key or button, the
    /// delta in X for MouseX/MouseY, (dx, dy) for Mouse2D.
    glm::vec3 RawKeyValue( const InputKey& key, const RawInputFrame& frame );
} // namespace Desert::Input
