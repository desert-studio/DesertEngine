// Ported from UE 5.8 ModelingComponents/Private/ModelingToolTargetUtil.cpp:302-335, adapted: see the header.
#include "ModelingToolTarget.hpp"

#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Geometry/DynamicMeshAsset.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <utility>

namespace Desert::Editor
{
    using MeshPtr = std::shared_ptr<const Geometry::DynamicMesh3>;

    Common::ResultStr<MeshPtr> LiftStaticMeshBytes( std::string_view bytes, std::string_view whatFor )
    {
        auto data = Assets::Serialization::ReadMeshAssetData( bytes, whatFor );
        if ( !data.IsSuccess() )
            return Common::MakeError<MeshPtr>( data.GetError() );
        auto mesh = Geometry::DynamicMeshFromMeshAssetData( data.GetValue() );
        if ( !mesh.IsSuccess() )
            return Common::MakeFormattedError<MeshPtr>( "{}: {}", whatFor, mesh.GetError() );
        return Common::MakeSuccess( MeshPtr( std::make_shared<Geometry::DynamicMesh3>( mesh.ExtractValue() ) ) );
    }

    Common::ResultStr<ToolTargetMesh> GetToolTargetMeshAt( const MeshPtr&               editable,
                                                           const std::filesystem::path& assetFile )
    {
        if ( editable )
            return Common::MakeSuccess( ToolTargetMesh{ editable, editable } );
        if ( assetFile.empty() )
            return Common::MakeError<ToolTargetMesh>(
                 "the entity has no editable mesh and no static mesh asset (a primitive or an unset mesh)" );
        std::error_code ec;
        const auto      stamp = std::filesystem::last_write_time( assetFile, ec );
        if ( ec )
            return Common::MakeFormattedError<ToolTargetMesh>( "static mesh {}: {}", assetFile.string(),
                                                               ec.message() );

        // Keyed by file and write time: the same file lifts to the same object until it is rewritten.
        static std::map<std::string, std::pair<std::filesystem::file_time_type, MeshPtr>> lifted;
        auto& slot = lifted[assetFile.string()];
        if ( !slot.second || slot.first != stamp )
        {
            // THE LOADER'S READ, NOT THE FILE'S BYTES: a .stmesh is a MeshSourceAsset (AF4d) and what the entity
            // draws is its render form from the DDC (StaticMeshAsset::LoadFromFile). Lifting that same form keeps
            // the target in the space the viewport picks in (import scale / up axis applied), and a file that is
            // not a source asset is refused here by name exactly as the loader refuses it.
            const auto raw = Assets::LoadMeshPlatformData( assetFile );
            if ( !raw.IsSuccess() )
                return Common::MakeError<ToolTargetMesh>( raw.GetError() );
            auto mesh = LiftStaticMeshBytes( raw.GetValue(), assetFile.string() );
            if ( !mesh.IsSuccess() )
                return Common::MakeError<ToolTargetMesh>( mesh.GetError() );
            slot = { stamp, mesh.ExtractValue() };
        }
        return Common::MakeSuccess( ToolTargetMesh{ slot.second, nullptr } );
    }

    MeshRestore PlanMeshRestore( const MeshPtr& current, const MeshPtr& state )
    {
        if ( !state )
            return current ? MeshRestore::Clear : MeshRestore::Unchanged;
        return state == current ? MeshRestore::Unchanged : MeshRestore::Set;
    }
} // namespace Desert::Editor
