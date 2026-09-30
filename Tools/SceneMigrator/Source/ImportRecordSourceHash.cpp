#include "ImportRecordSourceHash.hpp"

#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Assets/Serialization/ImportRecord.hpp>
#include <Common/Content/ImportRecord.hpp>
#include <Common/Content/TextAssetHeader.hpp>

namespace Desert::Migration
{
    Common::ResultStr<std::optional<std::string>> ImportRecordWithSourceHash( const std::filesystem::path& source )
    {
        using Result = std::optional<std::string>;
        namespace Ser = Desert::Assets::Serialization;
        auto record   = Ser::ReadImportRecord( source );
        if ( !record )
            return Common::MakeError<Result>( record.GetError() );
        if ( record.GetValue() && record.GetValue()->SourceHash )
            return Common::MakeSuccess( Result{} );
        const auto hash = Desert::Assets::HashMeshSourceFile( source );
        if ( !hash )
            return Common::MakeError<Result>( hash.GetError() );

        Ser::ImportRecordData data;
        auto                  kind = Common::Content::ContentKind::SkinnedMesh;
        if ( record.GetValue() )
        {
            data = *record.GetValue();
            if ( data.Header )
            {
                const auto stated = Common::Content::ContentKindNamed( data.Header->Kind );
                if ( !stated || !Ser::IsImportRecordKind( *stated ) )
                    return Common::MakeFormattedError<Result>( "'{}' states Kind '{}', which no import writes",
                                                               Common::Content::ImportRecordPathFor( source ).string(),
                                                               data.Header->Kind );
                kind = *stated;
            }
        }
        else
            data.Source = source.filename().string(); // no header: the stamp mints the GUID, as a first import does
        data.SourceHash = hash.GetValue();
        auto text       = Ser::WriteImportRecord( data, kind );
        if ( !text )
            return Common::MakeFormattedError<Result>( "'{}': {}",
                                                       Common::Content::ImportRecordPathFor( source ).string(),
                                                       text.GetError() );
        return Common::MakeSuccess( Result{ text.ExtractValue() } );
    }
} // namespace Desert::Migration
