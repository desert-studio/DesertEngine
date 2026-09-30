#include "ImportSettingsEdits.hpp"

#include <Common/Content/ImportRecord.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Engine/Assets/Serialization/ImportRecord.hpp>

#include <string>
#include <unordered_map>

namespace Desert::Editor::ImportOptions
{
    namespace
    {
        std::unordered_map<std::string, ImportSettingsEdit>& Edits()
        {
            static std::unordered_map<std::string, ImportSettingsEdit> s_Edits;
            return s_Edits;
        }
    } // namespace

    Common::ResultStr<ImportSettingsEdit*> EditOf( const std::filesystem::path& source )
    {
        const std::filesystem::path record = Common::Content::ImportRecordPathFor( source );
        // Sampled before the stat, so a write that lands after it is racy by construction (IsRacyWriteTime).
        const auto      readBegan = std::filesystem::file_time_type::clock::now();
        std::error_code ec;
        const auto      written = std::filesystem::last_write_time( record, ec );
        if ( ec )
            return Common::MakeFormattedError<ImportSettingsEdit*>( "the import record '{}' cannot be read: {}",
                                                                    record.generic_string(), ec.message() );

        // The write time is only a shortcut past the read, and only once it has settled: two writes of the
        // record inside one file-system tick leave it unchanged. A copy read at a racy stamp is checked against
        // the record's CONTENT on every ask until a read lands after the window.
        auto&      edits = Edits();
        const auto it    = edits.find( source.generic_string() );
        if ( it != edits.end() && !it->second.RecordRacy && it->second.RecordTime == written )
            return Common::MakeSuccess( &it->second );

        auto settings = Assets::Serialization::ReadImportRecordSettings( source );
        if ( !settings )
            return Common::MakeError<ImportSettingsEdit*>( settings.GetError() );
        auto kind = Assets::Serialization::ReadImportRecordKind( source );
        if ( !kind )
            return Common::MakeError<ImportSettingsEdit*>( kind.GetError() );
        const bool racy = Common::Utils::IsRacyWriteTime( written, readBegan );

        // The record states what the copy was read from: the edit not yet applied survives. A different record
        // is newer than the edit and wins over it.
        if ( it != edits.end() && it->second.Recorded == settings.GetValue() &&
             it->second.Kind == kind.GetValue() )
        {
            it->second.RecordTime = written;
            it->second.RecordRacy = racy;
            return Common::MakeSuccess( &it->second );
        }
        ImportSettingsEdit& edit = edits[source.generic_string()];
        edit = ImportSettingsEdit{ settings.GetValue(), settings.GetValue(), kind.GetValue(), written, racy };
        return Common::MakeSuccess( &edit );
    }

    void DropEdit( const std::filesystem::path& source )
    {
        Edits().erase( source.generic_string() );
    }
} // namespace Desert::Editor::ImportOptions
