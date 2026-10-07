#pragma once

#include <Engine/Geometry/MeshTypes.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace Desert::Assets
{
    class MeshAsset;
}

namespace Desert::Editor::MeshAssetDetails
{
    // WHAT A MESH ASSET IS, drawn in the ASSET EDITOR'S Details (UE: the Static Mesh Editor's and the Skeletal
    // Mesh Editor's Asset Details — LOD / element facts and the Import Settings category). The level's
    // Details of a mesh COMPONENT does not draw this: there a mesh is a slot, its materials and the
    // component's own properties. One body for both asset editors (the Static Mesh viewer and the Animation
    // Editor's Mesh mode), so the two can not drift into two different "what this mesh is" panels.
    //
    // Elements (one row per submesh: index, imported name, triangles, vertices, LOD levels) and the source's
    // Import Settings with Reimport (ImportOptions::DrawImportSettingsSection; nothing for a mesh with no
    // source).
    void Draw( const Assets::MeshAsset& asset );

    // What the vertex weights say about a rig. Real, checkable problems only — the engine has no fixed bone
    // cap to warn about (the pose lives in a storage buffer that grows on demand), but a vertex no bone
    // moves, or one pointing past the end of the skeleton, is a genuine bug of the ASSET.
    struct SkinningAudit
    {
        uint64_t Unweighted      = 0; // no influence at all -> the vertex stays in bind pose
        uint64_t OutOfRange      = 0; // references a bone index the skeleton does not have
        uint64_t FullyInfluenced = 0; // uses all 4 slots -> the importer may have dropped weights
    };
    [[nodiscard]] SkinningAudit AuditSkinning( std::span<const SkinnedVertex> vertices, std::size_t boneCount );

    // The audit as the Skeletal Mesh editor's warnings (wrapped, coloured by severity); nothing when clean.
    void DrawSkinningAudit( const SkinningAudit& audit );
} // namespace Desert::Editor::MeshAssetDetails
