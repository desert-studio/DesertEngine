// Ported from UE 5.8 ModelingComponents/Private/ModelingToolTargetUtil.cpp:302-335, adapted: see the header.
#include "ModelingToolTarget.hpp"

#include <Common/Content/ImportRecord.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Geometry/DynamicMeshAsset.hpp>

#include <filesystem>
#include <map>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace Desert::Editor
{
    using MeshPtr = std::shared_ptr<const Geometry::DynamicMesh3>;

    Common::ResultStr<MeshPtr> LiftStaticMeshBytes( std::string_view bytes, std::string_view whatFor )
    {
        auto data = Assets::Serialization::ReadMeshAssetData( bytes, whatFor );
        if ( !data.IsSuccess() )
            return Common::MakeError<MeshPtr>( data.GetError() );
        auto lifted = Geometry::DynamicMeshFromMeshAssetData( data.GetValue() );
        if ( !lifted.IsSuccess() )
            return Common::MakeFormattedError<MeshPtr>( "{}: {}", whatFor, lifted.GetError() );
        Geometry::ImportedDynamicMesh imported = lifted.ExtractValue();
        // The skipped faces are gone from the mesh the tool edits, so a commit writes the file without them:
        // said out loud here, with the same counts the importer reports for the EditMesh core.
        if ( imported.DroppedDegenerate != 0 || imported.DroppedDuplicate != 0 || imported.DetachedTriangles != 0 )
            LOG_WARN( "[Modeling] '{}': skipped {} degenerate and {} duplicate face(s), and detached {} "
                      "non-manifold face(s) onto their own vertices",
                      whatFor, imported.DroppedDegenerate, imported.DroppedDuplicate, imported.DetachedTriangles );
        return Common::MakeSuccess(
             MeshPtr( std::make_shared<Geometry::DynamicMesh3>( std::move( imported.Mesh ) ) ) );
    }

    Common::ResultStr<ToolTargetMesh> GetToolTargetMeshAt( const MeshPtr&               editable,
                                                           const std::filesystem::path& assetFile )
    {
        if ( editable )
            return Common::MakeSuccess( ToolTargetMesh{ editable, editable } );
        if ( assetFile.empty() )
            return Common::MakeError<ToolTargetMesh>(
                 "the entity has no editable mesh and no static mesh asset (a primitive or an unset mesh)" );
        // Sampled before the stat, so a write that lands after it is racy by construction (IsRacyWriteTime).
        const auto      readBegan = std::filesystem::file_time_type::clock::now();
        // The file whose bytes the lift is a function of: the asset's own, or - for an import since AF4h, which
        // has no file at the asset path (its envelope is derived from the source's bytes into the DDC) - the
        // raw source beside it. Stating the asset path alone refused every imported mesh by name (P9b).
        std::filesystem::path stamped = assetFile;
        std::error_code       ec;
        if ( !std::filesystem::exists( assetFile, ec ) )
            if ( auto source = Common::Content::MeshSourceBeside( assetFile ) )
                stamped = std::move( *source );
        const auto stamp = std::filesystem::last_write_time( stamped, ec );
        if ( ec )
            return Common::MakeFormattedError<ToolTargetMesh>( "static mesh {}: {}", stamped.string(),
                                                               ec.message() );

        // The same file lifts to the same object until its CONTENT changes: the element selection tracks by
        // identity. The write time is only a shortcut past the read, and only once it has settled — two
        // same-size writes inside one file-system tick (~15.6 ms on NTFS) leave it unchanged, and keying on
        // it alone served the old mesh after a rewrite (the class ThumbnailFreshness met on Windows CI).
        struct Lift
        {
            std::filesystem::file_time_type Stamp{};
            bool                            Racy        = true;
            std::size_t                     ContentHash = 0;
            MeshPtr                         Mesh;
        };
        static std::map<std::string, Lift> lifted;
        Lift&                              slot = lifted[assetFile.string()];
        if ( slot.Mesh && !slot.Racy && slot.Stamp == stamp )
            return Common::MakeSuccess( ToolTargetMesh{ slot.Mesh, nullptr } );

        // THE LOADER'S READ, NOT THE FILE'S BYTES: a .stmesh is a MeshSourceAsset (AF4d) and what the entity
        // draws is its render form from the DDC (StaticMeshAsset::LoadFromFile). Lifting that same form keeps
        // the target in the space the viewport picks in (import scale / up axis applied), and a file that is
        // not a source asset is refused here by name exactly as the loader refuses it.
        const auto raw = Assets::LoadMeshPlatformData( assetFile );
        if ( !raw.IsSuccess() )
            return Common::MakeError<ToolTargetMesh>( raw.GetError() );
        const std::size_t contentHash = std::hash<std::string_view>{}( raw.GetValue() );
        if ( !slot.Mesh || slot.ContentHash != contentHash )
        {
            auto mesh = LiftStaticMeshBytes( raw.GetValue(), assetFile.string() );
            if ( !mesh.IsSuccess() )
                return Common::MakeError<ToolTargetMesh>( mesh.GetError() );
            slot.Mesh        = mesh.ExtractValue();
            slot.ContentHash = contentHash;
        }
        slot.Stamp = stamp;
        slot.Racy  = Common::Utils::IsRacyWriteTime( stamp, readBegan );
        return Common::MakeSuccess( ToolTargetMesh{ slot.Mesh, nullptr } );
    }

    MeshRestore PlanMeshRestore( const MeshPtr& current, const MeshPtr& state )
    {
        if ( !state )
            return current ? MeshRestore::Clear : MeshRestore::Unchanged;
        return state == current ? MeshRestore::Unchanged : MeshRestore::Set;
    }
} // namespace Desert::Editor
