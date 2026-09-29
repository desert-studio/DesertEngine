#include "ImportSettingsEdits.hpp"

#include <Common/Content/ImportRecord.hpp>
#include <Engine/Assets/Serialization/ImportRecord.hpp>

#include <string>
#include <unordered_map>

namespace Desert::Editor::ImportOptions
{
    namespace
    {
        namespace Ser = Assets::Serialization;

        std::unordered_map<std::string, ImportSettingsEdit>& Edits()
        {
            static std::unordered_map<std::string, ImportSettingsEdit> s_Edits;
            return s_Edits;
        }
    } // namespace

    Common::ResultStr<ImportSettingsEdit*> EditOf( const std::filesystem::path& source )
    {
        const std::filesystem::path record = Common::Content::ImportRecordPathFor( source );
        std::error_code             ec;
        const auto                  written = std::filesystem::last_write_time( record, ec );
        if ( ec )
            return Common::MakeFormattedError<ImportSettingsEdit*>( "the import record '{}' cannot be read: {}",
                                                                    record.generic_string(), ec.message() );

        auto& edits = Edits();
        if ( const auto it = edits.find( source.generic_string() );
             it != edits.end() && it->second.RecordTime == written )
            return Common::MakeSuccess( &it->second );

        auto settings = Ser::ReadImportRecordSettings( source );
        if ( !settings )
            return Common::MakeError<ImportSettingsEdit*>( settings.GetError() );
        auto kind = Ser::ReadImportRecordKind( source );
        if ( !kind )
            return Common::MakeError<ImportSettingsEdit*>( kind.GetError() );
        ImportSettingsEdit& edit = edits[source.generic_string()];
        edit = ImportSettingsEdit{ settings.GetValue(), settings.GetValue(), kind.GetValue(), written };
        return Common::MakeSuccess( &edit );
    }

    void DropEdit( const std::filesystem::path& source )
    {
        Edits().erase( source.generic_string() );
    }
} // namespace Desert::Editor::ImportOptions
