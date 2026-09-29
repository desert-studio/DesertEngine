#include "ImportedMeshAsset.hpp"

#include <Engine/Assets/Serialization/ImportRecord.hpp>

#include "CookPaths.hpp"
#include "MaterialAdoption.hpp"
#include "NodeMeshSplit.hpp"

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Logger.hpp>
#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Geometry/EditMeshBridge.hpp>
#include <Editor/Widgets/ThumbnailFreshness.hpp>
#include <array>
#include <mutex>
#include <unordered_map>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <map>
#include <optional>
#include <vector>

namespace Desert::Editor
{
    namespace Ser = Assets::Serialization;

    std::pair<std::string, int> ParseSourceModelLOD( const std::string& name )
    {
        std::string lower = name;
        std::ranges::transform( lower, lower.begin(),
                                []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
        const auto pos = lower.rfind( "_lod" );
        if ( pos == std::string::npos )
            return { name, 0 };
        const std::string digits = name.substr( pos + 4 );
        if ( digits.empty() || digits.size() > 3 ||
             !std::ranges::all_of( digits, []( unsigned char c ) { return std::isdigit( c ) != 0; } ) )
            return { name, 0 };
        return { name.substr( 0, pos ), std::stoi( digits ) };
    }

    Common::ResultStr<ImportedMeshSource> MeshSourceFromImport( const Ser::MeshAssetData&                 imported,
                                                                std::span<const Assets::MeshMaterialSlot> named,
                                                                const std::string&                        name )
    {
        using Result = ImportedMeshSource;
        if ( imported.IsSkinned )
            return Common::MakeFormattedError<Result>( "'{}' is skinned; its source is AF4f's", name );
        if ( !imported.MorphTargets.empty() )
            return Common::MakeFormattedError<Result>(
                 "'{}' has {} morph target(s), and a static mesh source has no morph layer to keep them in", name,
                 imported.MorphTargets.size() );
        if ( imported.Submeshes.empty() )
            return Common::MakeFormattedError<Result>( "'{}' has no submesh", name );

        std::map<int, std::vector<size_t>> levels;
        for ( size_t s = 0; s < imported.Submeshes.size(); ++s )
            levels[ParseSourceModelLOD( imported.Submeshes[s].Name ).second].push_back( s );
        int expected = 0;
        for ( const auto& [level, submeshes] : levels )
            if ( level != expected++ )
                return Common::MakeFormattedError<Result>( "'{}' has submeshes for LOD{} but none for LOD{}; "
                                                           "authored LODs must be numbered without gaps",
                                                           name, level, expected - 1 );

        Result     out;
        auto&      slots  = out.Source.MaterialSlots;
        const auto slotOf = [&]( const Ser::SubmeshData& sub )
        {
            for ( size_t i = 0; i < slots.size(); ++i )
                if ( slots[i].Material == sub.MaterialGuid )
                    return static_cast<int>( i );
            const auto known = std::ranges::find_if( named, [&]( const auto& slot )
                                                     { return slot.Material == sub.MaterialGuid; } );
            slots.push_back( { known != named.end() ? known->Name : ParseSourceModelLOD( sub.Name ).first,
                               sub.MaterialGuid } );
            return static_cast<int>( slots.size() - 1 );
        };

        const bool groupsPerFace = imported.PolyGroups.size() == imported.Indices.size();
        // The optional streams (MeshBinary v4) are one per vertex or absent; a stream of any other length is
        // not sliced by guess but refused.
        const size_t vertexTotal = imported.StaticVertices.size();
        if ( ( !imported.Colors.empty() && imported.Colors.size() != vertexTotal ) ||
             ( !imported.UV1.empty() && imported.UV1.size() != vertexTotal ) )
            return Common::MakeFormattedError<Result>(
                 "'{}' carries {} colours and {} UV1 entries for {} vertices", name, imported.Colors.size(),
                 imported.UV1.size(), vertexTotal );
        for ( const auto& [level, submeshes] : levels )
        {
            Ser::MeshAssetData part;
            std::vector<int>   slotOfSubmesh;
            for ( const size_t s : submeshes )
            {
                const Ser::SubmeshData& sub       = imported.Submeshes[s];
                const size_t            firstFace = sub.IndexOffset / 3;
                if ( sub.VertexOffset + sub.VertexCount > imported.StaticVertices.size() ||
                     firstFace + sub.IndexCount / 3 > imported.Indices.size() )
                    return Common::MakeFormattedError<Result>( "'{}' submesh '{}' reaches past the mesh's arrays",
                                                               name, sub.Name );
                Ser::SubmeshData copy = sub;
                copy.VertexOffset     = static_cast<uint32_t>( part.StaticVertices.size() );
                copy.IndexOffset      = static_cast<uint32_t>( part.Indices.size() * 3 );
                copy.LODs.clear();
                // Iterator offsets are signed; the bounds were checked above, so these cannot overflow.
                const auto vertexBegin = static_cast<std::ptrdiff_t>( sub.VertexOffset );
                const auto vertexEnd   = static_cast<std::ptrdiff_t>( sub.VertexOffset ) +
                                       static_cast<std::ptrdiff_t>( sub.VertexCount );
                const auto faceBegin = static_cast<std::ptrdiff_t>( firstFace );
                const auto faceEnd   = static_cast<std::ptrdiff_t>( firstFace + sub.IndexCount / 3 );
                part.StaticVertices.insert( part.StaticVertices.end(),
                                            imported.StaticVertices.begin() + vertexBegin,
                                            imported.StaticVertices.begin() + vertexEnd );
                if ( !imported.Colors.empty() )
                    part.Colors.insert( part.Colors.end(), imported.Colors.begin() + vertexBegin,
                                        imported.Colors.begin() + vertexEnd );
                if ( !imported.UV1.empty() )
                    part.UV1.insert( part.UV1.end(), imported.UV1.begin() + vertexBegin,
                                     imported.UV1.begin() + vertexEnd );
                part.Indices.insert( part.Indices.end(), imported.Indices.begin() + faceBegin,
                                     imported.Indices.begin() + faceEnd );
                if ( groupsPerFace )
                    part.PolyGroups.insert( part.PolyGroups.end(), imported.PolyGroups.begin() + faceBegin,
                                            imported.PolyGroups.begin() + faceEnd );
                part.Submeshes.push_back( std::move( copy ) );
                slotOfSubmesh.push_back( slotOf( sub ) );
            }

            auto lifted = Geometry::Bridge::EditMeshFromMeshAssetData( part );
            if ( !lifted.IsSuccess() )
                return Common::MakeFormattedError<Result>( "'{}' LOD{} cannot become a source model: {}", name,
                                                           level, lifted.GetError() );
            Geometry::ImportedEditMesh welded = lifted.ExtractValue();
            out.DroppedDegenerate += welded.DroppedDegenerate;
            out.DroppedDuplicate += welded.DroppedDuplicate;
            out.DetachedTriangles += welded.DetachedTriangles;
            Geometry::EditMesh& mesh = welded.Mesh;
            // FromMeshAssetData numbers materials by submesh; the source numbers them by the shared slot table.
            for ( int t = 0; t < mesh.MaxTriangleId(); ++t )
                if ( mesh.IsTriangle( t ) )
                    mesh.Attributes().SetMaterialId(
                         t, slotOfSubmesh[static_cast<size_t>( mesh.Attributes().GetMaterialId( t ) )] );
            out.Source.Models.push_back( { Geometry::Bridge::SavedFormFromEditMesh( mesh ) } );
        }
        return Common::MakeSuccess( std::move( out ) );
    }

    std::string SkippedFacesWarning( const ImportedMeshSource& source, const std::string& name )
    {
        if ( source.DroppedDegenerate == 0 && source.DroppedDuplicate == 0 && source.DetachedTriangles == 0 )
            return {};
        return fmt::format( "[Import] '{}': skipped {} degenerate and {} duplicate face(s), and detached {} "
                            "non-manifold face(s) onto their own vertices",
                            name, source.DroppedDegenerate, source.DroppedDuplicate, source.DetachedTriangles );
    }

    std::optional<std::filesystem::path> RemoveBesideSourceFile( const std::filesystem::path& source )
    {
        const std::filesystem::path asset = CookPaths::MeshAsset( source );
        // Asked before the removal: afterwards the file that answers it is gone.
        const bool      edited = Assets::IsEditedImportedMesh( asset );
        std::error_code ec;
        if ( !std::filesystem::remove( asset, ec ) || !edited )
            return std::nullopt;
        LOG_WARN( "[Import] re-importing '{}' OVERWROTE the modeling edits saved in '{}': the asset is the "
                  "source file's import again (the edit is gone; undo does not bring it back)",
                  source.generic_string(), asset.generic_string() );
        return asset;
    }

    bool ImportedMeshAssetIsFresh( const std::filesystem::path& source )
    {
        const auto hash = Assets::HashMeshSourceFile( source );
        if ( !hash )
            return false;
        // Content-addressed: an entry under this exact byte content's key already existing IS "fresh" -
        // there is nothing else to compare, unlike the old beside-file check (provenance, stored source
        // hash) that existed only because the file's own claims could otherwise drift from the truth.
        return Common::DDC::Get( Assets::kMeshSourceDeriver, Assets::MeshSourceDerivedDataKey( hash.GetValue() ) )
             .has_value();
    }

    bool ImportedMaterialsPresent( const std::filesystem::path& source )
    {
        const auto hash = Assets::HashMeshSourceFile( source );
        if ( !hash )
            return false;
        const auto blob =
             Common::DDC::Get( Assets::kMeshSourceDeriver, Assets::MeshSourceDerivedDataKey( hash.GetValue() ) );
        if ( !blob )
            return false;
        const auto asset = Assets::DecodeMeshSourceAsset(
             std::span<const std::byte>( reinterpret_cast<const std::byte*>( blob->data() ), blob->size() ) );
        if ( !asset )
        {
            LOG_ERROR( "[Import] '{}': the cached import envelope does not decode ({}), so it is imported again",
                       source.generic_string(), asset.GetError() );
            return false;
        }
        bool present = true;
        for ( const auto& slot : asset.GetValue().Source.MaterialSlots )
        {
            const std::filesystem::path path = MaterialAdoption::MaterialAssetPath( source, slot.Name );
            std::error_code             ec;
            if ( std::filesystem::exists( path, ec ) )
                continue;
            LOG_WARN( "[Import] '{}': material '{}' is missing ('{}' is not on disk), so the source is imported "
                      "again to write it",
                      source.generic_string(), slot.Name, path.generic_string() );
            present = false;
        }
        return present;
    }

    namespace
    {
        // A SPLIT IMPORT'S "UP TO DATE" (THM1j): every node mesh the record names is on disk, states the
        // source's current bytes, and every material it slots has its .demat. There is no combined envelope to
        // ask: a split import writes none. A deleted node mesh re-imports the source, as a deleted .demat does.
        bool SplitImportIsCurrent( const std::filesystem::path& source, const std::vector<std::string>& nodes )
        {
            const auto hash = Assets::HashMeshSourceFile( source );
            if ( !hash )
                return false;
            for ( const std::string& node : nodes )
            {
                const std::filesystem::path path  = NodeMeshAssetPath( source, node );
                const auto                  asset = Assets::LoadMeshSourceAsset( path );
                if ( !asset )
                {
                    LOG_WARN( "[Import] '{}': node mesh '{}' does not load ({}), so the source is imported again",
                              source.generic_string(), path.generic_string(), asset.GetError() );
                    return false;
                }
                if ( asset.GetValue().Import.SourceHash != hash.GetValue() )
                    return false;
                for ( const auto& slot : asset.GetValue().Source.MaterialSlots )
                {
                    std::error_code ec;
                    if ( !std::filesystem::exists( MaterialAdoption::MaterialAssetPath( source, slot.Name ), ec ) )
                        return false;
                }
            }
            return true;
        }
    } // namespace

    bool ImportedMeshAssetIsCurrent( const std::filesystem::path& source )
    {
        if ( const auto record = Assets::Serialization::ReadImportRecord( source );
             record && record.GetValue() && record.GetValue()->Nodes )
            return SplitImportIsCurrent( source, *record.GetValue()->Nodes );
        return ImportedMeshAssetIsFresh( source ) && ImportedMaterialsPresent( source );
    }

    bool StaticMeshCookAvailable( const std::filesystem::path& cooked, const std::filesystem::path& source )
    {
        std::error_code ec;
        return std::filesystem::exists( cooked, ec ) || ImportedMeshAssetIsFresh( source );
    }

    Common::ResultStr<std::optional<std::filesystem::path>>
    MeshThumbnailHome( const std::filesystem::path& meshFile )
    {
        using Home = std::optional<std::filesystem::path>;
        std::error_code ec;
        if ( std::filesystem::is_regular_file( Common::Content::ImportRecordPathFor( meshFile ), ec ) )
            return Common::MakeSuccess( Home{ meshFile } );

        // A node mesh: `<source stem>_<node>.stmesh` beside its source. Only the records whose source stem
        // prefixes this name can have written it; the record's own `Nodes` decides. Remembered: the browser asks
        // per visible tile per frame, and the answer changes only when that record goes away.
        static std::mutex                                             mutex;
        static std::unordered_map<std::string, std::filesystem::path> found;
        const std::string                                             key = meshFile.generic_string();
        {
            const std::lock_guard lock( mutex );
            if ( const auto it = found.find( key ); it != found.end() )
            {
                if ( std::filesystem::is_regular_file( Common::Content::ImportRecordPathFor( it->second ), ec ) )
                    return Common::MakeSuccess( Home{ it->second } );
                found.erase( it );
            }
        }
        const std::string stem = meshFile.stem().string();
        for ( const auto& entry : std::filesystem::directory_iterator( meshFile.parent_path(), ec ) )
        {
            if ( !Common::Content::IsImportRecord( entry.path() ) )
                continue;
            const std::filesystem::path source = entry.path().parent_path() / entry.path().stem();
            const std::string           prefix = source.stem().string() + "_";
            if ( stem.size() <= prefix.size() || stem.compare( 0, prefix.size(), prefix ) != 0 )
                continue;
            const auto record = Ser::ReadImportRecord( source );
            if ( !record )
                return Common::MakeError<Home>( record.GetError() );
            if ( !record.GetValue() || !record.GetValue()->Nodes )
                continue;
            for ( const std::string& node : *record.GetValue()->Nodes )
                if ( NodeMeshAssetPath( source, node ).filename() == meshFile.filename() )
                {
                    const std::lock_guard lock( mutex );
                    found[key] = source;
                    return Common::MakeSuccess( Home{ source } );
                }
        }
        return Common::MakeSuccess( Home{} );
    }

    Common::ResultStr<Assets::ThumbnailOrbit> MeshThumbnailOrbit( const std::filesystem::path& meshFile )
    {
        const auto home = MeshThumbnailHome( meshFile );
        if ( !home )
            return Common::MakeError<Assets::ThumbnailOrbit>( home.GetError() );
        if ( !home.GetValue() )
            return Common::MakeSuccess( Assets::ThumbnailOrbit{} );
        return Ser::ReadImportRecordThumbnail( *home.GetValue(), meshFile.filename().string() );
    }

    Common::BoolResultStr SetMeshThumbnailOrbit( const std::filesystem::path&  meshFile,
                                                 const Assets::ThumbnailOrbit& orbit )
    {
        const auto home = MeshThumbnailHome( meshFile );
        if ( !home )
            return Common::MakeError<bool>( home.GetError() );
        if ( !home.GetValue() )
            return Common::MakeFormattedError<bool>(
                 "'{}' was written by no import, so there is no import record to state its thumbnail orbit",
                 meshFile.string() );
        return Ser::SetImportRecordThumbnail( *home.GetValue(), meshFile.filename().string(), orbit );
    }

    std::optional<uint64_t> MeshThumbnailFreshness( const std::filesystem::path& cooked )
    {
        const std::filesystem::path   meshFile = ThumbnailFreshness::MeshFreshnessSource( cooked );
        const std::optional<uint64_t> bytes    = ThumbnailFreshness::ContentHash( meshFile );
        if ( !bytes )
            return std::nullopt;
        const auto home = MeshThumbnailHome( meshFile );
        if ( !home )
            return std::nullopt;
        if ( !home.GetValue() )
            return bytes; // no record, no orbit but the default
        const std::string name   = meshFile.filename().string();
        const auto        record = Common::Content::ImportRecordPathFor( *home.GetValue() );
        // The orbit's hash, memoised on the record's (size, modtime); 0 stands for "the record states the
        // default".
        const std::optional<uint64_t> info = ThumbnailFreshness::Detail::MemoisedAs(
             record, record.generic_string() + "#thumbnail#" + name,
             [&]( const std::filesystem::path& ) -> std::optional<uint64_t>
             {
                 const auto orbit = Ser::ReadImportRecordThumbnail( *home.GetValue(), name );
                 if ( !orbit )
                     return std::nullopt;
                 if ( orbit.GetValue() == Assets::ThumbnailOrbit{} )
                     return uint64_t{ 0 };
                 const std::array<float, 3> fields{ orbit.GetValue().Pitch, orbit.GetValue().Yaw,
                                                    orbit.GetValue().Zoom };
                 return Common::Utils::PakContentHash( fields.data(), sizeof( fields ) );
             } );
        if ( !info )
            return std::nullopt;
        return ThumbnailFreshness::WithInfo( bytes, *info == 0 ? std::nullopt : info );
    }

    Common::ResultStr<MeshAssetWrite> WriteImportedMeshAsset( const Ser::MeshAssetData&                 imported,
                                                              std::span<const Assets::MeshMaterialSlot> named,
                                                              const std::filesystem::path&              source )
    {
        const auto hash = Assets::HashMeshSourceFile( source );
        if ( !hash )
            return Common::MakeError<MeshAssetWrite>( hash.GetError() );
        const uint64_t key = Assets::MeshSourceDerivedDataKey( hash.GetValue() );

        (void)RemoveBesideSourceFile( source );

        // THE IDENTITY FIRST (FIX8): the record beside the source is written by the first import and read by
        // every later one, so a re-import of changed bytes keeps the GUID every reference holds.
        // The record also states the mesh's box (DIMP 2), rewritten by every re-import that changes it: the
        // registry reads it there without the DDC.
        const auto bounds = Ser::MeshDataBounds( imported );
        if ( !bounds )
            return Common::MakeFormattedError<MeshAssetWrite>( "'{}': the import has no submesh, so no box",
                                                               source.string() );
        const auto identity = Assets::Serialization::EnsureImportRecord( source, *bounds );
        if ( !identity )
            return Common::MakeError<MeshAssetWrite>( identity.GetError() );

        if ( Common::DDC::Get( Assets::kMeshSourceDeriver, key ).has_value() )
            return Common::MakeSuccess( MeshAssetWrite::Unchanged ); // this exact content is already cached

        // THE ENVELOPE STATES THE RECORD'S GUID (FIX8). Its DDC key is the source's bytes, so an edit of the
        // source makes a new envelope - under the same identity, because the record is not rewritten. (AF4h
        // minted a GUID per import here, and every reference by GUID died with the next edit of the .fbx.)
        Assets::MeshSourceAsset asset;
        asset.Kind              = Common::Content::ContentKind::StaticMesh;
        asset.Guid              = identity.GetValue();
        asset.Name              = source.stem().string();
        asset.Import.SourceFile = Common::AssetHandle::StableKeyForPath( source );
        asset.Import.SourceHash = hash.GetValue();
        auto sourceData         = MeshSourceFromImport( imported, named, source.string() );
        if ( !sourceData )
            return Common::MakeError<MeshAssetWrite>( sourceData.GetError() );
        if ( const std::string warning = SkippedFacesWarning( sourceData.GetValue(), source.string() );
             !warning.empty() )
            LOG_WARN( "{}", warning );
        asset.Source = std::move( sourceData.ExtractValue().Source );

        auto bytes = Assets::EncodeMeshSourceAsset( asset );
        if ( !bytes )
            return Common::MakeError<MeshAssetWrite>( bytes.GetError() );
        std::string blob( bytes.GetValue().size(), '\0' );
        std::memcpy( blob.data(), bytes.GetValue().data(), blob.size() );
        if ( auto put = Common::DDC::Put( Assets::kMeshSourceDeriver, key, blob ); !put )
            return Common::MakeFormattedError<MeshAssetWrite>( "'{}': imported source built but not cached: {}",
                                                               source.string(), put.GetError() );
        return Common::MakeSuccess( MeshAssetWrite::Written );
    }

    Common::ResultStr<Assets::AssetGuidRef> PreviewMeshRefFor( const std::filesystem::path& source )
    {
        const auto guid = Assets::Serialization::ReadImportRecordGuid( source );
        if ( !guid )
            return Common::MakeError<Assets::AssetGuidRef>( guid.GetError() );
        std::error_code ec;
        const auto      located = std::filesystem::proximate( source, ec );
        return Common::MakeSuccess( Assets::AssetGuidRef{ Common::Content::AssetGuidToText( guid.GetValue() ),
                                                          ( ec ? source : located ).generic_string() } );
    }
} // namespace Desert::Editor
