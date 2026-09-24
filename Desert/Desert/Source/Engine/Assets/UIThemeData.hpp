#pragma once

#include <Engine/UI/UIStyleSlots.hpp>

#include <Engine/Assets/Common.hpp>
#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Core.hpp>
#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Desert::Assets
{
    /**
     * @file
     * @brief A UI THEME: named colours, metrics and fonts, the styles that bind them to element slots,
     *        the file they live in (`.detheme`), and the pure functions that turn one into the other.
     *
     * WHY AN ASSET AND NOT A TABLE IN A HEADER. The same argument `.decloudtype` made and for the same
     * reason: a theme that is not a file cannot be made, named, duplicated, diffed, or dropped into a
     * slot, and a game that ships two looks would need the engine rebuilt to get the second one.
     *
     * WHY TWO LEVELS OF INDIRECTION (slot -> token -> value) AND NOT ONE. A style could have bound
     * `Button.Normal` straight to an RGB triple, and then "the accent colour" would appear once per slot
     * that uses it — six times for the button alone — so changing it means finding all six. The token
     * table is what makes an accent ONE number, and it is also what makes the accessibility overlay below
     * cheap: a high-contrast pass overrides TOKENS, so a theme author states the handful that need to
     * change instead of maintaining a second complete palette in lockstep with the first.
     *
     * WHAT IS NOT HERE. The EDITOR's own chrome. `Editor/Core/ThemeManager` is an ImGui palette for the
     * tool's windows and it stays where it is: the tool's look is not the product's, the two have
     * disjoint consumers (ImGui vs Render2D), and merging them would make every game theme able to
     * recolour the editor. That boundary is a decision, stated here so the next reader does not re-derive
     * it as a duplication.
     */

    /// The extension the Content Browser, the file dialog, the drag-and-drop target and the preloader all
    /// agree on. One constant, because a second spelling of it is a slot that silently refuses a valid file.
    inline constexpr const char* kUIThemeExtension = ".detheme";

    /// Where the shipped themes live, RELATIVE to the project's assets root — the same relative form every
    /// reflected asset slot is serialized in, so a scene that names one is portable off this machine.
    inline constexpr const char* kUIThemeAssetsRelativeDir = "UI/Themes/";

    // THE SAME DIRECTORY, SAID TWICE, AND THE TWO CANNOT DRIFT. `Constants::Path` states the layout
    // against the PROJECT root; a scene's asset slot and the shipped library state it against the ASSETS
    // root, which is the form that survives leaving this machine. Two statements of one fact is the defect
    // shape this project keeps paying for, so the relation is asserted rather than remembered.
    static_assert( Common::Constants::Path::Detail::Spec( Common::Constants::Path::ContentDir::UITheme ).Rel ==
                        std::string_view( kUIThemeAssetsRelativeDir ),
                   "the UI theme library's assets-root-relative directory and its content-census row must "
                   "name the same folder" );

    /// The FILE layout's version. Bumped when a field moves; an unknown version is REFUSED rather than
    /// read as if it meant what it means here.
    ///
    /// VERSION 2 SINCE T7b: the file opens with the text asset header (Kind "UITheme", the GUID that IS the
    /// theme's identity and its handle - UIThemeAsset's constructor - and this number under the tag `UITH`),
    /// and states its version nowhere else. A version-1 file (top-level FormatVersion, no header) is refused
    /// by name; Tools/SceneMigrator mints its GUID once.
    inline constexpr int32_t kUIThemeFormatVersion = static_cast<int32_t>( kUIThemeSchemaVersion );

    /// The subsystem versions a .detheme of this build states: the UI theme schema, and nothing else.
    [[nodiscard]] inline std::span<const Common::Content::SubsystemVersion> UIThemeTextSubsystems()
    {
        static const std::array<Common::Content::SubsystemVersion, 1> versions = {
             Common::Content::SubsystemVersion{ kUIThemeSchemaTag, static_cast<uint32_t>( kUIThemeFormatVersion ) } };
        return versions;
    }

    /// The style every element uses unless it names another one. A theme without it themes nothing and
    /// says so once; it is a name and not an empty string so that "no style" cannot be confused with "the
    /// author left the field blank".
    inline constexpr const char* kUIThemeDefaultStyle = "Default";

    /// One named colour. Linear RGB, exactly as the components' own colour fields are, so a token and the
    /// literal it replaces are the same number and swapping one for the other cannot shift the picture.
    struct UIThemeColor
    {
        std::string Name;
        glm::vec3   Value = glm::vec3( 1.0f );

        [[nodiscard]] bool operator==( const UIThemeColor& ) const = default;
    };

    /// One named number in design pixels: a corner radius, a border width, a padding, a spacing.
    struct UIThemeMetric
    {
        std::string Name;
        float       Value = 0.0f;

        [[nodiscard]] bool operator==( const UIThemeMetric& ) const = default;
    };

    /// One named font: the asset and the size it is used at. ONE entry rather than two tokens, because
    /// "Body" is a typeface at a size — a theme that could bind a face without its size would let a style
    /// change the family and leave the element's own 34 px heading size behind it.
    struct UIThemeFont
    {
        std::string Name;
        /// Project-relative path of the font asset (e.g. "Fonts/Inter-Regular.ttf"). Empty means the
        /// engine's built-in default face, which is what an element with no font of its own already uses.
        std::string Asset;
        float       Size = 20.0f;

        [[nodiscard]] bool operator==( const UIThemeFont& ) const = default;
    };

    /// One slot of one style, bound to a token.
    struct UIThemeBinding
    {
        std::string Slot;  // a name from UI::kStyleSlotInfo, e.g. "Button.Normal"
        std::string Token; // a name from Colors / Metrics / Fonts, matching the slot's kind

        [[nodiscard]] bool operator==( const UIThemeBinding& ) const = default;
    };

    /**
     * @brief A named set of slot bindings — what CSS calls a class and UMG calls a style.
     *
     * A style is NOT per element kind. `Primary` may bind `Button.Normal` and `Panel.Color` at once; a
     * button reads the first, a panel the second, and neither sees the other. That is what lets one name
     * mean one idea ("this is the primary affordance") across the element set instead of per widget.
     *
     * A slot this style does not bind is LOCAL: the element's own authored field supplies it. The binding
     * table is therefore the per-slot precedence, and it is DATA rather than a rule — there is nothing
     * that can quietly win, because a slot has exactly one source and the Details style table prints
     * which one it is.
     */
    struct UIThemeStyle
    {
        std::string                 Name;
        std::vector<UIThemeBinding> Slots;

        [[nodiscard]] bool operator==( const UIThemeStyle& ) const = default;
    };

    /**
     * @brief One theme on disk, and in memory — the same struct, because there is nothing to convert.
     *
     * The optionals are what a hand-written file may leave out. The four tables are not optional in the
     * sense that matters: an absent table is an EMPTY table, which is a theme that binds nothing, which
     * is legal and observable (nothing is themed) rather than a silent failure.
     */
    struct UIThemeData
    {
        /// The text asset header, FIRST so the registry reads it without parsing the rest. Absent only on
        /// a theme that has never been written: WriteUITheme stamps it (the GUID kept, or minted when new).
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;
        std::optional<std::string>                                DisplayName;
        std::optional<std::string> Notes;

        std::vector<UIThemeColor>  Colors;
        std::vector<UIThemeMetric> Metrics;
        std::vector<UIThemeFont>   Fonts;
        std::vector<UIThemeStyle>  Styles;

        /**
         * @brief ACCESSIBILITY: colour tokens as they are under the high-contrast switch.
         *
         * SPARSE, and that is the whole point. Every name here must also exist in `Colors`; it REPLACES
         * that token's value while the canvas's High Contrast flag is on, and a token with no row here
         * keeps its ordinary value. So a theme author states the six or seven pairs whose separation is
         * too low and nothing else.
         *
         * WHY THIS RATHER THAN A SECOND THEME FILE, which would also work: a second file duplicates every
         * token that did NOT change, and two complete palettes maintained in lockstep is the second source
         * of truth §2.2 forbids — the day someone adds `Accent` to one of them, the other silently keeps
         * the old look. The overlay cannot drift, because there is only ever one list of tokens.
         */
        std::vector<UIThemeColor> HighContrastColors;

        [[nodiscard]] bool operator==( const UIThemeData& ) const = default;
    };

    // ----------------------------------------------------------------------------------------------
    // THE RUNTIME FORM
    // ----------------------------------------------------------------------------------------------
    //
    // The file is a list of named rows because that is what a human edits and a diff reads. The walk
    // resolves a slot per element per frame and must not do a string lookup to do it, so the rows are
    // flattened ONCE — at load — into indices, and a style becomes an array indexed by StyleSlot.
    //
    // The flattening is also where every name in the file is CHECKED, which is the reason it is a
    // separate step with its own result rather than a constructor: a theme whose binding names a slot
    // that does not exist, or a token in the wrong table, is refused with both names in the message,
    // instead of that binding quietly not being there.

    /// A font token after the asset path has been bound to a handle. A null handle is the built-in face,
    /// not an error — an empty `Asset` path means exactly that.
    struct UIThemeResolvedFont
    {
        AssetHandle Asset;
        float       Size = 20.0f;
    };

    /// "this style does not bind this slot" — the value every slot of a style starts at.
    inline constexpr uint16_t kUIThemeUnbound = 0xFFFF;

    /// One style, flattened: per slot, the index of the token in the table its kind names, or
    /// `kUIThemeUnbound`.
    struct UIThemeStyleTable
    {
        std::array<uint16_t, UI::kStyleSlotCount> Slots{};
    };

    /**
     * @brief A theme ready to be asked a question sixty times a second.
     *
     * Built by BuildUIThemeRuntime from a parsed file plus the font handles its paths resolved to. Pure
     * data: no asset manager, no GPU, no globals, so the whole resolution path is testable without either.
     */
    struct UIThemeRuntime
    {
        std::string Name; // DisplayName, or the file stem — for log messages and the Details style table

        std::vector<std::string> ColorNames;
        std::vector<glm::vec3>   Colors;
        /// Parallel to `Colors`; an entry without a value is a token the high-contrast pass leaves alone.
        std::vector<std::optional<glm::vec3>> HighContrast;

        std::vector<std::string> MetricNames;
        std::vector<float>       Metrics;

        std::vector<std::string>         FontNames;
        std::vector<UIThemeResolvedFont> Fonts;

        std::unordered_map<std::string, UIThemeStyleTable> Styles;

        /// Bumped by the owning asset on every successful load, so a view that caches a resolved style can
        /// tell a hot-reloaded theme from the same one.
        uint32_t Revision = 0;

        [[nodiscard]] const UIThemeStyleTable* FindStyle( const std::string& name ) const
        {
            const auto it = Styles.find( name );
            return it == Styles.end() ? nullptr : &it->second;
        }
    };

    // ----------------------------------------------------------------------------------------------
    // Pure functions over the two forms
    // ----------------------------------------------------------------------------------------------

    /**
     * @brief Rejects a theme the resolver cannot honour, naming what is wrong.
     *
     * Pure, so the theme editor can refuse to save for the same reason the loader refuses to read, rather
     * than the two disagreeing about what is legal. Every refusal names the row: "the style 'Primary'
     * binds 'Button.Norml', which is not a slot" is the defect itself; "the theme is invalid" is a
     * morning spent bisecting a file.
     */
    NO_DISCARD Common::BoolResultStr ValidateUIThemeData( const UIThemeData& data );

    /**
     * @brief Parses the text of a `.detheme`, or says why it could not.
     *
     * Reads the version FIRST and on its own, for the reason ParseCloudType does: a file from another
     * format fails a full parse with a message about whichever field happens to be missing, which is true
     * and useless. An unknown version is refused in BOTH directions.
     */
    NO_DISCARD Common::ResultStr<UIThemeData> ParseUITheme( const std::string& text );

    /// Serialises a theme back to the text ParseUITheme reads. Total over any @p data Validate accepts,
    /// and re-reads equal.
    std::string WriteUITheme( const UIThemeData& data );

    /**
     * @brief Flattens a validated theme into the form the walk queries.
     *
     * @p fontHandles maps a font token's NAME to the handle its `Asset` path resolved to. A name missing
     * from the map is a font whose file the asset scan did not find: the entry keeps the built-in face and
     * the CALLER (the asset) logs it, because only the caller knows the file it came from.
     *
     * Returns an error for exactly the things Validate rejects — it calls it — so a runtime built from a
     * file the loader accepted cannot fail here.
     */
    NO_DISCARD Common::ResultStr<UIThemeRuntime>
               BuildUIThemeRuntime( const UIThemeData& data, std::string_view name,
                                    const std::unordered_map<std::string, AssetHandle>& fontHandles );

    /// The path a scene stores for a shipped theme: the relative directory, the name, the extension.
    /// Pure and free of any global, so a test can predict it.
    inline std::string UIThemeAssetRelativePath( std::string_view name )
    {
        std::string path = kUIThemeAssetsRelativeDir;
        path.append( name );
        path.append( kUIThemeExtension );
        return path;
    }
} // namespace Desert::Assets
