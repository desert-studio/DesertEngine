#include "EditMeshBridge.hpp"

// The whole file is removed by P8b; the card that removes each function is named in EditMeshBridge.hpp.

#include "Engine/Geometry/DynamicMeshSerialization.hpp"
#include "Engine/Geometry/EditMeshSerialization.hpp"
#include "Engine/ECS/EditableMesh.hpp"

#include <mutex>
#include <unordered_map>

namespace Desert::Geometry::Bridge
{
    namespace
    {
        // Keyed by address, confirmed by the weak pointer: an address reused by a later mesh fails the
        // lock() comparison and is a miss, never another mesh's view.
        struct Entry
        {
            std::weak_ptr<const FDynamicMesh3> Mesh;
            std::shared_ptr<const EditMesh>    View;
        };

        std::mutex                                           g_Mutex;
        std::unordered_map<const FDynamicMesh3*, Entry>      g_Views;

        void RememberLocked( const std::shared_ptr<const FDynamicMesh3>& mesh, std::shared_ptr<const EditMesh> view )
        {
            std::erase_if( g_Views, []( const auto& kv ) { return kv.second.Mesh.expired(); } );
            g_Views[mesh.get()] = Entry{ mesh, std::move( view ) };
        }
    } // namespace

    Common::ResultStr<std::shared_ptr<const EditMesh>> EditMeshView( const std::shared_ptr<const FDynamicMesh3>& mesh )
    {
        if ( !mesh )
            return Common::MakeError<std::shared_ptr<const EditMesh>>( "EditMeshView: no mesh" );
        std::scoped_lock lock( g_Mutex );
        if ( auto it = g_Views.find( mesh.get() ); it != g_Views.end() && it->second.Mesh.lock() == mesh )
            return Common::MakeSuccess( it->second.View );

        if ( !mesh->IsCompact() )
            return Common::MakeFormattedError<std::shared_ptr<const EditMesh>>(
                 "EditMeshView: the mesh has {} of {} vertex IDs and {} of {} triangle IDs live; the EditMesh "
                 "view numbers densely, so its IDs would not name this mesh's elements",
                 mesh->VertexCount(), mesh->MaxVertexID(), mesh->TriangleCount(), mesh->MaxTriangleID() );
        auto read = FromSerialized( ToSerialized( *mesh ) );
        if ( !read.IsSuccess() )
            return Common::MakeError<std::shared_ptr<const EditMesh>>( "EditMeshView: " + read.GetError() );
        auto view = std::make_shared<const EditMesh>( read.ExtractValue() );
        RememberLocked( mesh, view );
        return Common::MakeSuccess( std::move( view ) );
    }

    Common::ResultStr<std::shared_ptr<const FDynamicMesh3>> FromEditMesh( EditMesh mesh, ElementSelection* selection )
    {
        const CompactMaps maps = mesh.Compact();
        if ( selection )
            (void)selection->Remap( maps );
        auto converted = DynamicMeshFromSerialized( ToSerialized( mesh ), "EditMeshBridge" );
        if ( !converted.IsSuccess() )
            return Common::MakeError<std::shared_ptr<const FDynamicMesh3>>( "FromEditMesh: " + converted.GetError() );
        auto out = std::make_shared<const FDynamicMesh3>( converted.ExtractValue() );
        std::scoped_lock lock( g_Mutex );
        RememberLocked( out, std::make_shared<const EditMesh>( std::move( mesh ) ) );
        return Common::MakeSuccess( std::move( out ) );
    }

    Common::BoolResultStr SetEditableMeshFromEditMesh( ECS::StaticMeshComponent& component, EditMesh mesh )
    {
        auto converted = FromEditMesh( std::move( mesh ) );
        if ( !converted.IsSuccess() )
            return Common::MakeError<bool>( converted.GetError() );
        return ECS::SetEditableMesh( component, converted.ExtractValue() );
    }
} // namespace Desert::Geometry::Bridge
