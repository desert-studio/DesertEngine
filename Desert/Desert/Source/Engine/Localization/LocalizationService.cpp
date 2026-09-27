#include "LocalizationService.hpp"

#include <Common/Core/Logger.hpp>

#include <spdlog/fmt/fmt.h>

#include <algorithm>

namespace Desert::Localization
{
    Localization& Localization::Get()
    {
        static Localization instance;
        if ( instance.m_Language == nullptr )
        {
            instance.m_Language = FindLocale( kSourceLanguage );
            // kSourceLanguage is a literal checked against the table this build compiles, so this cannot
            // fire in a shipped build; it fires the moment somebody edits the table and removes the row,
            // which is exactly when a null here would be a crash with no explanation.
            if ( instance.m_Language == nullptr )
            {
                LOG_ERROR( "[Localization] the source language '{}' has no row in LocaleFormat.cpp; falling "
                           "back to the first of {} languages",
                           kSourceLanguage, Locales().size() );
                instance.m_Language = Locales().data();
            }
        }
        return instance;
    }

    Common::BoolResultStr Localization::SetLanguage( const std::string_view tag )
    {
        const LocaleRow* row = FindLocale( tag );
        if ( row == nullptr )
        {
            std::string known;
            for ( const LocaleRow& candidate : Locales() )
            {
                if ( !known.empty() )
                    known += ", ";
                known += std::string( candidate.Tag );
            }
            return Common::MakeFormattedError<bool>( "language '{}' is not one this build knows; it knows: {}",
                                                     std::string( tag ), known );
        }

        m_Requested = row;
        if ( m_Requester )
            m_Requester( *row );
        if ( IsPending( *row ) )
        {
            LOG_INFO( "[Localization] language {} requested; {} stays on screen until its tables arrive", row->Tag,
                      m_Language->Tag );
            return BOOLSUCCESS;
        }
        Commit( *row );
        return BOOLSUCCESS;
    }

    void Localization::Commit( const LocaleRow& row )
    {
        m_Requested = nullptr;
        if ( &row == m_Language )
            return;
        const std::string_view previous = m_Language ? m_Language->Tag : std::string_view{};
        m_Language                      = &row;
        ++m_Generation;
        // A key that was missing in the OLD language is not missing in the new one, so the "already
        // complained" set has to go with it. Without this, switching away and back would resolve
        // silently the second time — the log would be telling the truth about a session that no longer
        // exists.
        m_Reported.clear();
        m_MissOrder.clear();
        LOG_INFO( "[Localization] language {} -> {} ({}), {} keys in {} tables, generation {}", previous, row.Tag,
                  row.Endonym, m_Rows.size(), m_Tables.size(), m_Generation );
    }

    void Localization::BindTableSource( std::vector<std::string> languagesOnDisk, TableRequester requester )
    {
        m_LanguagesOnDisk = { languagesOnDisk.begin(), languagesOnDisk.end() };
        m_Requester       = std::move( requester );
        ++m_Generation;
    }

    void Localization::TableRequested( const std::string_view tag )
    {
        ++m_InFlight[std::string( tag )];
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
        if ( m_Requested != nullptr && !IsPending( *m_Requested ) )
            Commit( *m_Requested );
    }

    bool Localization::IsPending( const LocaleRow& language ) const
    {
        return m_InFlight.contains( std::string( language.Tag ) );
    }

