#pragma once

#include <Engine/Localization/LocalizedText.hpp>

#include <Common/Core/ResultStr.hpp>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Desert::Localization
{
    /**
     * @brief THE current language, and every string table loaded under it.
     *
     * A global and flat store, deliberately, and for the same reasons `UI::UIDataStore` next door is one:
     * a key is a name, a process presents one language at a time, and the writer (gameplay, the editor's
     * language menu) must not have to know a UI exists.
     *
     * IT SURVIVES A SCENE LOAD, and that is a requirement rather than an accident. The data store is
     * emptied when a world goes away because its values belong to that world; a translation belongs to
     * the PROCESS, and a language that reset itself every time the editor opened a scene would be a
     * setting that silently undoes itself — the failure У13 catalogued.
     *
     * WHERE THE BOOT LANGUAGE COMES FROM. `kSourceLanguage`, a constant, plus `--language <tag>` on the
     * command line. NOT the machine's OS locale, and that is a decision with a measurement behind it:
     * every headless frame this project verifies with would then depend on the machine that took it, and
     * a pixel A/B between two machines would be comparing two languages. Reading the OS locale belongs to
     * whatever task gives a PLAYER a settings screen; the seam for it is `SetLanguage`, which is public.
     *
     * THERE IS NO FIELD FOR IT IN `.deproj` AND THAT IS NOT AN OVERSIGHT. The project descriptor's struct
     * lives in the `desert-shared` submodule, shared with the launcher, and this task does not own it.
     * The set of languages a project HAS is derived anyway — it is the union of what its tables carry —
     * and `Common/Core/Constants.hpp` already refused fourteen derivable fields on that ground.
     */
    class Localization
    {
    public:
        /// The language a build starts in, and the language the shipped tables are authored in. English,
        /// because that is what the engine's own content is written in; a project whose tables have no
        /// English is told so by name at the first resolve rather than shown blank labels.
        static constexpr const char* kSourceLanguage = "en";

        static Localization& Get();

        /// Never null. Starts at `kSourceLanguage`.
        const LocaleRow& Language() const
        {
            return *m_Language;
        }

        /**
         * @brief Switches the whole process to @p tag.
         *
         * Refuses, by name, a tag no `LocaleRow` answers for — an unknown language is one of the three
         * "nothing breaks silently" cases this subsystem owes, and a silent no-op would leave a menu
         * whose entry does nothing.
         *
         * A successful switch bumps `Generation()`; nothing is cached against a language anywhere, so
         * the next frame simply resolves differently. That is what makes the switch live: no scene
         * reload, no asset reload, no canvas rebuild.
         */
        NO_DISCARD Common::BoolResultStr SetLanguage( std::string_view tag );

        /// Bumped by a language change and by any table (de)registration. For a consumer that caches a
        /// resolved string — the editor's own panels do — comparing it is the whole invalidation rule.
        uint32_t Generation() const
        {
            return m_Generation;
        }

        /**
         * @brief Publishes @p table under @p id (the asset's path — one table per file).
         *
         * Refuses a key that another table already owns, naming both files: two tables answering one key
         * is a lookup whose answer depends on load order, which is the least reproducible defect a
         * content pipeline can have. Re-registering the same id replaces that table's rows (this is what
         * a hot reload does) and is not a conflict with itself.
         */
        NO_DISCARD Common::BoolResultStr RegisterTable( const std::string& id, StringTableData table );

        /// Drops a table's rows. Unknown id is a no-op — an unload of something never loaded is not an
        /// error, it is the state the caller wanted.
        void UnregisterTable( const std::string& id );

        /// Every table forgotten. For a project close, and for a test that must not leak into the next.
        void Clear();

        /// The ids currently registered, sorted — the editor's panel lists them.
        std::vector<std::string> TableIds() const;

        /// Every language at least one loaded row carries a translation in, sorted by the registry's own
        /// order so a picker is stable. DERIVED, never declared: a language a project has no strings in
        /// is not a language that project supports.
        std::vector<const LocaleRow*> AvailableLanguages() const;

        /// How a resolve ended. The caller that draws needs `Text`; the caller that reports needs this.
        enum class Outcome
        {
            Literal,        ///< the authored string was not a key; `Text` is it, unescaped
            Translated,     ///< the key resolved in the current language
            MissingKey,     ///< no table holds this key at all
            MissingLanguage ///< the key exists, and has no form for the current language
        };

        struct Resolved
        {
            std::string Text;
            Outcome     Outcome = Outcome::Literal;
        };

        /**
         * @brief The one entry point every drawn string goes through.
         *
         * A literal comes back unescaped and untouched — it is never translated, which is the half of the
         * relation that makes the other half meaningful. A key comes back as its translation, or as
         * ITSELF (sigil included) when it does not resolve, with one `LOG_ERROR` per (key, language) per
         * session naming which of the two misses it was.
         */
        Resolved Resolve( std::string_view authored, const FormatArguments& args = {} );

        /// The lookup on its own, for gameplay that holds a key rather than an authored field. @p key has
        /// NO sigil. Same outcomes and the same logging.
        Resolved Format( std::string_view key, const FormatArguments& args = {} );

        /// Every key any loaded table defines, sorted. For the editor's Localization panel, which is the
        /// only consumer that needs to walk the whole lookup rather than ask it one question.
        std::vector<std::string> Keys() const;

        /// The row @p key resolves against, or nullptr. STRUCTURAL: it answers "what does the content say
        /// about this key", where Resolve answers "what should be drawn". The panel needs the first —
        /// asking the second would push a miss into the register for every key it merely listed.
        const LocalizedEntry* Find( std::string_view key ) const;

        /// Every (key, language) this session has failed to resolve, newest last. The editor's panel
        /// shows it, and its emptiness is the only cheap answer to "is this build's content translated?".
        const std::vector<std::string>& Misses() const
        {
            return m_MissOrder;
        }

    private:
        struct Row
        {
            std::string    Table; ///< which id contributed it — needed to name both files in a conflict
            LocalizedEntry Entry;
        };

        Resolved ResolveKey( std::string_view key, std::string_view authored, const FormatArguments& args );

        const LocaleRow*                     m_Language = nullptr;
        std::unordered_map<std::string, Row> m_Rows;
        std::unordered_set<std::string>      m_Tables;
        std::unordered_set<std::string>      m_Reported;
        std::vector<std::string>             m_MissOrder;
        uint32_t                             m_Generation = 0;
    };
} // namespace Desert::Localization
