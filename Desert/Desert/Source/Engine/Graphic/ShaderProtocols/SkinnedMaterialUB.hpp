#pragma once
// The light views' (and the silhouette's) bone palette as a binding parameter. Its GPU layout (struct SkinnedBone,
// the Bones storage buffer at binding 1) is declared once in SkinnedBonesLayout.hpp; Tools/ShaderProtocolGen
// generates Mesh/SkinnedBones.glslh from it.
#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>
#include <Engine/Graphic/ShaderProtocols/SkinnedBonesLayout.hpp>

#include <string>

namespace Desert::Graphic::ShaderProtocols
{
    struct SkinnedUB
    {
        // The storage buffer as a binding parameter (RDGShaderParameters.hpp), named by its declaration; Name is
        // its string form and keys the material property lookup.
        static constexpr RDG::StorageBufferParameter Parameter{ kSkinnedBonesBlock.BlockName };
        inline const static std::string              Name = std::string( Parameter.Name );
    };
} // namespace Desert::Graphic::ShaderProtocols
