#pragma once

#include <Engine/Localization/LocaleFormat.hpp>
#include <Engine/Localization/StringTable.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Localization
{
    /**
     * @file
     * @brief HOW A STRING GETS INTO AN ELEMENT — the one decision this whole task turns on.
     *
     * The problem: text is authored directly in the component (`UIText::Text`). Localising it means one
     * of three things, and two of them are wrong here.
     *
     *   * MAKE THE FIELD A KEY. Then a literal is inexpressible, and the probe scenes — whose whole job
     *     is to be invariant — would have to be translated to stay on screen.
     *   * ADD A SECOND FIELD. Two fields where the reader has to know which one wins. This project has a
     *     named defect class for that and the brief forbade it outright.
     *   * MAKE THE KEY DIFFER FROM THE LITERAL BY ITS FORM. One field. A `#` at the start means "the rest
     *     of this is a key"; anything else is a literal and is never translated. `##` at the start is a
     *     literal `#`, which is how a string that really begins with a hash stays expressible.
     *
     * The third is what this file implements. It is also what makes the hardcoded-text census possible at
     * all: a scanner can tell an authored literal from a key BY LOOKING AT IT, with no table and no
     * guessing, which is the difference between a gate that fires on new untranslatable text and a gate
     * that cries wolf on every identifier.
     *
     * WHAT A MISSING KEY DOES, and it is a decision rather than an accident: it draws THE KEY, sigil and
     * all, and logs once. No fallback to another language — a fallback is how a German build ships in
     * English with nothing in the log, which is the same silent-substitution defect §1.4 of the contract
     * is about. And never an empty string: a blank label is the least diagnosable outcome on a screen.
     */

    /// The one character that separates a key from a literal.
    inline constexpr char kKeySigil = '#';

    /// What an authored string turned out to be.
    enum class AuthoredKind
    {
        Literal,      ///< draw it as typed (with `##` unescaped to `#`)
        KeyReference, ///< look it up
    };

    /// @return whether @p authored is a key reference, i.e. starts with exactly one `#`.
    bool IsKeyReference( std::string_view authored );

    /// The key inside @p authored, i.e. everything after the leading `#`. Empty view if it is not a key
    /// reference. A key reference with an EMPTY key (`"#"`) is a key reference whose key is "" — the
    /// resolver reports it as missing by that name rather than silently drawing nothing.
    std::string_view KeyOf( std::string_view authored );

    /// @p authored as a literal: `"##done"` -> `"#done"`, everything else unchanged. Called only when
    /// IsKeyReference is false, and the ONE place the escape is undone.
    std::string LiteralOf( std::string_view authored );

    /// The inverse, for the migration that has to make existing authored text safe under the new rule:
    /// a literal that already begins with `#` gains a second one. Pure, so the migration stays testable.
    std::string EscapeLiteral( std::string_view literal );

    /// What the caller knows about the thing being talked about, and the numbers in the sentence.
    struct FormatArguments
    {
        /// The count the plural form is selected from, and the value `{n}` prints. Absent means the string
        /// has no count in it, and only the `other` (or gendered) forms are reachable.
        std::optional<double> Count;

        /// How many fraction digits `{n}` prints, and therefore CLDR's `v` operand — "1 file" and
        /// "1.0 files" are different forms of the same sentence and this is what tells them apart.
        int32_t CountFractionDigits = 0;

        /// The gender of the subject, when the language needs it. `Unspecified` makes the gendered rungs
        /// of the lookup unreachable rather than choosing one.
        Gender Subject = Gender::Unspecified;

        /// Everything else, already turned into text by whoever knows what it is. Name -> value, and the
        /// name is what appears inside the braces.
        std::vector<std::pair<std::string, std::string>> Named;
    };

    /**
     * @brief Chooses the form for (@p category, @p gender) out of one language's map.
     *
     * MOST SPECIFIC FIRST: `<gender>.<plural>`, `<gender>`, `<plural>`, `other`. Returns nullptr when the
     * map has none of the four, which the caller reports as a missing translation rather than papering
     * over with the first entry it finds.
     */
    const std::string* SelectForm( const std::map<std::string, std::string>& forms, PluralCategory category,
                                   Gender gender );

    /// The result of substituting arguments into a form's text.
    struct FormattedText
    {
        std::string Text;
        /// Placeholders the arguments did not answer for. They are left in the output VERBATIM so the
        /// screen shows `{score}` rather than a hole, and the caller logs this list once.
        std::vector<std::string> Unresolved;
    };

    /**
     * @brief Substitutes @p args into @p pattern under @p locale.
     *
     * The grammar is small and closed:
     *
     *     {n}        the count, as an integer in this locale
     *     {n:3}      the count, with three fraction digits in this locale
     *     {name}     a named argument, verbatim
     *     {{ and }}  a literal brace
     *
     * There is deliberately no printf here. `UIBindingData` used to carry an author-typed printf format
     * and hand it straight to `std::snprintf` with a double — so an author typing `%s` in the editor got
     * undefined behaviour at runtime, and a number was formatted with a C locale on every screen in every
     * language. Both go away with that field; this is what replaces it.
     */
    FormattedText ApplyArguments( std::string_view pattern, const LocaleRow& locale, const FormatArguments& args );
} // namespace Desert::Localization
