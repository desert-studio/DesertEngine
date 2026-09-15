#pragma once

#include <Engine/Assets/UIThemeData.hpp>
#include <Engine/UI/UIStyleSlots.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>

// WHERE AN ELEMENT'S COLOUR, FONT AND SPACING COME FROM — one object, asked once per element per frame.
//
// THE PRECEDENCE IS DATA, NOT A RULE. A slot has EXACTLY ONE source: the style's binding table if that
// style binds it, the element's own authored field if it does not. There is no "the theme wins" and no
// "the local value wins", because there is never a moment when both apply — which is the point. The
// project's most expensive recurring defect is two values that both look right and one of which quietly
// beat the other; a table lookup that either hits or misses cannot have that shape, and the Details style
// table prints the hit-or-miss for every slot so the author sees which it was.
//
// ACCESSIBILITY IS TWO FIELDS AND THEY ARE ALWAYS LIVE. FontScale multiplies every font size this object
// hands back, themed or local, so the accessibility switch works on a canvas with no theme at all.
// HighContrast swaps a colour TOKEN for its high-contrast value where the theme declares one — so it is a
// property of the palette rather than an algorithm applied to arbitrary colours, and a theme that declares
// no overrides is simply unaffected by it.
namespace Desert::UI
{
    /**
     * @brief The style ONE element resolves through.
     *
     * Cheap to copy (two pointers, a float and a bool) and built per element by the walk. A default-
     * constructed one is "no theme": every query returns its local argument, which is exactly what every
     * scene authored before themes existed does.
     */
    class ElementStyle
    {
    public:
        ElementStyle() = default;

        ElementStyle( const Assets::UIThemeRuntime* theme, const Assets::UIThemeStyleTable* table, float fontScale,
                      bool highContrast )
             : m_Theme( theme ), m_Table( table ), m_FontScale( fontScale ), m_HighContrast( highContrast )
        {
        }

        /// Does the style bind @p slot? The Details style table asks this to say "Theme" or "Local" per
        /// row; the queries below ask it to decide.
        [[nodiscard]] bool IsThemed( StyleSlot slot ) const
        {
            return m_Theme != nullptr && m_Table != nullptr &&
                   m_Table->Slots[static_cast<std::size_t>( slot )] != Assets::kUIThemeUnbound;
        }

        /// The colour for @p slot, or @p local when the style does not bind it.
        [[nodiscard]] glm::vec3 Color( StyleSlot slot, const glm::vec3& local ) const
        {
            if ( !IsThemed( slot ) )
                return local;

            const std::size_t index = m_Table->Slots[static_cast<std::size_t>( slot )];
            if ( index >= m_Theme->Colors.size() )
                return local;

            // THE HIGH-CONTRAST OVERLAY IS SPARSE AND THAT IS WHY IT IS CHECKED HERE RATHER THAN BAKED.
            // Baking it at load would need a second copy of the whole palette; a token with no override
            // has to keep its ordinary value, and this is the one place that knows the flag.
            //
            // THE SUBSCRIPT IS EVALUATED ONCE, and that is a fix and not a tidy-up. It used to be
            // `m_HighContrast && m_Theme->HighContrast[index].has_value()` guarding
            // `*m_Theme->HighContrast[index]` — two separate subscript expressions, so the guard on the
            // first says nothing about the second to anyone who is not the author. scripts/CI/CheckTidy's
            // REGISTER half reports it as bugprone-unchecked-optional-access on this line, and that half
            // is the one that covers the lines nobody is editing. Binding the element to a reference makes
            // the checked thing and the dereferenced thing the same object, for the reader as well.
            if ( m_HighContrast )
            {
                if ( const auto& highContrast = m_Theme->HighContrast[index]; highContrast.has_value() )
                {
                    return *highContrast;
                }
            }

            return m_Theme->Colors[index];
        }

        /// The metric (a radius, a width, a padding, a spacing — design px) for @p slot, or @p local.
        [[nodiscard]] float Metric( StyleSlot slot, float local ) const
        {
            if ( !IsThemed( slot ) )
                return local;

            const std::size_t index = m_Table->Slots[static_cast<std::size_t>( slot )];
            return index < m_Theme->Metrics.size() ? m_Theme->Metrics[index] : local;
        }

