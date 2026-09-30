#pragma once

#include <Common/Core/ResultStr.hpp>

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

    [[nodiscard]] inline InputStep MakeInputStep( InputAction action, float x, float y, int32_t code = 0,
                                                  int32_t mods = 0 )
    {
        return { action, x, y, code, mods, {} };
    }

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
        if ( kind == "key" )
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
            return Common::MakeSuccess( Frames{ std::move( down ), std::move( up ) } );
        }
        return Common::MakeFormattedError<Frames>(
             "'{}' is not an input kind: move, click, press, release, key, drop.", kind );
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
