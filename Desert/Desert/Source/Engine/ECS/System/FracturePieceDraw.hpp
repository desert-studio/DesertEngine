#pragma once
// A FRACTURED ENTITY RECORDS ITS PIECES AS STATIC-MESH DRAWS (DST-06).
//
// Not a renderer: MeshECSSystem owns one of these and every piece goes out as the DrawStaticMeshCommand a static
// mesh records, so the MeshRenderer batches pieces exactly as it batches statics (one instanced draw per material
// per piece-mesh batch); submeshes whose material is a custom-shader one go out as DrawSlotMaterialMeshCommand,
// split as a static mesh's are (Destruction::PieceDraws), each draw under the piece's own motion part. An entity
// draws as pieces when it has
//   * a FracturePreviewComponent (the Fracture Mode's preview of the tool's fracture, Explode offsets applied), or
//   * a DestructibleComponent whose Rest Collection is read (at its bodies' poses while simulated, else at rest);
// its StaticMeshComponent, if any, is then not drawn (MeshECSSystem skips the entities Record returns).
//
// Per fracture, the piece meshes are built once (saved mesh -> DynamicMesh3 -> render arrays -> DynamicMesh) and
// shared by every entity drawing that fracture, so N crates of one `.dfrac` batch. Per entity, one slot binding
// per piece mesh: submesh i draws Destruction::PieceSubmeshMaterials' choice — the source mesh's slot or the
// interior material (Destruction::ChooseInteriorMaterial; a missing one is logged by name once per fracture and
// drawn with the default surface).
#include <Engine/Assets/Common.hpp>
#include <Engine/Destruction/FractureFormat.hpp>
#include <Engine/Graphic/Materials/MaterialInstance.hpp>
#include <Engine/Graphic/Render/Commands/DrawMeshCommand.hpp>

#include <entt/entt.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Desert
{
    class DynamicMesh;
}

namespace Desert::ECS
{
    class FracturePieceDraw final
    {
    public:
        /// What the mesh system already knows how to do, handed in so the rules stay its own:
        struct Services
        {
            /// The source mesh's material slots (empty while they do not resolve yet).
            std::function<std::vector<Assets::AssetHandle>( const Assets::AssetHandle& mesh )> SourceSlots;
            /// A runtime instance of a material asset; null when it cannot be made.
            std::function<Graphic::MaterialInstancePtr( const Assets::AssetHandle& material )> Instance;
            /// The default surface (a mesh with no slot, a missing interior material).
            std::function<Graphic::MaterialInstancePtr()> DefaultInstance;
            /// The material asset a GUID names, or a null handle when it names none.
            std::function<Assets::AssetHandle( const Common::Content::AssetGuid& )> MaterialByGuid;
            /// The material a slot instance draws with OFF the batched lit path (a custom-shader material), or
            /// null when the batched path draws it — the static path's own test
            /// (MeshECSSystem::CustomSlotMaterial).
            std::function<Graphic::Material*( const Graphic::MaterialInstance* )> SlotMaterial;
            /// A destructible's fracture: null while pending (FractureService::Get).
            std::function<std::shared_ptr<const Destruction::FractureData>( const Assets::AssetHandle& )> Fracture;
        };

        /// Records the pieces of every fractured entity; returns the entities that drew as pieces.
        std::unordered_set<entt::entity> Record( entt::registry&                       registry,
                                                 Graphic::Render::RenderCommandBuffer& commands,
                                                 uint32_t materialsVersion, const Services& services );

    private:
        struct PieceMesh
        {
            int32_t                      Node = -1;
            std::shared_ptr<DynamicMesh> Mesh;
            std::vector<int>             SubmeshMaterialIds;
        };
        struct FractureMeshes
        {
            std::weak_ptr<const Destruction::FractureData> Data;
            std::vector<PieceMesh>                         Pieces; // DrawnPieces order
            std::unordered_map<int32_t, size_t>            ByNode;
        };
        struct EntityPieces
        {
            const Destruction::FractureData*             Fracture         = nullptr;
            uint32_t                                     MaterialsVersion = 0;
            std::vector<Assets::AssetHandle>             SourceSlots;
            std::vector<Graphic::MaterialInstancePtr>    Owned;    // the entity's instances, kept alive here
            std::vector<Graphic::MaterialSlotBindingPtr> Bindings; // per FractureMeshes::Pieces entry
        };

        const FractureMeshes* Meshes( const std::shared_ptr<const Destruction::FractureData>& fracture );
        void Bind( EntityPieces& entity, const FractureMeshes& meshes, const Destruction::FractureData& fracture,
                   std::vector<Assets::AssetHandle> sourceSlots, uint32_t materialsVersion,
                   const Services& services );

        std::unordered_map<const Destruction::FractureData*, FractureMeshes> m_Meshes;
        std::unordered_map<entt::entity, EntityPieces>                       m_Entities;
        std::unordered_set<const Destruction::FractureData*>                 m_Reported; // logged once each
    };
} // namespace Desert::ECS
