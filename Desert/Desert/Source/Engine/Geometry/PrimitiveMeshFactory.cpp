#include "PrimitiveMeshFactory.hpp"

#include <Engine/Assets/MeshDistanceField.hpp>
#include <Engine/Geometry/ShapeGenerators.hpp>

#include <format>
#include <unordered_map>
#include <vector>

namespace Desert::Geometry
{
    std::shared_ptr<DynamicMesh> PrimitiveMeshFactory::Create( PrimitiveType type )
    {
        std::optional<ShapeMesh> shape = MakePrimitive( type );
        const auto               box   = PrimitiveBounds( type );
        if ( !shape.has_value() || !box.has_value() )
            return nullptr;

        // The submesh box is PrimitiveBounds, the statement the world partitioner reads as well; the
        // ShapeGenerators suite holds it to these vertices.
        const std::vector<Submesh> submeshes = {
             { PrimitiveTypeName( type ), 0, static_cast<uint32_t>( shape->Vertices.size() ), 0,
               static_cast<uint32_t>( shape->Indices.size() * 3 ), glm::mat4( 1.0f ), box.value() } };
        return std::make_shared<DynamicMesh>( shape->Vertices, shape->Indices, submeshes );
    }

    // One shared, GPU-ready mesh per primitive type, created lazily on first use and kept alive until
    // ReleaseShared(). Single-threaded engine, so no synchronization needed. At file scope rather than
    // inside GetShared() so that ReleaseShared() can reach it: as a function-local static it would only
    // ever have been destroyed at __cxa_finalize, which is after the device it holds buffers from.
    static std::unordered_map<PrimitiveType, std::shared_ptr<DynamicMesh>> s_SharedPrimitives;

    // THE SHARED PRIMITIVE CARRIES A DISTANCE FIELD LIKE ANY STATIC MESH (UE: the BasicShapes are static mesh
    // assets, so their fields are built like every other mesh's). Without it every primitive is invisible to
    // the global distance field and every trace through it - a Cornell box built of cubes was an empty volume.
    // Built in process from the same generator vertices the GPU draws: the geometry is code, not a file, so
    // there is no asset to key a DDC entry by and nothing a packaged game could have cooked; the build is
    // deterministic and runs once per shape per process, on a worker through the same asynchronous queue as
    // every mesh's (the primitive draws at once and joins the distance-field scene when its field lands). A
    // forked per-entity copy (the mesh editor) is procedural and has none, like UE's dynamic meshes. The Plane
    // bounds no volume: its triangles are two-sided, so the field around it is the distance to its surface with
    // no false interior below.
    static void AttachDistanceField( const PrimitiveType type, DynamicMesh& mesh )
    {
        const std::vector<uint8_t> twoSided( mesh.GetSubmeshes().size(), type == PrimitiveType::Plane ? 1 : 0 );
        auto input = Assets::MeshDistanceFieldInputOf( mesh.GetVertices(), mesh.GetIndices(), mesh.GetSubmeshes(),
                                                       twoSided );
        if ( !input.IsSuccess() )
        {
            LOG_ERROR( "[PrimitiveMeshFactory] '{}' has no distance field: {}", PrimitiveTypeName( type ),
                       input.GetError() );
            return;
        }
        auto slot = std::make_shared<Assets::MeshDistanceFieldSlot>();
        mesh.SetDistanceField( slot );
        Assets::EnqueueMeshDistanceField(
             std::move( slot ), std::format( "primitive '{}'", PrimitiveTypeName( type ) ),
             [in = std::make_shared<Assets::MeshDistanceFieldBuildInput>( input.ExtractValue() )](
                  bool& built, const Assets::MeshDistanceFieldCancel& cancel )
                  -> Common::ResultStr<std::shared_ptr<const Assets::MeshDistanceFieldData>>
             {
                 built      = true;
                 auto field =
                      Assets::BuildMeshDistanceField( *in, Assets::MeshDistanceFieldBuildSettings{}, cancel );
                 if ( !field.IsSuccess() )
                     return Common::MakeError<std::shared_ptr<const Assets::MeshDistanceFieldData>>(
                          field.GetError() );
                 return Common::MakeSuccess<std::shared_ptr<const Assets::MeshDistanceFieldData>>(
                      std::make_shared<const Assets::MeshDistanceFieldData>( field.ExtractValue() ) );
             } );
    }

    DynamicMesh* PrimitiveMeshFactory::GetShared( PrimitiveType type )
    {
        auto& mesh = s_SharedPrimitives[type];
        if ( !mesh )
        {
            // Shared by every scene and view until ReleaseShared(), so the device's, not the view's that first
            // drew one (RT-LEDGER-4: a view scope open here would report these buffers as its leak on close).
            const Graphic::ResourceAttributionScope owned( Graphic::ResourceOwner::Device );
            mesh = Create( type );
            if ( mesh )
            {
                mesh->Invalidate(); // build the GPU vertex/index buffers once
                AttachDistanceField( type, *mesh );
            }
        }
        return mesh.get();
    }

    void PrimitiveMeshFactory::ReleaseShared()
    {
        s_SharedPrimitives.clear();
    }
} // namespace Desert::Geometry
