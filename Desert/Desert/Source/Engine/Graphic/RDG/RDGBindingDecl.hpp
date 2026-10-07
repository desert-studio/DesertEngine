#pragma once

#include <Engine/Graphic/RDG/RDGAccess.hpp>
#include <Engine/Graphic/RDG/RDGCompileResult.hpp> // ResourceKind
#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// RDG-FAULT1. The setup-time declaration of a pass's shader parameter block: what PassBuilder::Bindings records
// and Builder::Compile validates (ValidatePassBindings) before anything is recorded. Split out of
// RDGPassBindings.hpp because the builder stores these per pass, and RDGPassBindings.hpp itself needs the builder.
namespace Desert::Graphic::RDG
{
    enum class SamplerFilter : uint8_t
    {
        Nearest,
        Linear,
    };

    // How a sampler moves between mips (Vulkan: VkSamplerMipmapMode).
    enum class SamplerMipMode : uint8_t
    {
        Nearest,
        Linear,
    };

    enum class SamplerAddress : uint8_t
    {
        ClampToEdge,
        Repeat,
        MirroredRepeat,
    };

    // The sampler a sampled entry is read with. Every field is given: there is no default-constructed sampler,
    // so a call site cannot bind a texture without saying how it is filtered and addressed (the six-field
    // constructor is the only one, so SamplerDesc is not an aggregate and has no default constructor). Level of
    // detail is unclamped (every mip of the bound view is reachable); a Mip(m) range already limits the view to
    // one mip.
    struct SamplerDesc
    {
        SamplerFilter  MinFilter;
        SamplerFilter  MagFilter;
        SamplerMipMode MipMode;
        SamplerAddress AddressU;
        SamplerAddress AddressV;
        SamplerAddress AddressW;

        constexpr SamplerDesc( SamplerFilter minFilter, SamplerFilter magFilter, SamplerMipMode mipMode,
                               SamplerAddress addressU, SamplerAddress addressV, SamplerAddress addressW )
             : MinFilter( minFilter ), MagFilter( magFilter ), MipMode( mipMode ), AddressU( addressU ),
               AddressV( addressV ), AddressW( addressW )
        {
        }

        static constexpr SamplerDesc LinearClamp()
        {
            return { SamplerFilter::Linear,       SamplerFilter::Linear,       SamplerMipMode::Linear,
                     SamplerAddress::ClampToEdge, SamplerAddress::ClampToEdge, SamplerAddress::ClampToEdge };
        }
        static constexpr SamplerDesc PointClamp()
        {
            return { SamplerFilter::Nearest,      SamplerFilter::Nearest,      SamplerMipMode::Nearest,
                     SamplerAddress::ClampToEdge, SamplerAddress::ClampToEdge, SamplerAddress::ClampToEdge };
        }
        static constexpr SamplerDesc LinearRepeat()
        {
            return { SamplerFilter::Linear,  SamplerFilter::Linear,  SamplerMipMode::Linear,
                     SamplerAddress::Repeat, SamplerAddress::Repeat, SamplerAddress::Repeat };
        }

        // One value per distinct description: the key of the backend's sampler cache.
        constexpr uint32_t GetKey() const
        {
            return static_cast<uint32_t>( MinFilter ) | ( static_cast<uint32_t>( MagFilter ) << 2 ) |
                   ( static_cast<uint32_t>( MipMode ) << 4 ) | ( static_cast<uint32_t>( AddressU ) << 6 ) |
                   ( static_cast<uint32_t>( AddressV ) << 9 ) | ( static_cast<uint32_t>( AddressW ) << 12 );
        }

        friend constexpr bool operator==( const SamplerDesc&, const SamplerDesc& ) = default;
    };

    // What the shader slot is. Checked against the reflected descriptor type by the consumer.
    enum class ShaderResourceKind : uint8_t
    {
        SampledTexture, // sampler2D, read with the entry's SamplerDesc
        StorageTexture, // image2D, one mip
        UniformBuffer,
        StorageBuffer,
    };

    // RDG-FAULT1. A shader's resource interface as the graph sees it: names and kinds, no set / binding indices
    // (those stay the backend's, resolved at record time). Made from the pipeline's reflection by the backend
    // (Vulkan: MakeShaderBindingLayout, REMAINDER-FAULT1-C0 step C2) when the renderer builds its pass; a value,
    // so validation needs no device and the suite builds one by hand.
    struct ShaderSlot
    {
        std::string        Name;
        ShaderResourceKind Kind = ShaderResourceKind::SampledTexture;
    };

    struct ShaderBindingLayout
    {
        std::string             ShaderName;            // what an error names: the shader's own name
        std::vector<ShaderSlot> Slots;                 // every resource slot the shader declares
        uint32_t                PushConstantBytes = 0; // the declared range; 0 = the shader declares none
    };

    // What the other route (the material, or a pipeline's own setters for asset textures) fills for the draw, as
    // known when the pass is declared: the slots holding a real resource and whether it gives the push constants.
    struct OtherRouteFill
    {
        std::vector<std::string> Slots;
        bool                     PushConstants = false;
    };

    // One entry of a declared block: the resource, the access it declares for the pass, and the shader slot.
    struct DeclaredBindingEntry
    {
        std::string                ShaderName;
        ShaderResourceKind         Kind     = ShaderResourceKind::SampledTexture;
        ResourceKind               Resource = ResourceKind::Texture;
        uint32_t                   Index    = kInvalidResource;
        Access                     Declared = Access::None;
        SubresourceRange           Range    = SubresourceRange::All();
        std::optional<SamplerDesc> Sampler; // SampledTexture only
    };

    struct DeclaredBindingBlock
    {
        // Shared and const: a renderer hands the graph the layout it keeps (RDG::LayoutCache) by pointer, no
        // per-frame copy of the slot list; null is refused by ValidatePassBindings.
        std::shared_ptr<const ShaderBindingLayout> Layout;
        OtherRouteFill                             Other;
        std::vector<DeclaredBindingEntry>          Entries;
        uint32_t                                   PushConstantBytes = 0; // what the exec will push
    };

    // A declared block of one pass: the exec's handle to it. Meaningless outside the graph that declared it.
    struct BindingBlockRef
    {
        uint32_t Pass  = ~0u;
        uint32_t Block = ~0u;
    };

} // namespace Desert::Graphic::RDG
