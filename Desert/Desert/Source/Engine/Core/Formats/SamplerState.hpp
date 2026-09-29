#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace Desert::Core::Formats
{
    /// How a texture coordinate outside [0,1] is folded back (glTF sampler wrapS/wrapT, UE TextureAddress).
    enum class SamplerWrap
    {
        Repeat = 0, ///< tile (glTF 10497 REPEAT) - what every sampler in the engine did before this field
        Clamp,      ///< clamp to the edge texel (glTF 33071 CLAMP_TO_EDGE)
        Mirror,     ///< tile, flipping every other copy (glTF 33648 MIRRORED_REPEAT)
    };

    /// Magnification/minification between texels. `Linear` follows the machine's texture-quality setting
    /// (bilinear / trilinear / anisotropic); `Nearest` is a property of the ASSET (pixel art, an ID map, a
    /// glTF `magFilter: NEAREST`) and wins over that setting.
    enum class SamplerFilter
    {
        Linear = 0,
        Nearest,
    };

    /**
     * @brief The sampling state of ONE texture slot.
     *
     * Where it comes from, in order: the material's own slot (`MaterialAssetRef::Sampler`, an override a
     * .demat may state) else the shader template's `Sampler(...)` attribute on the Texture2D property
     * (`ShaderParam::Sampler`) else this struct's defaults - Repeat/Repeat/Linear, the state every sampler had
     * before the field existed, so an unannotated template draws exactly the picture it drew before.
     * `ResolveSlotSampler` is the one place that order is written.
     */
    struct SamplerState
    {
        SamplerWrap   WrapU  = SamplerWrap::Repeat;
        SamplerWrap   WrapV  = SamplerWrap::Repeat;
        SamplerFilter Filter = SamplerFilter::Linear;

        bool operator==( const SamplerState& ) const = default;

        /// Dense key for a sampler cache: one state, one number.
        [[nodiscard]] constexpr uint32_t Key() const
        {
            return static_cast<uint32_t>( WrapU ) | ( static_cast<uint32_t>( WrapV ) << 4 ) |
                   ( static_cast<uint32_t>( Filter ) << 8 );
        }
    };

    /// The slot's state: the material's override when it states one, else the template's.
    [[nodiscard]] constexpr SamplerState ResolveSlotSampler( const SamplerState&                templateDefault,
                                                             const std::optional<SamplerState>& materialOverride )
    {
        return materialOverride ? *materialOverride : templateDefault;
    }

    [[nodiscard]] constexpr std::optional<SamplerWrap> ParseSamplerWrap( std::string_view lowerName )
    {
        if ( lowerName == "repeat" )
            return SamplerWrap::Repeat;
        if ( lowerName == "clamp" )
            return SamplerWrap::Clamp;
        if ( lowerName == "mirror" )
            return SamplerWrap::Mirror;
        return std::nullopt;
    }

    [[nodiscard]] constexpr std::optional<SamplerFilter> ParseSamplerFilter( std::string_view lowerName )
    {
        if ( lowerName == "linear" )
            return SamplerFilter::Linear;
        if ( lowerName == "nearest" )
            return SamplerFilter::Nearest;
        return std::nullopt;
    }

    /// glTF magFilter: 9728 NEAREST; 9729 LINEAR and "unset" leave the choice to the renderer (Linear).
    [[nodiscard]] constexpr SamplerFilter SamplerFilterFromGltf( int glMagFilter )
    {
        return glMagFilter == 9728 ? SamplerFilter::Nearest : SamplerFilter::Linear;
    }
} // namespace Desert::Core::Formats
