#pragma once

#include <Engine/Graphic/RDG/RDGBindingDecl.hpp>

#include <Common/Core/ResultStr.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>

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
    // Construction is explicit, so a string literal never converts into a parameter at a binding call: the name
    // comes from a struct member (DESERT_SHADER_PARAMETER) or a named engine constant of this type. @p Element is
    // set only by ShaderParameterArrayOf::operator[]: the slot is then "<Name>[<Element>]", the spelling the
    // reflected layout gives each element of an arrayed binding.
    template <ShaderResourceKind K>
    struct ShaderParameterOf
    {
        static constexpr uint32_t kWhole = ~0u;

        constexpr explicit ShaderParameterOf( std::string_view name, uint32_t element = kWhole )
             : Name( name ), Element( element )
        {
        }

        std::string_view Name;
        uint32_t         Element = kWhole;

        // The slot name the reflected layout lists: Name, or "<Name>[<Element>]" for an element of an array.
        [[nodiscard]] std::string SlotName() const
        {
            return Element == kWhole ? std::string( Name ) : std::format( "{}[{}]", Name, Element );
        }
    };

    // An arrayed binding (`texture2D Layers[4]`) declared once; an element is bound as parameter[i].
    template <ShaderResourceKind K>
    struct ShaderParameterArrayOf
    {
        constexpr explicit ShaderParameterArrayOf( std::string_view name ) : Name( name )
        {
        }

        std::string_view Name;

        [[nodiscard]] constexpr ShaderParameterOf<K> operator[]( uint32_t element ) const
        {
            return ShaderParameterOf<K>( Name, element );
        }
    };

    using TextureParameter               = ShaderParameterOf<ShaderResourceKind::Texture>;
    using SamplerParameter               = ShaderParameterOf<ShaderResourceKind::Sampler>;
    using StorageTextureParameter        = ShaderParameterOf<ShaderResourceKind::StorageTexture>;
    using UniformBufferParameter         = ShaderParameterOf<ShaderResourceKind::UniformBuffer>;
    using StorageBufferParameter         = ShaderParameterOf<ShaderResourceKind::StorageBuffer>;
    using AccelerationStructureParameter = ShaderParameterOf<ShaderResourceKind::AccelerationStructure>;

    // A list of separately declared slots that a pass binds by index (the atlas layers, a cube's faces), as
    // parameters of one kind: @p names is the list's one spelling, and each element binds as the slot it names.
    template <ShaderResourceKind K, std::size_t N>
    [[nodiscard]] constexpr std::array<ShaderParameterOf<K>, N>
    ParametersOf( const std::array<std::string_view, N>& names )
    {
        return [&]<std::size_t... I>( std::index_sequence<I...> ) {
            return std::array<ShaderParameterOf<K>, N>{ ShaderParameterOf<K>( names[I] )... };
        }( std::make_index_sequence<N>{} );
    }

    // The kind-erased form a struct lists in kAll for validation.
    struct ShaderParameter
    {
        std::string_view   Name;
        ShaderResourceKind Kind = ShaderResourceKind::Texture;
        // An arrayed binding: the layout lists it as "<Name>[0]", "<Name>[1]", ...
        bool Arrayed = false;
    };

    template <ShaderResourceKind K>
    [[nodiscard]] constexpr ShaderParameter Erase( ShaderParameterOf<K> parameter )
    {
        return ShaderParameter{ parameter.Name, K, false };
    }

    template <ShaderResourceKind K>
    [[nodiscard]] constexpr ShaderParameter Erase( ShaderParameterArrayOf<K> parameter )
    {
        return ShaderParameter{ parameter.Name, K, true };
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

// Declares one arrayed parameter member (`texture2D Name[N]` in the shader); bind element i as Name[i].
#define DESERT_SHADER_PARAMETER_ARRAY( Kind, Name )                                                               \
    static constexpr ::Desert::Graphic::RDG::ShaderParameterArrayOf<                                              \
         ::Desert::Graphic::RDG::ShaderResourceKind::Kind>                                                        \
         Name                                                                                                     \
    {                                                                                                             \
        #Name                                                                                                     \
    }
