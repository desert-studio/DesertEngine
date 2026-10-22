#pragma once

#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>
#include <Engine/Runtime/ImageHandle.hpp>

#include <string>

namespace Desert::Graphic::ShaderProtocols
{
    struct SkinnedUB
    {
        // The block as a binding parameter (RDGShaderParameters.hpp) - the one spelling; Name is its string form.
        static constexpr RDG::StorageBufferParameter Parameter{ "Bones" };
        inline const static std::string              Name = std::string( Parameter.Name );

        std::vector<glm::mat4> BoneMatrices;
    };
} // namespace Desert::Graphic::ShaderProtocols
