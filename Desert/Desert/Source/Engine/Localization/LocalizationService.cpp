#include "LocalizationService.hpp"

#include <Common/Core/Logger.hpp>
#include <Common/Json/Template.hpp>

#include <spdlog/fmt/fmt.h>

#include <algorithm>

namespace Desert::Localization
{
    Localization& Localization::Get()
    {
        // ONE thread-safe initialisation (a function-local static): a reader on a worker may be the first caller.
        static Localization& instance = []() -> Localization&
        {
            static Localization created;
            const LocaleRow*    source = FindLocale( kSourceLanguage );
            // kSourceLanguage is a literal checked against the table this build compiles, so this cannot
            // fire in a shipped build; it fires the moment somebody edits the table and removes the row,
            // which is exactly when a null here would be a crash with no explanation.
            if ( source == nullptr )
            {
                LOG_ERROR( "[Localization] the source language '{}' has no row in LocaleFormat.cpp; falling "
                           "back to the first of {} languages",
                           kSourceLanguage, Locales().size() );
                source = Locales().data();
            }
            created.m_Language.store( source, std::memory_order_release );
            created.Publish( std::make_shared<const RowTable>(), /*clearMisses*/ false );
            return created;
        }();
        return instance;
    }

    std::shared_ptr<const Localization::Snapshot> Localization::Current() const
    {
        const std::lock_guard<std::mutex> lock( m_PublishMutex );
        return m_Published;
    }

    void Localization::Publish( std::shared_ptr<const RowTable> table, const bool clearMisses, const bool bump )
    {
        auto next = std::make_shared<Snapshot>();
        if ( !table )
        {
            const std::shared_ptr<const Snapshot> current = Current();
            table = current ? current->Table : std::make_shared<const RowTable>();
        }
        next->Table    = std::move( table );
        next->Language = m_Language.load( std::memory_order_relaxed );
        next->Pending.reserve( m_InFlight.size() );
        for ( const auto& [tag, count] : m_InFlight )
            next->Pending.insert( tag );
        {
            const std::lock_guard<std::mutex> lock( m_PublishMutex );
            m_Published = std::move( next );
        }
        if ( clearMisses )
        {
            const std::lock_guard<std::mutex> lock( m_MissMutex );
            m_Reported.clear();
            m_MissOrder.clear();
        }
        // After the swap: a reader that sees the new generation finds this snapshot or a newer one.
        if ( bump )
            m_Generation.fetch_add( 1, std::memory_order_acq_rel );
    }

    bool Localization::ReportMiss( const std::string& token, const bool listed )
    {
        const std::lock_guard<std::mutex> lock( m_MissMutex );
        if ( !m_Reported.insert( token ).second )
            return false;
        if ( listed )
            m_MissOrder.push_back( token );
        return true;
    }

    std::vector<std::string> Localization::Misses() const
    {
        const std::lock_guard<std::mutex> lock( m_MissMutex );
        return m_MissOrder;
    }

    Common::BoolResultStr Localization::SetLanguage( const std::string_view tag )
    {
        const LocaleRow* row = FindLocale( tag );
        if ( row == nullptr )
        {
            const std::string known =
                 Common::Text::Join( Locales(), ", ", []( const LocaleRow& candidate ) { return candidate.Tag; } );
            return Common::MakeFormattedError<bool>( "language '{}' is not one this build knows; it knows: {}",
                                                     std::string( tag ), known );
        }

        m_Requested = row;
        if ( m_Requester )
            m_Requester( *row );
        if ( m_InFlight.contains( std::string( row->Tag ) ) )
        {
            LOG_INFO( "[Localization] language {} requested; {} stays on screen until its tables arrive", row->Tag,
                      Language().Tag );
            return BOOLSUCCESS;
        }
        Commit( *row );
        return BOOLSUCCESS;
    }

    void Localization::Commit( const LocaleRow& row )
    {
        m_Requested = nullptr;
        if ( &row == &Language() )
            return;
        const std::string_view previous = Language().Tag;
        m_Language.store( &row, std::memory_order_release );
        // A key that was missing in the OLD language is not missing in the new one, so the "already
        // complained" set has to go with it. Without this, switching away and back would resolve
        // silently the second time — the log would be telling the truth about a session that no longer
        // exists.
        Publish( nullptr, /*clearMisses*/ true );
        const std::shared_ptr<const Snapshot> current = Current();
        LOG_INFO( "[Localization] language {} -> {} ({}), {} keys in {} tables, generation {}", previous, row.Tag,
                  row.Endonym, current->Table->Rows.size(), current->Table->Tables.size(), Generation() );
    }

    void Localization::BindTableSource( std::vector<std::string> languagesOnDisk, TableRequester requester )
    {
        m_LanguagesOnDisk = { languagesOnDisk.begin(), languagesOnDisk.end() };
        m_Requester       = std::move( requester );
        Publish( nullptr, /*clearMisses*/ false );
    }

