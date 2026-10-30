#pragma once

#include <Common/Core/ResultStr.hpp>

#include <charconv>
#include <cstdint>
#include <deque>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Editor::Control
{
    enum class InputAction : uint8_t
    {
        Cursor,
        ButtonDown,
        ButtonUp,
        KeyDown,
        KeyUp,
        Drop,
        Scroll,
        Char, // one typed character (Code = its codepoint), as glfwSetCharCallback delivers it
    };

    struct InputStep
    {
        InputAction              Action = InputAction::Cursor;
        float                    X      = 0.0f;
        float                    Y      = 0.0f;
        int32_t                  Code   = 0;
        int32_t                  Mods   = 0;
        std::vector<std::string> Paths;
    };

    using InputFrame = std::vector<InputStep>;

    struct InputKeyName
    {
        std::string_view Name;
        int32_t          Code;
        int32_t          ModBit;
    };

    inline constexpr int32_t kModShift   = 0x0001;
    inline constexpr int32_t kModControl = 0x0002;
    inline constexpr int32_t kModAlt     = 0x0004;
    inline constexpr int32_t kModSuper   = 0x0008;

    inline constexpr InputKeyName kInputModifiers[] = {
         { "shift", 340, kModShift }, { "ctrl", 341, kModControl }, { "alt", 342, kModAlt },
         { "super", 343, kModSuper }, { "cmd", 343, kModSuper },
    };

    inline constexpr InputKeyName kInputNamedKeys[] = {
         { "space", 32, 0 },      { "escape", 256, 0 }, { "enter", 257, 0 }, { "tab", 258, 0 },
         { "backspace", 259, 0 }, { "delete", 261, 0 }, { "right", 262, 0 }, { "left", 263, 0 },
         { "down", 264, 0 },      { "up", 265, 0 },     { "f1", 290, 0 },    { "f2", 291, 0 },
         { "f3", 292, 0 },        { "f4", 293, 0 },     { "f5", 294, 0 },    { "f6", 295, 0 },
         { "f7", 296, 0 },        { "f8", 297, 0 },     { "f9", 298, 0 },    { "f10", 299, 0 },
         { "f11", 300, 0 },       { "f12", 301, 0 },
    };

    struct InputChord
    {
        int32_t                   Key = 0;
        std::vector<InputKeyName> Modifiers;
    };

    [[nodiscard]] inline std::string LowerInputName( std::string_view text )
    {
        std::string lower( text );
        for ( char& c : lower )
            if ( c >= 'A' && c <= 'Z' )
                c = static_cast<char>( c - 'A' + 'a' );
        return lower;
    }

    /// "shift+ctrl" -> kModShift | kModControl: the modifiers a pointer drag holds. Empty = none.
    [[nodiscard]] inline Common::ResultStr<int32_t> ParseInputModifiers( std::string_view text );

    [[nodiscard]] inline Common::ResultStr<InputChord> ParseInputChord( std::string_view text )
    {
        InputChord  chord;
        std::size_t start = 0;
        while ( start <= text.size() )
        {
            const std::size_t plus = text.find( '+', start );
            const std::string part = LowerInputName(
                 text.substr( start, plus == std::string_view::npos ? std::string_view::npos : plus - start ) );
            const bool last = plus == std::string_view::npos;
            if ( part.empty() )
                return Common::MakeFormattedError<InputChord>(
                     "'{}' has an empty part; a chord reads like Ctrl+Shift+T.", text );
            if ( !last )
            {
                const InputKeyName* modifier = nullptr;
                for ( const InputKeyName& candidate : kInputModifiers )
                    if ( candidate.Name == part )
                        modifier = &candidate;
                if ( modifier == nullptr )
                    return Common::MakeFormattedError<InputChord>(
                         "'{}' in '{}' is not a modifier: shift, ctrl, alt, super (cmd).", part, text );
                chord.Modifiers.push_back( *modifier );
                start = plus + 1;
                continue;
            }
            if ( part.size() == 1 &&
                 ( ( part[0] >= 'a' && part[0] <= 'z' ) || ( part[0] >= '0' && part[0] <= '9' ) ) )
                chord.Key = part[0] >= 'a' ? part[0] - 'a' + 65 : part[0];
            else
                for ( const InputKeyName& candidate : kInputNamedKeys )
                    if ( candidate.Name == part )
                        chord.Key = candidate.Code;
            if ( chord.Key == 0 )
                return Common::MakeFormattedError<InputChord>(
                     "'{}' is not a key this channel can press: a letter, a digit, space, escape, enter, tab, "
                     "backspace, delete, the arrows or f1..f12.",
                     part );
            break;
        }
        return Common::MakeSuccess( std::move( chord ) );
    }

    [[nodiscard]] inline Common::ResultStr<int32_t> ParseInputModifiers( std::string_view text )
    {
        int32_t     mods  = 0;
        std::size_t start = 0;
        while ( start < text.size() )
        {
            const std::size_t plus = text.find( '+', start );
            const std::string part = LowerInputName(
                 text.substr( start, plus == std::string_view::npos ? std::string_view::npos : plus - start ) );
            const InputKeyName* modifier = nullptr;
            for ( const InputKeyName& candidate : kInputModifiers )
                if ( candidate.Name == part )
                    modifier = &candidate;
            if ( modifier == nullptr )
                return Common::MakeFormattedError<int32_t>(
                     "'{}' in '{}' is not a modifier: shift, ctrl, alt, super (cmd).", part, text );
            mods |= modifier->ModBit;
            if ( plus == std::string_view::npos )
                break;
            start = plus + 1;
        }
        return Common::MakeSuccess( mods );
    }

    [[nodiscard]] inline InputStep MakeInputStep( InputAction action, float x, float y, int32_t code = 0,
                                                  int32_t mods = 0 )
    {
        return { action, x, y, code, mods, {} };
    }

    /// The button an 'input' request that names none presses (Request::Button absent): ImGui's left button.
    inline constexpr int32_t kLeftMouseButton = 0;

    [[nodiscard]] inline Common::ResultStr<std::vector<InputFrame>>
    PlanInput( std::string_view kind, float x, float y, int32_t button, std::string_view key,
               const std::vector<std::string>& paths )
    {
        using Frames           = std::vector<InputFrame>;
        const InputStep cursor = MakeInputStep( InputAction::Cursor, x, y );
        if ( kind == "move" )
            return Common::MakeSuccess( Frames{ { cursor } } );
        if ( kind == "click" )
            return Common::MakeSuccess( Frames{ { cursor },
                                                { cursor, MakeInputStep( InputAction::ButtonDown, x, y, button ) },
                                                { MakeInputStep( InputAction::ButtonUp, x, y, button ) } } );
        if ( kind == "press" )
            return Common::MakeSuccess(
                 Frames{ { cursor }, { cursor, MakeInputStep( InputAction::ButtonDown, x, y, button ) } } );
        if ( kind == "release" )
            return Common::MakeSuccess(
                 Frames{ { cursor, MakeInputStep( InputAction::ButtonUp, x, y, button ) } } );
        if ( kind == "drop" )
        {
            if ( paths.empty() )
                return Common::MakeError<Frames>( "'drop' needs at least one file path." );
            InputStep drop = MakeInputStep( InputAction::Drop, x, y );
            drop.Paths     = paths;
            return Common::MakeSuccess( Frames{ { cursor }, { cursor, drop } } );
        }
        // scroll = the mouse wheel over the point, `key` = signed notches (negative scrolls the content up, as a
        // wheel turned towards the user): the wheel event a mouse sends, so the hovered window scrolls as it would.
        if ( kind == "scroll" )
        {
            int32_t notches = 0;
            const auto [end, error] = std::from_chars( key.data(), key.data() + key.size(), notches );
            if ( error != std::errc() || end != key.data() + key.size() || notches == 0 )
                return Common::MakeFormattedError<Frames>(
                     "'scroll' needs a non-zero whole number of wheel notches, not '{}'.", key );
            return Common::MakeSuccess(
                 Frames{ { cursor }, { cursor, MakeInputStep( InputAction::Scroll, x, y, notches ) } } );
        }
        // key = press and release (a tap); keydown = press and HOLD until a keyup of the same chord — the same
        // key events a keyboard sends, so they reach whatever holds keyboard focus (the played viewport's
        // player, a text field) by the same route.
        if ( kind == "key" || kind == "keydown" || kind == "keyup" )
        {
            const auto chord = ParseInputChord( key );
            if ( !chord.IsSuccess() )
                return Common::MakeError<Frames>( chord.GetError() );
            int32_t    mods = 0;
            InputFrame down;
            for ( const InputKeyName& modifier : chord.GetValue().Modifiers )
            {
                mods |= modifier.ModBit;
                down.push_back( MakeInputStep( InputAction::KeyDown, x, y, modifier.Code, mods ) );
            }
            down.push_back( MakeInputStep( InputAction::KeyDown, x, y, chord.GetValue().Key, mods ) );
            InputFrame up{ MakeInputStep( InputAction::KeyUp, x, y, chord.GetValue().Key, mods ) };
            for ( const InputKeyName& modifier : std::views::reverse( chord.GetValue().Modifiers ) )
            {
                mods &= ~modifier.ModBit;
                up.push_back( MakeInputStep( InputAction::KeyUp, x, y, modifier.Code, mods ) );
            }
            if ( kind == "keydown" )
                return Common::MakeSuccess( Frames{ std::move( down ) } );
            if ( kind == "keyup" )
                return Common::MakeSuccess( Frames{ std::move( up ) } );
            return Common::MakeSuccess( Frames{ std::move( down ), std::move( up ) } );
        }
        // type = the TEXT a keyboard types: the character events GLFW sends beside the key events
        // (glfwSetCharCallback), which is the only thing a text field reads as typing - a key event alone
        // types nothing. All of @p key's codepoints go in one frame, so a field takes them as one typed run
        // (one undo step), as a paste-speed typist would.
        if ( kind == "type" )
        {
            InputFrame typed;
            for ( std::size_t i = 0; i < key.size(); )
            {
                const auto  lead  = static_cast<unsigned char>( key[i] );
                std::size_t count = 0;
                char32_t    code  = 0;
                if ( lead < 0x80u )
                    code = lead;
                else if ( ( lead & 0xE0u ) == 0xC0u )
                {
                    count = 1;
                    code  = lead & 0x1Fu;
                }
                else if ( ( lead & 0xF0u ) == 0xE0u )
                {
                    count = 2;
                    code  = lead & 0x0Fu;
                }
                else if ( ( lead & 0xF8u ) == 0xF0u )
                {
                    count = 3;
                    code  = lead & 0x07u;
                }
                else
                    return Common::MakeFormattedError<Frames>( "'type' text '{}' is not UTF-8.", key );
                if ( i + count >= key.size() )
                    return Common::MakeFormattedError<Frames>( "'type' text '{}' ends inside a character.", key );
                for ( std::size_t b = 1; b <= count; ++b )
                {
                    const auto next = static_cast<unsigned char>( key[i + b] );
                    if ( ( next & 0xC0u ) != 0x80u )
                        return Common::MakeFormattedError<Frames>( "'type' text '{}' is not UTF-8.", key );
                    code = ( code << 6u ) | ( next & 0x3Fu );
                }
                typed.push_back( MakeInputStep( InputAction::Char, x, y, static_cast<int32_t>( code ) ) );
                i += count + 1;
            }
            if ( typed.empty() )
                return Common::MakeError<Frames>( "'type' needs the text to type." );
            return Common::MakeSuccess( Frames{ std::move( typed ) } );
        }
        return Common::MakeFormattedError<Frames>(
             "'{}' is not an input kind: move, click, press, release, scroll, key, keydown, keyup, type, drop.",
             kind );
    }

    class InputInjection
    {
    public:
        [[nodiscard]] static Common::BoolResultStr Arm( std::vector<InputFrame> frames )
        {
            if ( !Queue().empty() )
                return Common::MakeError( "input is already playing; its reply has not been sent yet." );
            for ( InputFrame& frame : frames )
                Queue().push_back( std::move( frame ) );
            return Common::MakeSuccess( true );
        }

        [[nodiscard]] static std::optional<InputFrame> NextFrame()
        {
            if ( Queue().empty() )
                return std::nullopt;
            InputFrame frame = std::move( Queue().front() );
            Queue().pop_front();
            return frame;
        }

        [[nodiscard]] static bool Playing() noexcept
        {
            return !Queue().empty();
        }

    private:
        static std::deque<InputFrame>& Queue()
        {
            static std::deque<InputFrame> queue;
            return queue;
        }
    };
} // namespace Desert::Editor::Control
