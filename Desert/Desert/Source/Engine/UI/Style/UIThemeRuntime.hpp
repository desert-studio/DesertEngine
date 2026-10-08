#pragma once

#include <Engine/UI/UIStyleSlots.hpp>

#include <Common/Core/AssetHandle.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// A THEME AS THE UI WALK READS IT -- pure data, flattened once at load. The file form, its parser, its
// validator and the flattening (BuildUIThemeRuntime) are the engine's asset business and stay in
// Assets/UIThemeData.hpp; the walk (UIStyleResolver) only ever holds this.
namespace Desert::UI
{
    /// A font token after the asset path has been bound to a handle. A null handle is the built-in face,
    /// not an error — an empty `Asset` path means exactly that.
    struct UIThemeResolvedFont
    {
        Common::AssetHandle Asset;
        float       Size = 20.0f;
    };

    /// "this style does not bind this slot" — the value every slot of a style starts at.
    inline constexpr uint16_t kUIThemeUnbound = 0xFFFF;

    /// One style, flattened: per slot, the index of the token in the table its kind names, or
    /// `kUIThemeUnbound`.
    struct UIThemeStyleTable
    {
        std::array<uint16_t, kStyleSlotCount> Slots{};
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
} // namespace Desert::UI