    Common::BoolResultStr Localization::RegisterTable( const std::string& tableId, const std::string_view language,
                                                       StringTableData table )
    {
        // A re-registration of the same id is a hot reload: its old rows go first, so a key deleted from
        // the file is deleted from the lookup. Leaving them would make a removal a no-op and the feature
        // would report itself as working.
        UnregisterTable( tableId );
        const std::string tag( language );
        for ( StringTableEntry& entry : table.Entries )
        {
            Row& row = m_Rows[entry.Key];
            if ( const auto owner = row.Owners.find( tag ); owner != row.Owners.end() )
            {
                const std::string other = owner->second;
                // Undo the rows this call has already added, so a refused table contributes nothing at
                // all rather than half of itself.
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
        m_Tables.insert( tableId );
        ++m_Generation;
        m_Reported.clear();
        m_MissOrder.clear();
        return BOOLSUCCESS;
    }

    void Localization::UnregisterTable( const std::string& tableId )
    {
        m_Tables.erase( tableId );

        // Always the full scan, never guarded by "was this id registered?": RegisterTable calls this to
        // undo a PARTIAL insertion, whose rows exist while the id does not, and a guard would leave them
        // behind — a refused table half-present is worse than one that failed outright.
        for ( auto it = m_Rows.begin(); it != m_Rows.end(); )
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
            it = row.Owners.empty() ? m_Rows.erase( it ) : std::next( it );
        }

        ++m_Generation;
        m_Reported.clear();
        m_MissOrder.clear();
    }

    void Localization::Clear()
    {
        m_Rows.clear();
        m_Requester = nullptr;
        m_Requested = nullptr;
        m_LanguagesOnDisk.clear();
        m_InFlight.clear();
        m_Tables.clear();
        m_Reported.clear();
        m_MissOrder.clear();
        ++m_Generation;
    }

    std::vector<std::string> Localization::TableIds() const
    {
        std::vector<std::string> ids( m_Tables.begin(), m_Tables.end() );
        std::sort( ids.begin(), ids.end() );
        return ids;
    }

    std::vector<std::string> Localization::Keys() const
    {
        std::vector<std::string> keys;
        keys.reserve( m_Rows.size() );
        for ( const auto& [key, row] : m_Rows )
            keys.push_back( key );
        std::sort( keys.begin(), keys.end() );
        return keys;
    }

    const LocalizedEntry* Localization::Find( const std::string_view key ) const
    {
        const auto row = m_Rows.find( std::string( key ) );
        return row == m_Rows.end() ? nullptr : &row->second.Entry;
    }

    std::vector<const LocaleRow*> Localization::AvailableLanguages() const
    {
        std::vector<const LocaleRow*> out;
        // Walked in REGISTRY order rather than in the rows' order, so the picker is stable whatever the
        // hash map decided this run.
        for ( const LocaleRow& row : Locales() )
        {
            const bool carried =
                 m_LanguagesOnDisk.contains( std::string( row.Tag ) ) ||
                 std::any_of( m_Rows.begin(), m_Rows.end(), [&row]( const auto& pair )
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
        return ResolveKey( KeyOf( authored ), authored, args );
    }

    Localization::Resolved Localization::Format( const std::string_view key, const FormatArguments& args )
    {
        const std::string authored = std::string( 1, kKeySigil ) + std::string( key );
        return ResolveKey( key, authored, args );
    }

    Localization::Resolved Localization::ResolveKey( const std::string_view key, const std::string_view authored,
                                                     const FormatArguments& args )
    {
        const LocaleRow&  locale = Language();
        const std::string keyText( key );

        auto complain = [&]( const Outcome outcome, const std::string& reason )
        {
            const std::string token = std::string( locale.Tag ) + "/" + keyText;
            if ( m_Reported.insert( token ).second )
            {
                m_MissOrder.push_back( token );
                LOG_ERROR( "[Localization] {} — the screen will show '{}' instead", reason,
                           std::string( authored ) );
            }
            // THE KEY IS WHAT GETS DRAWN, sigil included. Never a blank label, never another language's
            // text: the first is undiagnosable and the second is a wrong answer that looks right.
            return Resolved{ std::string( authored ), outcome };
        };

        const auto row = m_Rows.find( keyText );
        // NOT A MISS WHILE THE LANGUAGE IS ON ITS WAY: the key is drawn and nothing is logged, because
        // nothing is wrong yet. The miss register stays a list of real defects.
        if ( IsPending( locale ) &&
             ( row == m_Rows.end() || !row->second.Entry.Forms.contains( std::string( locale.Tag ) ) ) )
            return Resolved{ std::string( authored ), Outcome::Pending };
        if ( row == m_Rows.end() )
        {
            return complain(
                 Outcome::MissingKey,
                 fmt::format( "key '{}' is in none of the {} loaded string tables", keyText, m_Tables.size() ) );
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
            std::string names;
            for ( const std::string& name : formatted.Unresolved )
            {
                if ( !names.empty() )
                    names += ", ";
                names += name;
            }
            const std::string token = std::string( locale.Tag ) + "/" + keyText + "/{}";
            if ( m_Reported.insert( token ).second )
            {
                LOG_ERROR( "[Localization] key '{}' ({}) has placeholders nothing supplied: {} — they are "
                           "drawn as written",
                           keyText, std::string( locale.Tag ), names );
            }
        }
        return { std::move( formatted.Text ), Outcome::Translated };
    }
} // namespace Desert::Localization
