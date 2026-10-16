#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingDynamicGeometry.hpp>
#include <Engine/Graphic/Systems/RayTracing/ProceduralRayTracingGeometry.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingScene.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace Desert
{
    class Mesh;
}

// THE BLAS OF A PROCEDURAL MESH (UE: a dynamic mesh's FRayTracingGeometry, built by the dynamic-geometry
// collection). A mesh the uploader made no BLAS for (DynamicMesh: Mesh::MarkGpuStale drops it on every re-upload)
// gets one here, built INSIDE the frame graph by "RayTracingScene.DynamicGeometry" from its resident vertex and
// index buffers - once per (mesh, hidden mask) and buffer upload, PREFER_FAST_TRACE, never refit (the vertices
// change only by a re-upload, which is a new buffer and therefore a new build). Owned by the scene
// (RayTracingSceneCache), shared by its views: the first view of a frame builds, the next ones instance it.
namespace Desert::Graphic::RayTracing
{
    class RayTracingProceduralCache
    {
    public:
        // For every primitive with a ProceduralMesh: resolves its (mesh, mask) entry, appends a build to @p
        // buildsOut when the entry has no build of the mesh's current buffers, and points Geometry at the entry.
        // An entry built earlier (another frame or view) is appended to @p blasOut for this view's TLAS pass to
        // read. A refused entry keeps Geometry null (counted NoGeometry) and is logged once per entry.
        [[nodiscard]] Common::BoolResultStr AddBuilds( RDG::Builder&                  graph,
                                                       std::span<RayTracingPrimitive> primitives, uint64_t frame,
                                                       std::vector<RayTracingDynamicGeometryBuild>& buildsOut,
                                                       std::vector<RDG::BufferRef>&                 blasOut );

        // Releases the entries no view asked for in @p frame (RayTracingSceneCache::BeginViewFrame).
        void EndFrame( const uint64_t frame )
        {
            std::erase_if( m_Entries, [frame]( const auto& entry ) { return entry.second.LastFrame != frame; } );
        }

        [[nodiscard]] uint64_t GetMemoryBytes() const
        {
            uint64_t bytes = 0;
            for ( const auto& item : m_Entries )
                if ( item.second.Geometry )
                    bytes += item.second.Geometry->GetMemoryBytes();
            return bytes;
        }
        [[nodiscard]] std::size_t GetEntryCount() const
        {
            return m_Entries.size();
        }

    private:
        struct Key
        {
            const Mesh* MeshPtr                        = nullptr;
            uint64_t    Hidden                         = 0;
            bool        operator==( const Key& ) const = default;
        };
        struct KeyHash
        {
            std::size_t operator()( const Key& key ) const noexcept
            {
                return std::hash<const void*>{}( key.MeshPtr ) ^
                       ( std::hash<uint64_t>{}( key.Hidden ) * 0x9E3779B97F4A7C15ull );
            }
        };
        struct Entry
        {
            std::unique_ptr<ProceduralRayTracingGeometry> Geometry;
            RDG::ExternalBuffer VerticesImport; // the mesh's vertex buffer as a graph external
            uint64_t            LastFrame     = 0;
            uint64_t            LastCall      = 0;     // the AddBuilds call that last handled it
            bool                Ready         = false; // prepared in LastFrame: views may trace it
            bool                RefusalLogged = false;
        };

        std::unordered_map<Key, Entry, KeyHash> m_Entries;
        uint64_t                                m_Call = 0;
    };
} // namespace Desert::Graphic::RayTracing
