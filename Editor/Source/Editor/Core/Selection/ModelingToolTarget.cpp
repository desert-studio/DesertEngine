// Ported from UE 5.8 ModelingComponents/Private/ModelingToolTargetUtil.cpp:302-335, adapted: see the header.
#include "ModelingToolTarget.hpp"

#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Geometry/DynamicMeshAsset.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>

namespace Desert::Editor
{
    using MeshPtr = std::shared_ptr<const Geometry::FDynamicMesh3>;

    Common::ResultStr<MeshPtr> LiftStaticMeshBytes( std::string_view bytes, std::string_view whatFor )
    {
        auto data = Assets::Serialization::ReadMeshAssetData( bytes, whatFor );
        if ( !data.IsSuccess() )
            return Common::MakeError<MeshPtr>( data.GetError() );
        auto mesh = Geometry::DynamicMeshFromMeshAssetData( data.GetValue() );
        if ( !mesh.IsSuccess() )
            return Common::MakeFormattedError<MeshPtr>( "{}: {}", whatFor, mesh.GetError() );
        return Common::MakeSuccess( MeshPtr( std::make_shared<Geometry::FDynamicMesh3>( mesh.ExtractValue() ) ) );
    }

    Common::ResultStr<ToolTargetMesh> GetToolTargetMesh( const ECS::StaticMeshComponent& component )
    {
        if ( component.EditableMesh )
            return Common::MakeSuccess( ToolTargetMesh{ component.EditableMesh, component.EditableMesh } );
        if ( !component.MeshHandle )
            return Common::MakeError<ToolTargetMesh>(
                 "the entity has no editable mesh and no static mesh asset (a primitive or an unset mesh)" );
        const auto* asset = Runtime::ResourceRegistry::GetMeshService()->GetAsset( component.MeshHandle );
        if ( !asset )
            return Common::MakeFormattedError<ToolTargetMesh>( "static mesh asset {} is not loaded",
                                                               static_cast<uint64_t>( component.MeshHandle ) );
        const std::filesystem::path path = asset->GetMetadata().Filepath;
        std::error_code             ec;
        const auto                  stamp = std::filesystem::last_write_time( path, ec );
        if ( ec )
            return Common::MakeFormattedError<ToolTargetMesh>( "static mesh {}: {}", path.string(), ec.message() );

        // Keyed by file and write time: the same file lifts to the same object until it is rewritten.
        static std::map<std::string, std::pair<std::filesystem::file_time_type, MeshPtr>> lifted;
        auto& slot = lifted[path.string()];
        if ( !slot.second || slot.first != stamp )
        {
            std::ifstream in( path, std::ios::binary );
            if ( !in )
                return Common::MakeFormattedError<ToolTargetMesh>( "static mesh {} cannot be opened",
                                                                   path.string() );
            std::ostringstream bytes;
            bytes << in.rdbuf();
            auto mesh = LiftStaticMeshBytes( bytes.str(), path.string() );
            if ( !mesh.IsSuccess() )
                return Common::MakeError<ToolTargetMesh>( mesh.GetError() );
            slot = { stamp, mesh.ExtractValue() };
        }
        return Common::MakeSuccess( ToolTargetMesh{ slot.second, nullptr } );
    }
} // namespace Desert::Editor
