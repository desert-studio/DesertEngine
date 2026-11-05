#include "PrimitiveMeshFactory.hpp"

#include <Engine/Assets/MeshCardRepresentation.hpp>
#include <Engine/Assets/MeshDistanceField.hpp>
#include <Engine/Geometry/ShapeGenerators.hpp>

#include <array>
#include <atomic>
#include <format>
#include <mutex>
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

    // One shared mesh per primitive type, created on first use and kept alive until ReleaseShared().
    //
    // TWO THREADS MAY ASK AT ONCE (UE: a cache that parallel tasks share is guarded, or publishes an immutable
    // value lock-free). MeshECSSystem ticks on a worker beside the other parallel systems (CanRunParallel), and
    // the scene's raycasts, physics and the editor ask from their own threads. A first create of one type from
    // two callers raced on a map, and one shared_ptr died under the other's raw pointer. Now the steady path is
    // one acquire load per call, no lock per entity per frame. The first create of a type and ReleaseShared take
    // the mutex. A slot is written once, under the mutex, and published by the release store of its flag only
    // once its mesh is complete (distance field and cards attached); after that it is only read, until
    // ReleaseShared. A type with no mesh here (Terrain, LightCube) resolves to null once and never locks again.
    //
    // At file scope rather than inside GetShared() so that ReleaseShared() can reach it: as a function-local
    // static it would only ever have been destroyed at __cxa_finalize, after the device it holds buffers from.
    namespace
    {
        constexpr size_t kPrimitiveTypeCount = static_cast<size_t>( PrimitiveType::Count );

        std::mutex s_SharedCreateMutex;
        std::array<std::shared_ptr<DynamicMesh>, kPrimitiveTypeCount>
                                                           s_SharedPrimitives;  // written under the mutex
        std::array<std::atomic<bool>, kPrimitiveTypeCount> s_SharedPublished{}; // release after the write
    } // namespace

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
        auto slot = std::make_shared<Assets::MeshDistanceFieldSlot>( Assets::MostlyTwoSided( input.GetValue() ) );
        mesh.SetDistanceField( slot );
        // The surface-cache cards too, built in the same job from the same triangles (no asset, no DDC key).
        auto cards = std::make_shared<Assets::MeshCardsSlot>();
        mesh.SetCards( cards );
        Assets::EnqueueMeshDistanceField(
             std::move( slot ), std::format( "primitive '{}'", PrimitiveTypeName( type ) ),
             [in = std::make_shared<Assets::MeshDistanceFieldBuildInput>( input.ExtractValue() ), cards,
              type]( bool& built, const Assets::MeshDistanceFieldCancel& cancel )
                  -> Common::ResultStr<std::shared_ptr<const Assets::MeshDistanceFieldData>>
             {
                 built = true;
                 if ( auto cardBuild = Assets::BuildMeshCards( *in, Assets::MeshCardsBuildSettings{}, cancel );
                      cardBuild.IsSuccess() )
                     cards->Publish(
                          std::make_shared<const Assets::MeshCardsBuildData>( cardBuild.ExtractValue() ) );
                 else if ( cancel.IsCancelled() )
                     return Common::MakeError<std::shared_ptr<const Assets::MeshDistanceFieldData>>(
                          std::string( Assets::kMeshDistanceFieldCancelled ) );
                 else
                     LOG_ERROR( "[PrimitiveMeshFactory] '{}' has no surface-cache cards: {}",
                                PrimitiveTypeName( type ), cardBuild.GetError() );
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
        const auto index = static_cast<size_t>( type );
        if ( index >= kPrimitiveTypeCount )
            return nullptr;
        if ( s_SharedPublished[index].load( std::memory_order_acquire ) )
            return s_SharedPrimitives[index].get();

        // The first ask of this type: one creator, the others wait here and read what it published.
        const std::lock_guard lock( s_SharedCreateMutex );
        if ( !s_SharedPublished[index].load( std::memory_order_relaxed ) )
        {
            // CPU ONLY: the first caller may be a system's tick that runs past the previous update's views, so the
            // pool upload is the fence's, like every mesh's. The constructor raised the pending flag, and the
            // first draw's Prepare (SceneRenderer::PrepareMesh, Scene::OnUpdate's intake) uploads it once, with no
            // view job in flight. The pages it lands in are the pool's (GeometryPool claims them for
            // AssetService), so no view's ledger scope can take them (RT-LEDGER-4). A caller that only reads the
            // CPU arrays (bounds, collision, the spline mesh, the thumbnail framing) never needed the upload.
            std::shared_ptr<DynamicMesh> mesh = Create( type );
            if ( mesh )
                AttachDistanceField( type, *mesh );
            s_SharedPrimitives[index] = std::move( mesh );
            s_SharedPublished[index].store( true, std::memory_order_release );
        }
        return s_SharedPrimitives[index].get();
    }

    void PrimitiveMeshFactory::ReleaseShared()
    {
        // Renderer::Shutdown: no system ticks any more, so no caller still holds a pointer published here. The
        // meshes die outside the lock.
        std::array<std::shared_ptr<DynamicMesh>, kPrimitiveTypeCount> released;
        {
            const std::lock_guard lock( s_SharedCreateMutex );
            for ( size_t index = 0; index < kPrimitiveTypeCount; ++index )
            {
                s_SharedPublished[index].store( false, std::memory_order_relaxed );
                released[index] = std::move( s_SharedPrimitives[index] );
            }
        }
    }
} // namespace Desert::Geometry
