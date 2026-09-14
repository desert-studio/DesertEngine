#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/UIThemeData.hpp>

namespace Desert::Assets
{
    /**
     * @brief A UI theme on disk (`.detheme`), in the engine's ONE asset system.
     *
     * The same `AssetBase` / `AssetManager` as a texture, a material or a cloud type, so it gets the
     * Content Browser, the drag-and-drop payload, the scene reference and the hot reload for free.
     *
     * WHAT IT DOES NOT DO: resolve its font paths. A font in this engine is not an `AssetBase` — it is a
     * path registered with `Runtime::FontService`, which lives a layer above this one and owns a GPU
     * atlas. So the asset holds the PARSED FILE and `Runtime::UIThemeService` is what flattens it into a
     * `UIThemeRuntime`, binding each font token to the handle its path registers as. That split is the
     * same one CloudTypeAsset draws against CloudTypeService, and it is what keeps this class testable
     * without a device.
     */
    class UIThemeAsset final : public AssetBase
    {
    public:
        UIThemeAsset( AssetPriority priority, const Common::Filepath& filepath );

        /// Reads and parses the file. A file that is missing, malformed, from an unknown format version or
        /// carrying a binding the resolver cannot honour is an ERROR carrying the reason — never a quietly
        /// substituted default, because a theme that silently became a different theme renders as a UI
        /// that merely looks like somebody else's.
        Common::BoolResultStr Load() override;
        Common::BoolResultStr Unload() override;

        bool IsReadyForUse() const override
        {
            return m_Ready;
        }

        const UIThemeData& GetData() const
        {
            return m_Data;
        }

        /// What to show in a slot. The file's DisplayName when it has one, the file's stem when it does not.
        const std::string& GetDisplayName() const
        {
            return m_DisplayName;
        }

        /// Bumped by every successful Load. The service holds the revision it last flattened, so a
        /// hot-reloaded theme is rebuilt and an unchanged one is not.
        uint32_t GetRevision() const
        {
            return m_Revision;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::UITheme;
        }

        /**
         * @brief Writes a theme to disk, creating the directory if needed.
         *
         * Static because saving is what CREATES an asset — writing through an instance would mean an
         * instance had to exist for a file that does not. Refuses a theme Validate rejects, so an unusable
         * file is never written in the first place.
         */
        static Common::BoolResultStr Save( const Common::Filepath& filepath, const UIThemeData& data );

    private:
        UIThemeData m_Data;
        std::string m_DisplayName;
        bool        m_Ready    = false;
        uint32_t    m_Revision = 0;
    };
} // namespace Desert::Assets
