#pragma once

#include <Engine/Localization/PluralRules.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Localization
{
    /**
     * @file
     * @brief The `.destrings` asset: key -> text, per language, per grammatical form.
     *
     * ONE FILE FORMAT, ONE STRUCT, reflect-cpp between them — the same idiom as `.demat` and
     * `.decloudtype`, so there is one text-asset shape in this engine rather than a third.
     *
     * WHY A FORM MAP AND NOT A `Text` FIELD WITH AN OPTIONAL `Plural` FIELD. Two fields, where a reader
     * has to know which one wins, is the defect class this task was told not to create. There is one
     * field: a map from a FORM SELECTOR to the text. A string with no grammar in it has one entry under
     * `other`, which is CLDR's name for "the form a language always has". The verbosity of `{"other":
     * "Play"}` is the price of there being exactly one place a translation can live.
     *
     * THE SELECTOR GRAMMAR, and it is the whole grammar:
     *
     *     <plural>                 one of CLDR's six category names: zero one two few many other
     *     <gender>                 masculine | feminine | neuter | common
     *     <gender>.<plural>        both at once
     *
     * Lookup is MOST SPECIFIC FIRST — `<gender>.<plural>`, then `<gender>`, then `<plural>`, then
     * `other` — so a translator adds only the distinctions their language actually makes, and an English
     * table never has to mention gender. The order is fixed, documented and asserted by
     * Desert/Tests/Engine/LocalizedText; it is not a search for "whatever is there".
     *
     * WHY GENDER IS A SELECTOR AND NOT A RULE. CLDR has no gender rules, because gender is not a
     * property of the NUMBER — it is a property of the thing being talked about, which only the caller
     * knows ("Игрок вошёл" / "Игрок вошла"). So the caller passes it and the translator decides whether
     * their language needs it. Inventing a per-language gender rule would have been inventing data, which
     * is exactly what the plural half of this file goes out of its way not to do.
     */

    /// The extension the Content Browser, the preloader and the asset registration all agree on. One
    /// constant, because a second spelling of it is a slot that silently refuses a valid file.
    inline constexpr const char* kStringTableExtension = ".destrings";

    /// The FILE layout's version. 1 — the first, and the only one that has ever existed. An unknown
    /// version is REFUSED, never read anyway: a file written by a build this one is not may spell a field
    /// the same and mean something else.
    inline constexpr int32_t kStringTableFormatVersion = 1;

    /// Grammatical gender, as a SELECTOR the caller supplies. `Unspecified` is not a fourth gender — it
    /// is "the caller said nothing", and it makes the gendered rungs of the lookup ladder unreachable
    /// rather than picking one.
    enum class Gender : uint8_t
    {
        Unspecified,
        Masculine,
        Feminine,
        Neuter,
        Common
    };

    /// The selector spelling of @p gender, or an empty view for `Unspecified`. The file's spelling and
    /// the enum come from this one table for the same reason the plural names do.
    constexpr std::string_view GenderName( const Gender gender )
    {
        switch ( gender )
        {
            case Gender::Unspecified:
                return {};
            case Gender::Masculine:
                return "masculine";
            case Gender::Feminine:
                return "feminine";
            case Gender::Neuter:
                return "neuter";
            case Gender::Common:
                return "common";
        }
        return {};
    }

    /// The inverse. Nothing on a miss — an unknown gender name in a file is a refusal by name.
    std::optional<Gender> GenderFromName( std::string_view name );

    /// One key's translations. `Forms` is language tag -> selector -> text.
    struct LocalizedEntry
    {
        /// The identity, and what an authored `UIText` writes after the `#`. Lower-case, dot-separated;
        /// validated at parse so a key that cannot be typed cannot be shipped.
        std::string Key;

        /// What this string is FOR, for whoever translates it. Optional, and the reason it exists at all
        /// is that "Back" is three different words depending on whether it is a direction, a verb or a
        /// noun, and a translator cannot see the screen.
        std::optional<std::string> Comment;

        /// language tag -> form selector -> text. A language present here with an empty map is refused at
        /// parse: "this language exists and has nothing" is the empty-successful-answer shape (DC §1.4).
        std::map<std::string, std::map<std::string, std::string>> Forms;

        [[nodiscard]] bool operator==( const LocalizedEntry& ) const = default;
    };

    /// A whole `.destrings` file.
    struct StringTableData
    {
        /// Absent means 1 — the first version, and the only one that has ever existed.
        std::optional<int32_t> FormatVersion;

        /// What to call this table in the editor. Absent means the file's stem.
        std::optional<std::string> DisplayName;

        /// The rows. Ordered, and written back in the order they were read, so a hand-edited file keeps
        /// the grouping its author gave it and a diff of two translations is readable.
        std::vector<LocalizedEntry> Entries;

        [[nodiscard]] bool operator==( const StringTableData& ) const = default;
    };

    /**
     * @brief Reads @p text as a `.destrings` file, or refuses it by name.
     *
     * Every refusal names the offending value: an unknown format version, a key that is empty or
     * malformed, a duplicate key, a language tag no LocaleRow answers for, a form selector outside the
     * grammar above, a language with no forms, and an empty translation. A `.destrings` that parses is a
     * table every lookup in this engine can be run against without a second check.
     */
    NO_DISCARD Common::ResultStr<StringTableData> ParseStringTable( const std::string& text );

    /// Serialises a table back to the text ParseStringTable reads. Total: any @p data this parser accepts
    /// writes, and re-reads equal.
    std::string WriteStringTable( const StringTableData& data );

    /// The set of form selectors a table may use, for the one caller that has to say what was allowed
    /// (the parse refusal) and for the editor's own validation. Derived from the two name tables above,
    /// so it cannot fall behind them.
    std::vector<std::string> ValidFormSelectors();
} // namespace Desert::Localization
