#pragma once

#include <Engine/Graphic/RDG/RDGBindingDecl.hpp>

#include <Common/Core/ResultStr.hpp>

#include <array>
#include <span>
#include <string_view>

// A pass's shader parameters declared once in C++ (UE: SHADER_PARAMETER_STRUCT). The struct is the one place a
// parameter's name is spelled: the pass binds through its members (a typo is a compile error, and the member's
// kind picks the only BindingBlockBuilder call that accepts it), and ValidateShaderParameters compares the struct
// with the shader's reflected layout when the pipeline is created, so a shader that renames or drops a parameter
// fails at load with the parameter's name instead of at the first frame that declares the pass.
//
//   struct FXAAShaderParameters
//   {
//       DESERT_SHADER_PARAMETER( Texture, LdrSceneColorTexture );
//       DESERT_SHADER_PARAMETER( Sampler, LinearRepeatSampler );
//       static constexpr std::array kAll{ RDG::Erase( LdrSceneColorTexture ), RDG::Erase( LinearRepeatSampler ) };
//   };
namespace Desert::Graphic::RDG
{
    // One parameter with its kind in the type: TextureParameter binds only through BindingBlockBuilder::Texture.
    template <ShaderResourceKind K>
    struct ShaderParameterOf
    {
        std::string_view Name;
    };

    using TextureParameter               = ShaderParameterOf<ShaderResourceKind::Texture>;
    using SamplerParameter               = ShaderParameterOf<ShaderResourceKind::Sampler>;
    using StorageTextureParameter        = ShaderParameterOf<ShaderResourceKind::StorageTexture>;
    using UniformBufferParameter         = ShaderParameterOf<ShaderResourceKind::UniformBuffer>;
    using StorageBufferParameter         = ShaderParameterOf<ShaderResourceKind::StorageBuffer>;
    using AccelerationStructureParameter = ShaderParameterOf<ShaderResourceKind::AccelerationStructure>;

    // The kind-erased form a struct lists in kAll for validation.
    struct ShaderParameter
    {
        std::string_view   Name;
        ShaderResourceKind Kind = ShaderResourceKind::Texture;
    };

    template <ShaderResourceKind K>
    [[nodiscard]] constexpr ShaderParameter Erase( ShaderParameterOf<K> parameter )
    {
        return ShaderParameter{ parameter.Name, K };
    }

    // The struct against the shader: every parameter is a slot of @p layout with the same kind; the first that is
    // not is the error, naming the parameter and the shader. The other direction - a slot neither the pass nor
    // the material fills - stays ValidatePassBindings', because what the material fills is known only once it
    // holds its resources, not when the pipeline is created.
    [[nodiscard]] Common::BoolResultStr ValidateShaderParameters( const ShaderBindingLayout&      layout,
                                                                  std::span<const ShaderParameter> parameters );
} // namespace Desert::Graphic::RDG

// Declares one parameter member whose shader name is its own spelling: the name is written once.
#define DESERT_SHADER_PARAMETER( Kind, Name )                                                                     \
    static constexpr ::Desert::Graphic::RDG::ShaderParameterOf<::Desert::Graphic::RDG::ShaderResourceKind::Kind>  \
         Name                                                                                                     \
    {                                                                                                             \
        #Name                                                                                                     \
    }
