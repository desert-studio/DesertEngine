#pragma once
// HOW A FRACTURED ENTITY'S PIECES ARE DRAWN (DST-06; UE: UGeometryCollectionComponent's render data — one
// instance per transform of the collection, its material list the source mesh's plus the interior one).
//
// No renderer of its own: MeshECSSystem turns every piece into the same DrawStaticMeshCommand a static mesh
// records, so pieces batch through the existing instanced path (one draw per material per piece-mesh batch).
// What lives here is the decision that draw needs, kept free of the GPU so a suite checks it:
//   * WHICH nodes draw: the leaves that carry geometry (an inner node is the union of its leaves);
//   * WHERE: from the piece's body while the fracture is simulated (DestructibleComponent::RuntimeNodeWorld,
//     written after each physics step), otherwise the rest pose — the entity's world transform, because the
//     bake is in the fracture's own space — moved by the Fracture Mode's Explode offset in the PREVIEW only;
//   * WITH WHAT: the bake gives the cell walls their own material ID (FractureData::InteriorMaterialId); a
//     submesh of that ID draws the fracture's interior material, any other the source mesh's slot of its ID;
//   * ON WHICH PATH: a submesh whose material is a custom-shader one leaves the batched lit draw for a
//     DrawSlotMaterialMeshCommand — the split a static mesh makes (ECS::Rules::SplitMeshSlotDraws) — and every
//     draw of a piece keeps the piece's own motion key, on either path.
//
// Every number is in centimetres.
#include <Engine/Destruction/FractureFormat.hpp>
#include <Engine/ECS/System/SystemRules.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Desert::Destruction
{
    /// The nodes that draw: Piece leaves (no child) whose saved mesh has a triangle, in node order.
    [[nodiscard]] std::vector<int32_t> DrawnPieces( const std::vector<FractureNode>& nodes );

    /// One piece instance: the node it draws and its world transform (fracture space -> world).
    struct PieceInstance
    {
        int32_t   Node      = -1;
        glm::mat4 Transform = glm::mat4( 1.0f );
    };

    /// The instances of @p nodes' drawn pieces.
    ///  - @p simulatedNodeWorld non-empty (the fracture is in the DestructionWorld): one entry per node; a piece
    ///    takes its body's transform, and a piece whose body is gone (removed on sleep, killed) is not drawn.
    ///  - otherwise the rest pose: @p entityWorld, preceded by the node's @p previewOffsets entry (cm, fracture
    ///    space) when the Fracture Mode preview passes them. Outside the preview they are empty: the Explode
    ///    slider is editor state and never reaches a saved or simulated pose.
    [[nodiscard]] std::vector<PieceInstance>
    PieceInstances( const std::vector<FractureNode>& nodes, const glm::mat4& entityWorld,
                    std::span<const std::optional<glm::mat4>> simulatedNodeWorld,
                    std::span<const glm::dvec3>               previewOffsets );

    /// Which material a piece submesh draws with.
    struct PieceSubmeshMaterial
    {
        bool     Interior   = false; // the fracture's interior material
        uint32_t SourceSlot = 0;     // else: this slot of the source mesh's materials
    };

    /// Per submesh of a piece (its render submeshes, in ascending material-ID order as ToRenderMesh emits them,
    /// @p submeshMaterialIds index-aligned): the interior ID -> the interior material; any other ID -> the source
    /// slot of that ID, clamped to the last of @p sourceSlotCount (the static path's rule for a mesh with more
    /// IDs than slots); with no source slot at all, slot 0 (the default surface the caller resolves it to).
    [[nodiscard]] std::vector<PieceSubmeshMaterial> PieceSubmeshMaterials( std::span<const int> submeshMaterialIds,
                                                                           int32_t              interiorMaterialId,
                                                                           size_t               sourceSlotCount );

    /// The interior material to bind, or why there is none. A null GUID or one that names no material asset
    /// is an ERROR naming the fracture and the GUID: the caller logs it and draws the interior faces with the
    /// default surface — never silently with an exterior material.
    struct InteriorMaterialChoice
    {
        Common::Content::AssetGuid Material; // null when Error is set
        std::string                Error;
    };
    [[nodiscard]] InteriorMaterialChoice
    ChooseInteriorMaterial( const FractureData& fracture, const std::string& fractureName,
                            const std::function<bool( const Common::Content::AssetGuid& )>& isMaterialAsset );

    /// The motion part of a piece's draws (MotionRecord::Part): node + 1, so every piece reads its own previous
    /// world from the view's MotionHistory and 0 stays "the entity itself".
    [[nodiscard]] inline uint32_t PieceMotionPart( int32_t node )
    {
        return static_cast<uint32_t>( node ) + 1u;
    }

    /// One draw of a piece instance. SlotPath false: the batched lit draw (DrawStaticMeshCommand), Mask = the
    /// submeshes it hides. SlotPath true: a custom-shader slot material's draw (DrawSlotMaterialMeshCommand) with
    /// Mat, Mask = the submeshes it draws. Both at the piece's Transform under the piece's MotionPart.
    template <class Material>
    struct PieceDraw
    {
        bool      SlotPath = false;
        Material  Mat{};
        uint64_t  Mask        = 0;
        bool      CastShadows = false;
        glm::mat4 Transform   = glm::mat4( 1.0f );
        uint32_t  MotionPart  = 0;
    };

    /// The draws of @p instance's piece mesh of @p submeshCount submeshes: split exactly as a static mesh is
    /// (ECS::Rules::SplitMeshSlotDraws; the piece casts, hides nothing), slot draws first, then the lit draw when
    /// a submesh stays on it. @p slotMaterialOf( si ) -> submesh si's custom-shader material (the piece's slot
    /// binding entry, i.e. the PieceSubmeshMaterials choice — interior or source slot), or null for the lit path.
    template <class Material, class SlotMaterialOf>
    [[nodiscard]] std::vector<PieceDraw<Material>> PieceDraws( const PieceInstance& instance, size_t submeshCount,
                                                               SlotMaterialOf&& slotMaterialOf )
    {
        const auto     split = ECS::Rules::SplitMeshSlotDraws<Material>( submeshCount, /*hidden*/ 0ull,
                                                                         /*castShadows*/ true, slotMaterialOf );
        const uint32_t part  = PieceMotionPart( instance.Node );
        std::vector<PieceDraw<Material>> draws;
        for ( const auto& slot : split.SlotDraws )
            draws.push_back( { true, slot.Mat, slot.VisibleMask, slot.CastShadows, instance.Transform, part } );
        if ( split.SurfaceDrawEmitted )
            draws.push_back(
                 { false, Material{}, split.SurfaceHidden, split.SurfaceCastShadows, instance.Transform, part } );
        return draws;
    }
} // namespace Desert::Destruction
