#pragma once

#include <Engine/Assets/UIThemeAsset.hpp>

#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace Desert::Runtime
{
    /**
     * @brief The UI themes a canvas can point at, flattened once for every viewport.
     *
     * The same shape as CloudTypeService next door and for the same two reasons: the walk must not know
     * how to read a file, and an editor with three viewports must not re-resolve one asset three times.
     * It owns no GPU resource — a theme is a table of numbers, and the one device object behind it (a
     * font atlas) belongs to FontService, which is where the font paths are bound.
     *
     * WHY THE FONT BINDING IS HERE AND NOT IN THE ASSET. A font in this engine is not an `AssetBase`: it
     * is a path registered with FontService, which lives in this layer and owns the atlas. The asset
     * layer cannot reach it without inverting the dependency, so the asset stops at the parsed file and
     * this is where the paths become handles. A path the font scan cannot find is logged HERE, once, with
     * the path in the message — and the token keeps the built-in face, which is what an element with no
     * font of its own already draws with.
     *
     * AN EMPTY SLOT IS NOT AN ERROR. A canvas with no theme resolves every slot from the elements' own
     * authored fields, which is exactly what every scene authored before themes existed does — so Get()
     * returns nullptr for an empty handle and says nothing. A handle that names a theme nobody
     * registered IS an error, logged once with the handle, and also resolves to nullptr: the UI keeps the
     * colours its author typed rather than disappearing.
     */
    class UIThemeService
    {
    public:
        /// Flattens @p asset and caches it under its handle. Called again after a hot reload; a changed
        /// revision replaces the entry, an unchanged one is a no-op.
        NO_DISCARD Common::BoolResultStr Register( const std::shared_ptr<Assets::UIThemeAsset>& asset );

        /**
         * @brief The theme a canvas's slot resolves to, or nullptr.
         *
         * NULLPTR IS A MEANINGFUL ANSWER and the two ways to get it are deliberately not the same event:
         * an empty handle is "this canvas has no theme" and is silent; an unknown handle is "the scene
         * names a .detheme the asset scan did not find" and is reported once.
         */
        const Assets::UIThemeRuntime* Get( const Assets::AssetHandle& handle );

        /**
         * @brief Bumped whenever any registered theme changes.
         *
         * A view that caches a resolved style holds the generation it cached under. Comparing the two is
         * what makes a hot reload of a `.detheme` show up in the viewport without a restart — the handle
         * in the canvas's slot has not changed, so nothing else would tell the walk to look again.
         */
        uint32_t GetGeneration() const
        {
            return m_Generation;
        }

        void Clear();

    private:
        struct Entry
        {
            Assets::UIThemeRuntime Runtime;
            uint32_t               Revision = 0;
        };

        std::unordered_map<Assets::AssetHandle, Entry> m_Themes;
        // Handles already complained about. A missing theme is a permanent state of the scene, so without
        // this the error would be logged every frame of every viewport and bury everything else.
        std::unordered_set<Assets::AssetHandle> m_Reported;
        uint32_t                                m_Generation = 0;
    };
} // namespace Desert::Runtime
