#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// WHERE AN AUTHORED UI STRING BECOMES THE STRING ON SCREEN, as the walk asks it and nothing more.
//
// Every label, option and placeholder the walk draws goes through ResolveLabel / SplitOptions /
// the input field's placeholder, and those asked the process-wide Localization singleton. The framework
// asks this instead: the engine's answer (UI/Ecs/LocalizationUITextSource) is that same singleton, so what
// is drawn does not change; a host with no localization can hand in its own.
namespace Desert::UI
{
    // One resolved string. @p Literal is the half of the relation the walk branches on: a bound number
    // REPLACES a literal label, and is a key's `{n}` argument otherwise.
    struct UIResolvedText
    {
        std::string Text;
        bool        Literal = true;
    };

    class IUITextSource
    {
    public:
        virtual ~IUITextSource() = default;

        // @p authored as drawn: a literal unescaped and untouched, a key translated in the current language
        // (or itself when it does not resolve). @p count is the plural count and the value `{n}` prints.
        // @p count is std::nullopt for text that has no plural form.
        [[nodiscard]] virtual UIResolvedText Resolve( std::string_view authored, std::optional<double> count ) = 0;

        // @p value laid out as the reader's locale writes numbers, with @p fractionDigits after the separator.
        [[nodiscard]] virtual std::string FormatNumber( double value, int32_t fractionDigits ) = 0;
    };
} // namespace Desert::UI
