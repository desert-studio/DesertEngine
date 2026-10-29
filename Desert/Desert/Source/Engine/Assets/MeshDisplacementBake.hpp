#pragma once

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/DerivedDataCache.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Core/Formats/ShaderProgramMeta.hpp>
#include <Engine/Geometry/DisplacementTessellation.hpp>
#include <Engine/Geometry/MeshTypes.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

// THE DISPLACED SURFACE AS DERIVED DATA (CONTRACT-DISPLACE §1g, §9.9, §9.12). A displaced surface that no camera
// chooses - one fixed level - is cooked once from the render data, the topology and the section materials'
// heights, and serves:
// - Baked mode: the dense mesh drawn as an ordinary StaticMesh (its own LOD chain, SDF, cards, BLAS);
// - collision in BOTH modes: the same bake-level surface, so physics does not depend on the camera;
// - Runtime SDF and cards: the maximum-level surface (BuildDisplacedMesh, not stored - only a miss of the SDF or
//   cards bucket reads it), keyed from DisplacedMeshDerivedDataKey with no edge length.
// Heights are sampled on the CPU (Geometry::BuildDisplacedSurface) from the texture asset's source; the decoder
// lives in the editor, so the build is the editor's (SetDisplacedMeshBuilder) and a packaged game reads the
// bucket.
namespace Desert::Assets
{
    class AssetManager;
    class SurfaceMaterialAsset;

    // The chain a material's values resolve through: the material first, its template-owning root last, and the
    // template's metadata. MaterialService::CreateRuntimeInstance walks the same chain; the CPU readers of a
    // material's textures (the displaced surface's heights, the grass cover's look) read it here.
    struct MaterialChain
    {
        std::vector<std::shared_ptr<SurfaceMaterialAsset>> Links;
        Core::Formats::ShaderProgramMeta                   Meta;
    };
    [[nodiscard]] Common::ResultStr<MaterialChain> ResolveMaterialChain( AssetManager&                     manager,
                                                                         const Common::Content::AssetGuid& material );
    // Child first, then each parent, then the template's default; zero when nobody declares it.
    [[nodiscard]] glm::vec4 MaterialChainParam( const MaterialChain& chain, const std::string& name );

    // One section's height as its material resolves it on the CPU (parent chain, then the template's defaults),
    // the asset-side twin of Graphic::System::DisplacementMaterialRecord.
    struct SubmeshHeightInput
    {
        std::filesystem::path TextureAsset;   // the height map's texture asset (.detex)
        uint64_t              TextureKey = 0; // its derived-data key: source content + import settings
        uint32_t              Channel    = 0;
        uint32_t              UVSet      = 0;
        float                 ScaleCm    = 0.0f;
        float                 Center     = 0.0f;
        glm::vec2             Tiling{ 1.0f, 1.0f };
        glm::vec2             Offset{ 0.0f, 0.0f };
        float                 Rotation                                      = 0.0f;
        bool                  HasTransform                                  = false;
        bool                  operator==( const SubmeshHeightInput& ) const = default;
    };

    // The height of the section drawn with @p material: nullopt when its template declares no Height or the
    // material assigns no Height texture (the same predicate as the `.Parallax` cells, AssignsSurfaceHeight).
    [[nodiscard]] Common::ResultStr<std::optional<SubmeshHeightInput>>
    ResolveSubmeshHeight( AssetManager& manager, const Common::Content::AssetGuid& material );

    inline constexpr Common::DDC::Deriver kDisplacedStaticMeshDeriver{
         "DisplacedStaticMesh", ".stmesh", { 0x5C0D7A41E92B36F8ULL, 0xA7E4193D0B65C2F1ULL } };
    // 2: heights read from the cooked texture chain (as the GPU samples), no longer from the source pixels.
    inline constexpr uint32_t kDisplacedMeshBuilderVersion = 2;

    struct DisplacedMeshBuildInput
    {
        std::vector<Vertex>                                   Vertices; // LOD0 render data
        std::vector<MeshVertexStreams>                        Streams;
        std::vector<Index>                                    Indices;
        std::vector<Submesh>                                  Submeshes;
        std::shared_ptr<const Geometry::DisplacementTopology> Topology;
        std::vector<std::optional<SubmeshHeightInput>>        Heights; // one per submesh
        // The bake level: every cluster diced to this edge length (DisplacementLevelsForEdge). nullopt = every
        // cluster at its MaxLevel (the Runtime SDF/cards surface).
        std::optional<float> EdgeLengthCm;
        bool                 BakeLODs = false; // the drawn Baked mesh carries a LOD chain; collision reads LOD0
    };

    // Render-data key + topology and builder versions + level + LOD flag + every section's height -> the key.
    [[nodiscard]] uint64_t
    DisplacedMeshDerivedDataKey( uint64_t renderDataKey, std::optional<float> edgeLengthCm, bool bakeLODs,
                                 std::span<const std::optional<SubmeshHeightInput>> heights );

    // The decoded surface every consumer reads (StaticMesh constructor inputs; BakedLODs filled when cooked).
    struct DisplacedMeshData
    {
        std::vector<Vertex>            Vertices;
        std::vector<Index>             Indices;
        std::vector<Submesh>           Submeshes;
        std::vector<MeshVertexStreams> Streams;
        uint64_t                       Key = 0; // the derived-data key it was read or built under
    };

    // Where a builder is registered (the editor, at startup) a miss builds and Puts; a packaged game registers
    // none, and a miss there is an error naming the asset and the key. Returns EncodeMeshBinary bytes.
    using DisplacedMeshBuilder = std::function<Common::ResultStr<std::string>( const DisplacedMeshBuildInput& )>;
    void SetDisplacedMeshBuilder( DisplacedMeshBuilder builder );

    // DDC hit, or build (single-flight per key) and Put. @p input is called only on a miss.
    [[nodiscard]] Common::ResultStr<std::shared_ptr<const DisplacedMeshData>>
    LoadDisplacedMesh( const std::filesystem::path& asset, uint64_t key,
                       const std::function<Common::ResultStr<DisplacedMeshBuildInput>()>& input );

    // The builder run directly, nothing stored (the maximum-level surface behind a Runtime SDF/cards miss).
    [[nodiscard]] Common::ResultStr<std::shared_ptr<const DisplacedMeshData>>
    BuildDisplacedMesh( const std::filesystem::path& asset, uint64_t key, const DisplacedMeshBuildInput& input );

    // Where a mesh's baked displaced geometry lands when its derived-data job finishes (as MeshDistanceFieldSlot):
    // empty until then, and for good when the derive failed (logged naming the mesh). Any thread.
    class DisplacedMeshSlot
    {
    public:
        [[nodiscard]] std::shared_ptr<const DisplacedMeshData> Get() const
        {
            const std::lock_guard<std::mutex> lock( m_Mutex );
            return m_Data;
        }
        void Publish( std::shared_ptr<const DisplacedMeshData> data )
        {
            const std::lock_guard<std::mutex> lock( m_Mutex );
            m_Data = std::move( data );
        }
        // The derive failed (logged naming the mesh and the key): the readers stop waiting and keep the base mesh,
        // as the log says.
        void Fail()
        {
            const std::lock_guard<std::mutex> lock( m_Mutex );
            m_Failed = true;
        }
        [[nodiscard]] bool Failed() const
        {
            const std::lock_guard<std::mutex> lock( m_Mutex );
            return m_Failed;
        }

    private:
        mutable std::mutex                       m_Mutex;
        std::shared_ptr<const DisplacedMeshData> m_Data;
        bool                                     m_Failed = false;
    };
} // namespace Desert::Assets