    void Localization::TableRequested( const std::string_view tag )
    {
        // A pending language is what a reader sees through the snapshot: published with the request. No
        // generation bump: a pending set is not a language or table change (Generation's contract).
        ++m_InFlight[std::string( tag )];
        Publish( nullptr, /*clearMisses*/ false, /*bump*/ false );
    }

    void Localization::TableSettled( const std::string_view tag )
    {
        const auto it = m_InFlight.find( std::string( tag ) );
        if ( it == m_InFlight.end() )
            return;
        if ( --it->second <= 0 )
            m_InFlight.erase( it );
        // The switch lands on the tick its last file does - refused files included, so a language with one
        // broken table still switches, and the broken table's keys are named as misses.
        if ( m_Requested != nullptr && !m_InFlight.contains( std::string( m_Requested->Tag ) ) )
            Commit( *m_Requested );
        // The pending set as it is now (Commit published only when the language moved).
        Publish( nullptr, /*clearMisses*/ false, /*bump*/ false );
    }

    bool Localization::IsPending( const LocaleRow& language ) const
    {
        const std::shared_ptr<const Snapshot> current = Current();
        return current->Pending.contains( std::string( language.Tag ) );
    }

    void Localization::RemoveTable( RowTable& table, const std::string& tableId )
    {
        table.Tables.erase( tableId );

        // Always the full scan, never guarded by "was this id registered?": a table's rows are withdrawn
        // whether or not its id is listed.
        for ( auto it = table.Rows.begin(); it != table.Rows.end(); )
        {
            Row& row = it->second;
            for ( auto owner = row.Owners.begin(); owner != row.Owners.end(); )
            {
                if ( owner->second == tableId )
                {
                    row.Entry.Forms.erase( owner->first );
                    owner = row.Owners.erase( owner );
                }
                else
                    ++owner;
            }
            it = row.Owners.empty() ? table.Rows.erase( it ) : std::next( it );
        }
    }

    Common::BoolResultStr Localization::RegisterTable( const std::string& tableId, const std::string_view language,
                                                       StringTableData table )
    {
        // THE NEXT ROWS ARE BUILT ON A COPY and published whole: a reader never sees half a table. A
        // re-registration of the same id is a hot reload: its old rows go first, so a key deleted from the file
        // is deleted from the lookup.
        auto next = std::make_shared<RowTable>( *Current()->Table );
        RemoveTable( *next, tableId );
        const std::string tag( language );
        for ( StringTableEntry& entry : table.Entries )
        {
            Row& row = next->Rows[entry.Key];
            if ( const auto owner = row.Owners.find( tag ); owner != row.Owners.end() )
            {
                const std::string other = owner->second;
                // The copy is dropped, so this call contributes nothing at all rather than half of itself; what
                // the id held before the call is withdrawn, as the in-place path did.
                UnregisterTable( tableId );
                return Common::MakeFormattedError<bool>(
                     "string table '{}' defines key '{}' in '{}', which '{}' already defines; one key must "
                     "resolve to one string, and which of the two wins would otherwise depend on load order",
                     tableId, entry.Key, tag, other );
            }
            row.Entry.Key = entry.Key;
            row.Owners.emplace( tag, tableId );
            // The SOURCE language's note wins: it is written where the translator starts.
            if ( entry.Comment.has_value() && ( !row.Entry.Comment.has_value() || tag == kSourceLanguage ) )
                row.Entry.Comment = entry.Comment;
            row.Entry.Forms[tag] = std::move( entry.Forms );
        }
        next->Tables.insert( tableId );
        Publish( std::move( next ), /*clearMisses*/ true );
        return BOOLSUCCESS;
    }

    void Localization::UnregisterTable( const std::string& tableId )
    {
        auto next = std::make_shared<RowTable>( *Current()->Table );
        RemoveTable( *next, tableId );
        Publish( std::move( next ), /*clearMisses*/ true );
    }

    void Localization::Clear()
    {
        m_Requester = nullptr;
        m_Requested = nullptr;
        m_LanguagesOnDisk.clear();
        m_InFlight.clear();
        Publish( std::make_shared<const RowTable>(), /*clearMisses*/ true );
    }

    std::vector<std::string> Localization::TableIds() const
    {
        const std::shared_ptr<const Snapshot> current = Current();
        std::vector<std::string>              ids( current->Table->Tables.begin(), current->Table->Tables.end() );
        std::sort( ids.begin(), ids.end() );
        return ids;
    }

    std::vector<std::string> Localization::Keys() const
    {
        const std::shared_ptr<const Snapshot> current = Current();
        std::vector<std::string>              keys;
        keys.reserve( current->Table->Rows.size() );
        for ( const auto& [key, row] : current->Table->Rows )
            keys.push_back( key );
        std::sort( keys.begin(), keys.end() );
        return keys;
    }

    const LocalizedEntry* Localization::Find( const std::string_view key ) const
    {
        // GAME THREAD: the published table lives until the next writer call (m_Published holds it).
        const std::shared_ptr<const Snapshot> current = Current();
        const auto                            row     = current->Table->Rows.find( std::string( key ) );
        return row == current->Table->Rows.end() ? nullptr : &row->second.Entry;
    }

