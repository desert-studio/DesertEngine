#include "ImportRecordSourceHash.hpp"

#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Assets/Serialization/ImportRecord.hpp>
#include <Common/Content/ImportRecord.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Engine/Assets/Serialization/Mesh.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>

#include <fstream>
#include <iterator>

namespace Desert::Migration
{
    namespace
    {
        // The box of the committed `.skmesh`: the import wrote it in the engine's space, the options applied,
        // so it IS the box RecordImport states (NodeMeshSplit.cpp RecordImport -> SourceToEngineBounds).
        Common::ResultStr<Desert::Assets::Serialization::ImportRecordData::Box>
        SkinnedMeshBox( const std::filesystem::path& skinnedMesh )
        {
            using Box = Desert::Assets::Serialization::ImportRecordData::Box;
            std::ifstream file( skinnedMesh, std::ios::binary );
            if ( !file )
                return Common::MakeFormattedError<Box>( "'{}' could not be opened for its box",
                                                        skinnedMesh.string() );
            const std::string bytes{ std::istreambuf_iterator<char>( file ), std::istreambuf_iterator<char>() };
            const auto mesh = Desert::Assets::Serialization::ReadMeshAssetData( bytes, skinnedMesh.string() );
            if ( !mesh )
                return Common::MakeError<Box>( mesh.GetError() );
            const auto bounds = Desert::Assets::Serialization::MeshDataBounds( mesh.GetValue() );
            if ( !bounds )
                return Common::MakeFormattedError<Box>( "'{}' has no submesh, so no box", skinnedMesh.string() );
            return Common::MakeSuccess( Box{ { bounds->Min.x, bounds->Min.y, bounds->Min.z },
                                             { bounds->Max.x, bounds->Max.y, bounds->Max.z } } );
        }
    } // namespace

    Common::ResultStr<std::optional<std::string>>
    ImportRecordWithSourceHash( const std::filesystem::path& source, const std::filesystem::path& skinnedMesh )
    {
        using Result  = std::optional<std::string>;
        namespace Ser = Desert::Assets::Serialization;
        auto record   = Ser::ReadImportRecord( source );
        if ( !record )
            return Common::MakeError<Result>( record.GetError() );
        Ser::ImportRecordData data;
        auto                  kind = Common::Content::ContentKind::SkinnedMesh;
        if ( const auto& stored = record.GetValue(); stored.has_value() )
        {
            data = *stored;
            if ( data.Header )
            {
                const auto stated = Common::Content::ContentKindNamed( data.Header->Kind );
                if ( !stated || !Ser::IsImportRecordKind( *stated ) )
                    return Common::MakeFormattedError<Result>(
                         "'{}' states Kind '{}', which no import writes",
                         Common::Content::ImportRecordPathFor( source ).string(), data.Header->Kind );
                kind = *stated;
            }
        }
        else
            data.Source =
                 source.filename().string(); // no header: the stamp mints the GUID, as a first import does
        const bool needsBox = kind == Common::Content::ContentKind::SkinnedMesh && !data.Bounds;
        if ( data.SourceHash && !needsBox )
            return Common::MakeSuccess( Result{} );
        if ( !data.SourceHash ) // a stated hash is the import's own word and is left as it is
        {
            const auto hash = Desert::Assets::HashMeshSourceFile( source );
            if ( !hash )
                return Common::MakeError<Result>( hash.GetError() );
            data.SourceHash = hash.GetValue();
        }
        if ( needsBox )
        {
            const auto box = SkinnedMeshBox( skinnedMesh );
            if ( !box )
                return Common::MakeError<Result>( box.GetError() );
            data.Bounds = box.GetValue();
        }
        auto text = Ser::WriteImportRecord( data, kind );
        if ( !text )
            return Common::MakeFormattedError<Result>(
                 "'{}': {}", Common::Content::ImportRecordPathFor( source ).string(), text.GetError() );
        return Common::MakeSuccess( Result{ text.ExtractValue() } );
    }
} // namespace Desert::Migration