        /// The font size for @p slot (the theme's when bound, @p local when not), SCALED. Every font size
        /// the walk draws at goes through here or through ScaleFontSize below, so the accessibility
        /// multiplier cannot be missed on one code path and applied on another.
        [[nodiscard]] float FontSize( StyleSlot slot, float local ) const
        {
            if ( !IsThemed( slot ) )
                return ScaleFontSize( local );

            const std::size_t index = m_Table->Slots[static_cast<std::size_t>( slot )];
            return ScaleFontSize( index < m_Theme->Fonts.size() ? m_Theme->Fonts[index].Size : local );
        }

        /// A font size the theme has nothing to say about (the auto-size floor), scaled by the same
        /// multiplier — or the floor would stop shrinking with the text it is a floor for.
        [[nodiscard]] float ScaleFontSize( float local ) const
        {
            return local * m_FontScale;
        }

        /// The font asset for @p slot, or @p local when the style does not bind it. A themed font whose
        /// path the asset scan did not find resolves to a null handle, which every consumer already reads
        /// as "the built-in face"; the THEME ASSET logs that path, because only it knows what the path was.
        [[nodiscard]] Assets::AssetHandle Font( StyleSlot slot, const Assets::AssetHandle& local ) const
        {
            if ( !IsThemed( slot ) )
                return local;

            const std::size_t index = m_Table->Slots[static_cast<std::size_t>( slot )];
            return index < m_Theme->Fonts.size() ? m_Theme->Fonts[index].Asset : local;
        }

        /// The theme behind this element, or nullptr. For the editor's style table and for log messages;
        /// the walk never needs it.
        [[nodiscard]] const Assets::UIThemeRuntime* Theme() const
        {
            return m_Theme;
        }

    private:
        const Assets::UIThemeRuntime*    m_Theme        = nullptr;
        const Assets::UIThemeStyleTable* m_Table        = nullptr;
        float                            m_FontScale    = 1.0f;
        bool                             m_HighContrast = false;
    };

    /**
     * @brief The canvas's half: the theme it points at and the two accessibility knobs, bound once per
     *        canvas walk and asked for an ElementStyle per element.
     *
     * WHY THE CANVAS AND NOT THE PROCESS. Ю4 keyed every piece of UI walk state by (canvas x view)
     * precisely because a value owned by the process cannot express two canvases or two viewports, and a
     * theme held in a global would reintroduce that in the one place an author notices instantly — a HUD
     * and a menu overlay in one scene cannot have two looks, and the UI Editor's preview cannot show a
     * theme the viewport is not on. WHY NOT PER ELEMENT: an element picks a STYLE, which is a role inside
     * a theme. Two themes inside one canvas is two palettes fighting on one screen, and the ancestor walk
     * it would cost is paid per element per frame for a capability a second canvas already expresses
     * (UICanvas has a Sort Order for exactly that).
     */
    class CanvasStyle
    {
    public:
        CanvasStyle() = default;

        /// @p theme may be null — that is a canvas with no theme, and every element is then fully local.
        CanvasStyle( const Assets::UIThemeRuntime* theme, float fontScale, bool highContrast )
             : m_Theme( theme ), m_FontScale( std::max( 0.01f, fontScale ) ), m_HighContrast( highContrast )
        {
        }

        [[nodiscard]] const Assets::UIThemeRuntime* Theme() const
        {
            return m_Theme;
        }

        [[nodiscard]] float FontScale() const
        {
            return m_FontScale;
        }

        [[nodiscard]] bool HighContrast() const
        {
            return m_HighContrast;
        }

        /**
         * @brief The style an element resolves through.
         *
         * @p styleName is the element's own (`UIStyleData::Style`), or the theme's default style name for
         * an element carrying no UIStyle component.
         *
         * @p unknownStyle is set when a theme IS bound and does not declare @p styleName. That is a
         * genuine authoring mistake — a typo in a style name silently draws the element's local colours,
         * which looks exactly like a theme that does not cover this element — so the caller reports it,
         * ONCE per name per canvas cell. The element falls back to fully local, which is the value the
         * author did write, rather than to an invented default or to nothing drawn at all.
         */
        [[nodiscard]] ElementStyle For( const std::string& styleName, bool& unknownStyle ) const
        {
            unknownStyle = false;
            if ( m_Theme == nullptr )
                return ElementStyle( nullptr, nullptr, m_FontScale, m_HighContrast );

            const Assets::UIThemeStyleTable* table = m_Theme->FindStyle( styleName );
            unknownStyle                           = table == nullptr;
            return ElementStyle( m_Theme, table, m_FontScale, m_HighContrast );
        }

    private:
        const Assets::UIThemeRuntime* m_Theme        = nullptr;
        float                         m_FontScale    = 1.0f;
        bool                          m_HighContrast = false;
    };
} // namespace Desert::UI