    std::vector<const LocaleRow*> Localization::AvailableLanguages() const
    {
        const std::shared_ptr<const Snapshot> current = Current();
        std::vector<const LocaleRow*>         out;
        // Walked in REGISTRY order rather than in the rows' order, so the picker is stable whatever the
        // hash map decided this run.
        for ( const LocaleRow& row : Locales() )
        {
            const bool carried =
                 m_LanguagesOnDisk.contains( std::string( row.Tag ) ) ||
                 std::any_of( current->Table->Rows.begin(), current->Table->Rows.end(), [&row]( const auto& pair )
                              { return pair.second.Entry.Forms.count( std::string( row.Tag ) ) != 0; } );
            if ( carried )
                out.push_back( &row );
        }
        return out;
    }

    Localization::Resolved Localization::Resolve( const std::string_view authored, const FormatArguments& args )
    {
        if ( !IsKeyReference( authored ) )
            return { LiteralOf( authored ), Outcome::Literal };
        // ONE snapshot for the whole resolve: the language, the rows and the pending set agree.
        const std::shared_ptr<const Snapshot> snapshot = Current();
        return ResolveKey( *snapshot, KeyOf( authored ), authored, args );
    }

    Localization::Resolved Localization::Format( const std::string_view key, const FormatArguments& args )
    {
        const std::string                     authored = std::string( 1, kKeySigil ) + std::string( key );
        const std::shared_ptr<const Snapshot> snapshot = Current();
        return ResolveKey( *snapshot, key, authored, args );
    }

    Localization::Resolved Localization::ResolveKey( const Snapshot& snapshot, const std::string_view key,
                                                     const std::string_view authored, const FormatArguments& args )
    {
        const LocaleRow&                            locale = *snapshot.Language;
        const std::string                           keyText( key );
        const std::unordered_map<std::string, Row>& rows = snapshot.Table->Rows;

        auto complain = [&]( const Outcome outcome, const std::string& reason )
        {
            const std::string token = std::string( locale.Tag ) + "/" + keyText;
            if ( ReportMiss( token, /*listed*/ true ) )
            {
                LOG_ERROR( "[Localization] {} — the screen will show '{}' instead", reason,
                           std::string( authored ) );
            }
            // THE KEY IS WHAT GETS DRAWN, sigil included. Never a blank label, never another language's
            // text: the first is undiagnosable and the second is a wrong answer that looks right.
            return Resolved{ std::string( authored ), outcome };
        };

        const auto row = rows.find( keyText );
        // NOT A MISS WHILE THE LANGUAGE IS ON ITS WAY: the key is drawn and nothing is logged, because
        // nothing is wrong yet. The miss register stays a list of real defects.
        if ( snapshot.Pending.contains( std::string( locale.Tag ) ) &&
             ( row == rows.end() || !row->second.Entry.Forms.contains( std::string( locale.Tag ) ) ) )
            return Resolved{ std::string( authored ), Outcome::Pending };
        if ( row == rows.end() )
        {
            return complain(
                 Outcome::MissingKey,
                 fmt::format( "key '{}' is in none of the {} loaded string tables", keyText, snapshot.Table->Tables.size() ) );
        }

        const auto language = row->second.Entry.Forms.find( std::string( locale.Tag ) );
        if ( language == row->second.Entry.Forms.end() )
        {
            return complain( Outcome::MissingLanguage,
                             fmt::format( "key '{}' has no '{}' translation in any loaded table", keyText,
                                          std::string( locale.Tag ) ) );
        }

        const PluralCategory category =
             args.Count.has_value() ? SelectCardinal( locale.Plural, *args.Count, args.CountFractionDigits )
                                    : PluralCategory::Other;

        const std::string* form = SelectForm( language->second, category, args.Subject );
        if ( form == nullptr )
        {
            return complain( Outcome::MissingLanguage,
                             fmt::format( "key '{}' (table '{}') has a '{}' translation with no form for "
                                          "category '{}'",
                                          keyText, row->second.Owners.at( std::string( locale.Tag ) ),
                                          std::string( locale.Tag ),
                                          std::string( PluralCategoryName( category ) ) ) );
        }

        FormattedText formatted = ApplyArguments( *form, locale, args );
        if ( !formatted.Unresolved.empty() )
        {
            const std::string names = Common::Text::Join( formatted.Unresolved, ", " );
            const std::string token = std::string( locale.Tag ) + "/" + keyText + "/{}";
            if ( ReportMiss( token, /*listed*/ false ) )
            {
                LOG_ERROR( "[Localization] key '{}' ({}) has placeholders nothing supplied: {} — they are "
                           "drawn as written",
                           keyText, std::string( locale.Tag ), names );
            }
        }
        return { std::move( formatted.Text ), Outcome::Translated };
    }
} // namespace Desert::Localization
