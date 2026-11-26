#pragma once
// ONE BONE OF A LIGHT VIEW'S (and the silhouette's) SKINNING PALETTE, declared once: Tools/ShaderProtocolGen
// generates Mesh/SkinnedBones.glslh (`struct SkinnedBone` + the Bones storage buffer at binding 1) from it, included
// by Mesh/Surface/Vertex_Skinned.glslh (light views) and Programs/Silhouette/Silhouette_Skinned.shader. A view pass
// skins from the view's ObjectBones instead (Common/Velocity.glslh). Material::UploadSkinnedBones writes the pass's
// packed palette. Layout only: the RDG parameter lives in SkinnedMaterialUB.hpp.
#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

// X( C++ type, member, "GLSL name", initialiser )
#define DESERT_SKINNED_BONE_FIELDS( X ) X( glm::mat4, Matrix, "Matrix", glm::mat4( 1.0f ) ) /* 0; stride 64 */

namespace Desert::Graphic::ShaderProtocols
{
    DESERT_SHADER_STRUCT( SkinnedBone, "SkinnedBone", Std430, DESERT_SKINNED_BONE_FIELDS );
    // The palette is uploaded as packed glm::mat4 (Material::UploadSkinnedBones): one bone is exactly one matrix.
    static_assert( sizeof( SkinnedBone ) == sizeof( glm::mat4 ) );

    inline constexpr const ShaderStructDecl* kSkinnedBonesStructs[] = { &kSkinnedBoneStruct };
    /// The light view's bone palette: every skinned draw of the pass packed end to end, a draw's slice named by its
    /// push block's BoneOffset.
    inline constexpr ShaderBlockDecl kSkinnedBonesBlocks[] = { { .Kind             = ShaderBlockKind::ReadBuffer,
                                                                 .Binding          = { ShaderBindingKind::Fixed, 1u, {} },
                                                                 .BlockName        = "Bones",
                                                                 .InstanceName     = "bones",
                                                                 .RuntimeArrayOf   = &kSkinnedBoneStruct,
                                                                 .RuntimeArrayName = "BoneMatrices" } };
    inline constexpr const ShaderBlockDecl& kSkinnedBonesBlock = kSkinnedBonesBlocks[0];
    inline constexpr ShaderIncludeDecl      kSkinnedBonesShaderInclude{
             .Path         = "Mesh/SkinnedBones.glslh",
             .SourceHeader = "Desert/Desert/Source/Engine/Graphic/ShaderProtocols/SkinnedBonesLayout.hpp",
             .Structs      = kSkinnedBonesStructs,
             .Blocks       = kSkinnedBonesBlocks };
} // namespace Desert::Graphic::ShaderProtocols
