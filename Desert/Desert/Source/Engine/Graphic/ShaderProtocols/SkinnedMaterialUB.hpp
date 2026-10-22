#pragma once

#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>
#include <Engine/Graphic/ShaderProtocols/Std140Block.hpp>

#include <glm/glm.hpp>

#include <array>
#include <string>
#include <string_view>

// ONE BONE OF A LIGHT VIEW'S (and the silhouette's) SKINNING PALETTE, listed ONCE: the member expands into
// SkinnedBone and into kSkinnedBoneFields, from which the includer GENERATES Mesh/SkinnedBones.glslh (`struct
// SkinnedBone` + the Bones storage buffer at binding 1; no file on disk), included by
// Mesh/Surface/Vertex_Skinned.glslh (light views) and Programs/Silhouette/Silhouette_Skinned.shader. A view pass
// skins from the view's ObjectBones instead (Common/ObjectMotion.glslh). Material::UploadSkinnedBones writes the
// pass's packed palette.
//
// FIELD( C++ type, member, GLSL name, initialiser )
#define DESERT_SKINNED_BONE_FIELDS( FIELD ) FIELD( glm::mat4, Matrix, Matrix, { 1.0f } ) /* 0; stride 64 */

namespace Desert::Graphic::ShaderProtocols
{
    /// SkinnedBone and kSkinnedBoneFields (GLSL `struct SkinnedBone`), Std140Block.hpp.
    DESERT_SHADER_STRUCT( SkinnedBone, DESERT_SKINNED_BONE_FIELDS )
    static_assert( Std430StructLayoutMatches( kSkinnedBoneFields, sizeof( SkinnedBone ) ),
                   "SkinnedBone is not laid out (or strided) as std430 places the generated struct SkinnedBone" );
    // The palette is uploaded as packed glm::mat4 (Material::UploadSkinnedBones): one bone is exactly one matrix.
    static_assert( sizeof( SkinnedBone ) == sizeof( glm::mat4 ) );

    struct SkinnedUB
    {
        // Block name of the storage buffer in the shader — keys the material property lookup.
        // The block as a binding parameter (RDGShaderParameters.hpp) - the one spelling; Name is its string form.
        static constexpr RDG::StorageBufferParameter Parameter{ "Bones" };
        inline const static std::string              Name = std::string( Parameter.Name );
    };

    /// The include path the shader includer answers with the generated struct + storage buffer.
    inline constexpr std::string_view kSkinnedBonesInclude = "Mesh/SkinnedBones.glslh";

    /// The light view's bone palette: every skinned draw of the pass packed end to end, a draw's slice named by
    /// its push block's BoneOffset.
    [[nodiscard]] inline const std::string& SkinnedBonesGlsl()
    {
        static const std::string glsl = GenerateGuardedGlsl(
             "DESERT_SKINNED_BONES_GLSLH", "Engine/Graphic/ShaderProtocols/SkinnedMaterialUB.hpp",
             GenerateGlslStruct( "SkinnedBone", kSkinnedBoneFields ) + "\n" +
                  GenerateGlslBlock( "ReadBuffer", 1, SkinnedUB::Name, "    SkinnedBone BoneMatrices[];\n",
                                     "bones" ) );
        return glsl;
    }
} // namespace Desert::Graphic::ShaderProtocols
