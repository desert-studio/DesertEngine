#pragma once

#include <Engine/VFX/VFXStackCompiler.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Graphic::System
{
    // THE SPRITE RENDERER'S BINDINGS to an emitter's particle data (VFX-03g; UE: UNiagaraSpriteRendererProperties'
    // PositionBinding / ColorBinding / SpriteSizeBinding over the emitter's FNiagaraDataSet layout). The sprite
    // vertex path reads each bound attribute from the pool's SoA float components at
    //
    //   F[floatBase + (Start + c) * capacity + slot]      (VFXSimulationProgram.hpp's addressing)
    //
    // where Start is the attribute's FloatStart in that emitter's compiled layout. An attribute the layout lacks
    // (or holds with another component count) is not bound: the sprite uses the renderer default for it, and the
    // renderer says so once per stack. Pure, so the resolution is tested without a device.

    enum class ParticleSpriteAttribute : std::uint32_t
    {
        Position   = 0, // vec3, cm, world space
        Color      = 1, // vec4, linear RGBA
        SpriteSize = 2, // vec2, cm
    };
    inline constexpr std::uint32_t kParticleSpriteAttributeCount = 3;

    // The bound float start meaning "not in the layout: the renderer default".
    inline constexpr std::uint32_t kParticleSpriteUnbound = 0xFFFFFFFFu;

    struct ParticleSpriteBindingSpec
    {
        std::string_view Name;
        std::uint32_t    Components = 0;
    };

    inline constexpr std::array<ParticleSpriteBindingSpec, kParticleSpriteAttributeCount>
         kParticleSpriteBindingSpecs = {
              ParticleSpriteBindingSpec{ "Position", 3u },
              ParticleSpriteBindingSpec{ "Color", 4u },
              ParticleSpriteBindingSpec{ "SpriteSize", 2u },
    };

    struct ParticleSpriteBindings
    {
        // Float component start per ParticleSpriteAttribute, kParticleSpriteUnbound = the renderer default.
        std::array<std::uint32_t, kParticleSpriteAttributeCount> FloatStart{
             kParticleSpriteUnbound, kParticleSpriteUnbound, kParticleSpriteUnbound };
        // One line per unbound attribute: what the layout lacks and why ("Color: not in the layout").
        std::vector<std::string> Unbound;

        [[nodiscard]] std::uint32_t StartOf( const ParticleSpriteAttribute attribute ) const
        {
            return FloatStart[static_cast<std::uint32_t>( attribute )];
        }
    };

    [[nodiscard]] inline ParticleSpriteBindings
    ResolveParticleSpriteBindings( const VFX::VFXDataSetLayout& layout )
    {
        ParticleSpriteBindings bindings;
        for ( std::uint32_t a = 0; a < kParticleSpriteAttributeCount; ++a )
        {
            const ParticleSpriteBindingSpec& spec      = kParticleSpriteBindingSpecs[a];
            const VFX::VFXAttributeLayout*   attribute = layout.Find( spec.Name );
            if ( attribute == nullptr )
            {
                bindings.Unbound.push_back( std::string( spec.Name ) + ": not in the layout" );
                continue;
            }
            if ( attribute->FloatCount != spec.Components || attribute->IntCount != 0 )
            {
                bindings.Unbound.push_back(
                     std::string( spec.Name ) + ": the layout holds " + std::to_string( attribute->FloatCount ) +
                     " float and " + std::to_string( attribute->IntCount ) + " int components, the sprite reads " +
                     std::to_string( spec.Components ) + " floats" );
                continue;
            }
            bindings.FloatStart[a] = attribute->FloatStart;
        }
        return bindings;
    }
} // namespace Desert::Graphic::System
