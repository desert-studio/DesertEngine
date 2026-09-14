#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Localization/StringTable.hpp>

namespace Desert::Assets
{
    /**
     * @brief A string table on disk (`.destrings`), in the engine's ONE asset system.
     *
     * The same `AssetBase` / `AssetManager` as a texture, a material or a cloud type, so it gets the
     * Content Browser, the VFS (a packaged game reads its translations out of the `.dpak` like everything
     * else) and the hot reload for free. A second asset system for one file type would be a permanent
     * second place to look (contract §2.2).
     *
     * WHAT LOADING MEANS HERE, and it is the one thing this asset does that the others do not: a loaded
     * table PUBLISHES itself to `Localization::Localization`, and an unloaded one withdraws. That is what
     * makes a `.destrings` edit reach the screen without a restart — the process's lookup is rebuilt, the
     * generation moves, and the next frame resolves differently. Nothing caches a resolved string across
     * frames, so there is nothing else to invalidate.
     *
     * A FILE THAT DOES NOT PARSE IS NOT PARTIALLY LOADED. `Localization::RegisterTable` is called with the
     * whole table or not at all, and a key another table already owns refuses the WHOLE file naming both —
     * because "which of the two won" would otherwise be decided by directory-walk order, which is the least
     * reproducible thing a content pipeline can depend on.
     */
    class StringTableAsset final : public AssetBase
    {
    public:
        StringTableAsset( AssetPriority priority, const Common::Filepath& filepath );

        /// Reads, parses, validates and PUBLISHES the table. A file that is missing, empty, malformed, of
        /// an unknown format version, or that collides with another table is an ERROR carrying the reason
        /// and the offending value — never a quietly empty table, because an empty table is a screen full
        /// of keys with nothing in the log to say why.
        Common::BoolResultStr Load() override;

        /// Withdraws the table from the lookup. A key it owned resolves as missing afterwards, loudly,
        /// which is the correct state: the strings really are gone.
        Common::BoolResultStr Unload() override;

        [[nodiscard]] bool IsReadyForUse() const override
        {
            return m_Ready;
        }

        [[nodiscard]] const Localization::StringTableData& GetData() const
        {
            return m_Data;
        }

        /// What to show in a slot: the file's DisplayName when it has one, the file's stem when it does not.
        [[nodiscard]] const std::string& GetDisplayName() const
        {
            return m_DisplayName;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::StringTable;
        }

        /**
         * @brief Writes a table to disk, creating the directory if needed.
         *
         * Static because saving is what CREATES an asset — the Localization panel authors rows and writes
         * them, and only then does the AssetManager get asked to load the file back.
         *
         * Refuses a table its own parser would reject, so an unusable file is never written in the first
         * place: the round trip is the invariant, and writing something unreadable breaks it silently.
         */
        static Common::BoolResultStr Save( const Common::Filepath&              filepath,
                                           const Localization::StringTableData& data );

    private:
        /// The id this table is published under: the file's path. One table per file, so the path is both
        /// the identity and the thing a conflict message must name.
        [[nodiscard]] std::string PublishId() const;

        Localization::StringTableData m_Data;
        std::string                   m_DisplayName;
        bool                          m_Ready = false;
    };
} // namespace Desert::Assets
