#pragma once

// THE COOKED DISPLACEMENT TOPOLOGY OF A MESH ASSET (contract CONTRACT-DISPLACE §9.1: clusters at cook): its own
// DDC bucket next to the render data, keyed by the render-data key, derived by the editor and shipped in a package
// like the distance field (MeshDistanceField.hpp is the pattern). A packaged game registers no builder, and a miss
// there is an error naming the asset and the key.

#include <Common/Content/DerivedDataCache.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Geometry/DisplacementTessellation.hpp>
#include <Engine/Geometry/MeshTypes.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Desert::Assets
{
    // LOD0 render data the topology is built from (the vertices the mesh uploads, submesh-relative indices).
    struct DisplacementTopologyBuildInput
    {
        std::vector<Vertex>  Vertices;
        std::vector<Index>   Indices;
        std::vector<Submesh> Submeshes;
    };

    inline constexpr Common::DDC::Deriver kDisplacementTopologyDeriver{
         "DisplacementTopology", ".dtop", { 0x961BEDE8AFB27933ULL, 0xE857895C98F055FEULL } };
    // Bump when the build produces different bytes for the same input.
    inline constexpr uint32_t kDisplacementTopologyBuilderVersion = 3; // 2: min-cut graph clusters, 3: multilevel

    [[nodiscard]] uint64_t DisplacementTopologyDerivedDataKey( uint64_t renderDataKey );

    using DisplacementTopologyBuilder =
         std::function<Common::ResultStr<std::string>( const DisplacementTopologyBuildInput& input )>;
    // The editor registers BuildDisplacementTopologyBytes at startup; a packaged game registers none.
    void SetDisplacementTopologyBuilder( DisplacementTopologyBuilder builder );
    [[nodiscard]] Common::ResultStr<std::string>
    BuildDisplacementTopologyBytes( const DisplacementTopologyBuildInput& input );

    // DDC hit, or build (single flight per key) and Put. @p input is called only on a miss.
    // Where a mesh's topology lands (the MeshDistanceFieldSlot pattern): empty until the loader's worker job
    // publishes it, empty for good when it could not be had (logged naming the mesh). The mesh draws its base
    // until then.
    class DisplacementTopologySlot
    {
    public:
        [[nodiscard]] std::shared_ptr<const Geometry::DisplacementTopology> Get() const
        {
            const std::lock_guard<std::mutex> lock( m_Mutex );
            return m_Topology;
        }
        void Publish( std::shared_ptr<const Geometry::DisplacementTopology> topology )
        {
            const std::lock_guard<std::mutex> lock( m_Mutex );
            m_Topology = std::move( topology );
        }

    private:
        mutable std::mutex                                    m_Mutex;
        std::shared_ptr<const Geometry::DisplacementTopology> m_Topology;
    };

    [[nodiscard]] Common::ResultStr<std::shared_ptr<const Geometry::DisplacementTopology>>
    LoadDisplacementTopology( const std::filesystem::path& asset, uint64_t renderDataKey,
                              const std::function<Common::ResultStr<DisplacementTopologyBuildInput>()>& input );
} // namespace Desert::Assets
