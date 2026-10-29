#pragma once
// HOW A MESH ASSET IS DISPLACED (contract CONTRACT-DISPLACE §1g, §9.7): the import settings both mesh kinds state -
// a static mesh in its source asset's IMPT (MeshSourceAsset.hpp MeshImportSettings), a skinned mesh in its
// MeshBinary header (version 6; a skinned mesh has no source envelope, its .skmesh is the asset).
#include <cstdint>

namespace Desert::Assets
{
    // How a mesh whose section materials declare a Height moves its surface (contract CONTRACT-DISPLACE §1g, §9):
    // `None` = not displaced (the surfaces keep their parallax); `Runtime` = the scene's DisplacementCache dices
    // it per cluster by screen size; `Baked` = a dense displaced mesh cooked into its own DDC bucket and drawn as
    // any static mesh (LOD chain, loader BLAS, SDF, cards). UE: the material's Nanite displacement + the mesh's
    // Nanite settings; here the choice is the mesh's, so one material serves displaced and flat meshes.
    enum class MeshDisplacementMode : uint8_t
    {
        None,
        Runtime,
        Baked,
    };
    // The cook-time tessellation of a displaced mesh: the Baked mesh, and the fixed-level surface the collision
    // of BOTH modes is cooked from (§9.9) - camera-independent, so feet stand on the same stones every run.
    struct MeshDisplacementBakeSettings
    {
        float EdgeLengthCm                                             = 1.0f; // target diced edge; finite, > 0
        bool  operator==( const MeshDisplacementBakeSettings& ) const = default;
    };

    // A skinned mesh is displaced only at runtime, over the skin cache's posed vertices each frame (§9.8): its
    // deformation is not known at cook, so a baked surface could only be one pose. Baked is refused for it.
    [[nodiscard]] constexpr bool IsDisplacementModeAllowedForSkinned( const MeshDisplacementMode mode )
    {
        return mode != MeshDisplacementMode::Baked;
    }
} // namespace Desert::Assets
