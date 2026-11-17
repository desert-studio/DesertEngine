#pragma once

#include <Engine/Localization/LocalizedText.hpp>

#include <Common/Core/ResultStr.hpp>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
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
     *
     * THREADS (FPS-GAME-RENDER-PIPE, debt #18; UE: the game thread builds, the readers see an immutable copy).
     * The writers - SetLanguage, BindTableSource, TableRequested/Settled, RegisterTable, UnregisterTable,
     * Clear - are the GAME THREAD's. They never edit what a reader holds: each builds the next immutable
     * `Snapshot` (the rows copied only when the rows change, shared otherwise) and publishes it with one
     * pointer swap and a generation bump. `Resolve`, `Format`, `Language`, `IsPending` and `Generation` read
     * one published snapshot from start to end and are safe on any thread (the UI text walk on a worker);
     * the miss register they append to has its own lock. `Find`, `Keys`, `TableIds`, `AvailableLanguages`
     * and `RequestedLanguage` are the editor panel's, game thread only; `Find`'s pointer lives until the
     * next writer call, as before.
     */
    class Localization
    {
    public:
        /// The language a build starts in, and the language the shipped tables are authored in. English,
        /// because that is what the engine's own content is written in; a project whose tables have no
        /// English is told so by name at the first resolve rather than shown blank labels.
        /// A `string_view` and not a `const char*`: the pointer-ownership census counts every raw pointer
        /// member in the tree and asks it to name its guard, and a constant that need not be one should not
        /// add a row to that register.
        static constexpr std::string_view kSourceLanguage = "en";

        static Localization& Get();

        /// Never null. Starts at `kSourceLanguage`.
        [[nodiscard]] const LocaleRow& Language() const
        {
            return *m_Language.load( std::memory_order_acquire );
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

        /**
         * WHAT A SWITCH COSTS SINCE AL1-7b: the new language's files are REQUESTED, never read in the frame.
         * While they are on their way the language on screen stays the old one - `Language()` does not move
         * and no label turns into its key - and the switch lands (generation bumped) on the tick the last of
         * them is published. A language with no request outstanding switches at once, as before. This is
         * the language asked for; `Language()` is the one resolved against.
         */
        [[nodiscard]] const LocaleRow& RequestedLanguage() const
        {
            return m_Requested != nullptr ? *m_Requested : Language();
        }

        /// Asked with a language whose files are to be requested; answers nothing - each request it makes
        /// is reported through `TableRequested` and, when it ends either way, `TableSettled`.
        using TableRequester = std::function<void( const LocaleRow& language )>;

        /**
         * @brief Tells the lookup which languages have tables on disk and who fetches them.
         *
         * The asset layer calls it once per project (`Assets::BeginStringTables`) from the content
         * registry, which knows every table's language from its path WITHOUT opening one - so the picker
         * can list a language nothing has read yet, and `SetLanguage` has someone to ask. Rebinding
         * replaces both; `Clear` forgets both.
         */
        void BindTableSource( std::vector<std::string> languagesOnDisk, TableRequester requester );

        /// The requester's bookkeeping: one call per file asked for, one per file whose request ended
        /// (published, refused or cancelled). A language with any left is PENDING.
        void TableRequested( std::string_view tag );
        void TableSettled( std::string_view tag );

        /// Whether @p language has a table requested and not yet arrived.
        [[nodiscard]] bool IsPending( const LocaleRow& language ) const;

        /// Bumped by a language change and by any table (de)registration. For a consumer that caches a
        /// resolved string — the editor's own panels do — comparing it is the whole invalidation rule.
        [[nodiscard]] uint32_t Generation() const
        {
            return m_Generation.load( std::memory_order_acquire );
        }

        /**
         * @brief Publishes @p table, @p language's rows of one table, under @p id (the file's path).
         *
         * Refuses a key that another table already owns IN THE SAME LANGUAGE, naming both files: two tables
         * answering one key is a lookup whose answer depends on load order, which is the least reproducible defect
         * a content pipeline can have. Re-registering the same id replaces that table's rows (this is what a hot
         * reload does) and is not a conflict with itself.
         */
        NO_DISCARD Common::BoolResultStr RegisterTable( const std::string& tableId, std::string_view language,
                                                        StringTableData table );

        /// Drops a table's rows. Unknown id is a no-op — an unload of something never loaded is not an
        /// error, it is the state the caller wanted.
        void UnregisterTable( const std::string& tableId );

        /// Every table forgotten. For a project close, and for a test that must not leak into the next.
        void Clear();

        /// The ids currently registered, sorted — the editor's panel lists them.
        [[nodiscard]] std::vector<std::string> TableIds() const;

        /// Every language the project has tables in - on disk (`BindTableSource`) or loaded - sorted by the
        /// registry's own order so a picker is stable. DERIVED, never declared: a language a project has no
        /// strings in is not a language that project supports.
        [[nodiscard]] std::vector<const LocaleRow*> AvailableLanguages() const;

        /// How a resolve ended. The caller that draws needs `Text`; the caller that reports needs this.
        enum class Outcome
        {
            Literal,         ///< the authored string was not a key; `Text` is it, unescaped
            Translated,      ///< the key resolved in the current language
            MissingKey,      ///< no table holds this key at all
            MissingLanguage, ///< the key exists, and has no form for the current language
            Pending          ///< the current language's tables are still on their way; `Text` is the key, and
                             ///< nothing is logged, because nothing is wrong yet
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
        [[nodiscard]] std::vector<std::string> Keys() const;

        /// The row @p key resolves against, or nullptr. STRUCTURAL: it answers "what does the content say
        /// about this key", where Resolve answers "what should be drawn". The panel needs the first —
        /// asking the second would push a miss into the register for every key it merely listed.
        [[nodiscard]] const LocalizedEntry* Find( std::string_view key ) const;

        /// Every (key, language) this session has failed to resolve, newest last. The editor's panel
        /// shows it, and its emptiness is the only cheap answer to "is this build's content translated?".
        /// A copy: a resolve on another thread may append while the caller reads.
        [[nodiscard]] std::vector<std::string> Misses() const;

    private:
        struct Row
        {
            /// language -> the table id that contributed that language's forms; needed to name both files in
            /// a conflict, and to withdraw exactly one file's forms.
            std::map<std::string, std::string> Owners;
            LocalizedEntry                     Entry;
        };
        /// The rows and the table ids: copied on a table change only, shared by every snapshot in between.
        struct RowTable
        {
            std::unordered_map<std::string, Row> Rows;
            std::unordered_set<std::string>      Tables;
        };
        /// What a reader sees, all of it from one publish: never written after `Publish`.
        struct Snapshot
        {
            std::shared_ptr<const RowTable> Table;
            const LocaleRow*                Language = nullptr;
            /// Languages with a table requested and not yet arrived (m_InFlight's keys at the publish).
            std::unordered_set<std::string> Pending;
        };

        /// The snapshot published last (any thread; one short lock, one reference count).
        [[nodiscard]] std::shared_ptr<const Snapshot> Current() const;
        /// GAME THREAD: publishes the next snapshot - @p table (null: the current rows), the language, the
        /// pending set. @p clearMisses: the miss register goes with it. @p bump: the generation moves (a
        /// language or table change; a pending-set change alone is not one, Generation's contract).
        void Publish( std::shared_ptr<const RowTable> table, bool clearMisses, bool bump = true );
        /// Withdraws one table's forms from @p table (RegisterTable's replace, UnregisterTable).
        static void RemoveTable( RowTable& table, const std::string& tableId );

        Resolved ResolveKey( const Snapshot& snapshot, std::string_view key, std::string_view authored,
                             const FormatArguments& args );
        void     Commit( const LocaleRow& row );
        /// Appends @p token to the miss register; true the first time (the caller logs then).
        bool     ReportMiss( const std::string& token, bool listed );

        // GAME THREAD (the writers' own state).
        const LocaleRow*                     m_Requested = nullptr;
        TableRequester                       m_Requester;
        std::unordered_set<std::string>      m_LanguagesOnDisk;
        std::unordered_map<std::string, int> m_InFlight;

        // PUBLISHED (THREADS above).
        std::atomic<const LocaleRow*>   m_Language{ nullptr };
        std::atomic<uint32_t>           m_Generation{ 0 };
        mutable std::mutex              m_PublishMutex;
        std::shared_ptr<const Snapshot> m_Published; // under m_PublishMutex

        // The miss register: appended by any resolving thread.
        mutable std::mutex              m_MissMutex;
        std::unordered_set<std::string> m_Reported;  // under m_MissMutex
        std::vector<std::string>        m_MissOrder; // under m_MissMutex
    };
} // namespace Desert::Localization
