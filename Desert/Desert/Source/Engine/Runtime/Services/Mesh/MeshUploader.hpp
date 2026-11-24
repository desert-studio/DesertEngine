#pragma once

#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Geometry/Mesh.hpp>

#include <Common/Core/ResultStr.hpp>

#include <memory>

namespace Desert::Runtime
{
    /**
     * @brief THE DEVICE HALF OF MeshService, as a seam: turn a PARSED mesh asset into a runtime mesh whose
     *        vertex and index buffers live on the GPU, and upload a procedural one.
     *
     * WHY IT IS AN INTERFACE. Everything else MeshService decides — pending or drawable, which read to
     * request, when a failure is final, what eviction may drop — is bookkeeping over assets and the async
     * loader, and none of it needs a device. It could not be executed by any suite while the GPU calls sat
     * inline in the same file: `MeshService.cpp` was compiled by `Desert.make` alone, and its most expensive
     * defect (a mesh built from an unparsed shell and cached for the life of the process) was guarded by a
     * source-text census instead. Same shape as `IEvictionSink` for AssetEviction: the engine passes the GPU
     * implementation, `Desert/Tests/Engine/MeshServiceResidency` passes a fake.
     *
     * RELEASE HAS NO METHOD, deliberately. A runtime mesh's buffers are released by the mesh's destructor
     * when MeshService drops the last `shared_ptr` (eviction, `Clear`), so "unload" is the service
     * forgetting the mesh — which a suite observes through the fake mesh's destructor, not through a second
     * call that could disagree with it.
     */
    class IMeshUploader
    {
    public:
        virtual ~IMeshUploader() = default;

        /// THE CPU HALF, ANY THREAD (UE: the render data built/loaded on the async loading thread, before any RHI
        /// resource exists). Build the runtime mesh for @p asset, which MeshService has already parsed and
        /// resolved, with its geometry STAGED (host copy, nothing on the device yet). MeshService runs it on a
        /// worker for a mesh a frame asked for, so it touches no device object, no renderer state and no lock
        /// of the service. nullptr when the build is refused; the uploader has logged which precondition failed.
        [[nodiscard]] virtual std::shared_ptr<Mesh> Stage( const std::shared_ptr<Assets::MeshAsset>& asset ) = 0;

        /// THE DEVICE HALF, ON THE THREAD THAT ASKED FOR THE MESH (UE BeginInitResource): upload what Stage left
        /// staged and claim the buffers in the resource ledger under the asset's handle (the BLAS is the ray-tracing
        /// scene's, built on demand: RayTracingGeometryVariants). False when the upload is refused; the uploader has logged why.
        [[nodiscard]] virtual bool Commit( const std::shared_ptr<Mesh>& mesh, const Assets::MeshAsset& asset ) = 0;
    };

    /// The device implementation: `Graphic::MeshFactory` plus the resource ledger. Defined in
    /// MeshGpuUploader.cpp, the only file of the service that touches GPU types.
    [[nodiscard]] std::unique_ptr<IMeshUploader> MakeGpuMeshUploader();
} // namespace Desert::Runtime
