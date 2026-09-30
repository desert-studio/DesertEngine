#pragma once

#include <Common/Core/ResultStr.hpp>

#include <Engine/Graphic/RDG/RDGBuilder.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// RDG-A2 - how a renderer binds graph resources to a pipeline (UE: the pass parameter struct of FRDGTexture*
// that the shader parameters are set from inside the pass lambda, and that nothing keeps after it).
//
// A PassBindings is the ONE parameter block of one draw or dispatch recorded by an exec lambda. It is built
// inside the exec from the PassContext, filled with TextureRef / BufferRef the pass declared in its setup,
// handed to Renderer::DispatchCompute / Renderer::DrawFullscreen, and destroyed when the exec returns. The
// backend writes a descriptor set for it in THAT exec (Vulkan: VulkanRdgBackend::DescriptorsOf) and forgets it.
//
// Every image or buffer that is a resource of the graph - a transient (Builder::CreateTexture/CreateBuffer) OR
// an external (FrameTextures::Import, a history image, a framebuffer attachment) - is bound through a
// PassBindings when a pass records with it. There is no second route: ComputePipeline::SetInput/SetOutput/
// SetStorageBuffer and Texture2DProperty::SetImage remain only for work recorded OUTSIDE a graph (bakes on a
// GpuBatch, immediate Dispatch) and for asset textures that are not graph resources (a lens-dirt texture, a
// LUT loaded from disk). A pass that binds a graph resource through a Material property or a pipeline setter
// is a defect, and so is a slot bound by both routes (the draw is refused, see Renderer::DrawFullscreen).
//
// SLOTS ARE SHADER NAMES. An entry names the resource as the shader declares it ("u_BloomTexture",
// "u_Output"); the consumer resolves the name against the pipeline's shader reflection to (set, binding,
// descriptor type). A name the shader does not declare, a kind that does not match the declared descriptor
// type (Sampled into a storage-image slot), and a resource slot of the shader that neither this block nor the
// material fills are errors returned by the consumer, never a silent fallback image. Binding indices never
// appear at a call site: they are the shader's business and drift with it.
//
// SAMPLERS. A sampled entry names the sampler it is read with (SamplerDesc), as UE's pass parameters carry a
// TStaticSamplerState next to the texture. There is no default and no implicit sampler on the graph route: the
// call site passes one, usually a named constant (SamplerDesc::LinearClamp()). The backend turns the description
// into a device object it caches for the device's lifetime (Vulkan: VulkanRdgPassDescriptors::GetSampler), so
// equal descriptions share one object and nothing is created per frame. Asset textures bound through a material
// keep the sampler their image owns; that is the other route and is unchanged.
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
    // so a call site cannot bind a texture without saying how it is filtered and addressed. Level of detail is
    // unclamped (every mip of the bound view is reachable); a Mip(m) range already limits the view to one mip.
    struct SamplerDesc
    {
        SamplerFilter  MinFilter;
        SamplerFilter  MagFilter;
        SamplerMipMode MipMode;
        SamplerAddress AddressU;
        SamplerAddress AddressV;
        SamplerAddress AddressW;

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

    // One resolved texture entry: the binding PassContext::GetTexture returned for THIS execution of the pass.
    // Valid only until the exec lambda returns (see PassContext A(3)).
    struct BoundTexture
    {
        std::string        ShaderName;
        ShaderResourceKind Kind = ShaderResourceKind::SampledTexture;
        TextureBinding     Texture;
        SubresourceRange   Range    = SubresourceRange::All();
        Access             Declared = Access::None;
        // Set for SampledTexture (the entry's sampler), empty for StorageTexture.
        std::optional<SamplerDesc> Sampler;
    };

    struct BoundBuffer
    {
        std::string        ShaderName;
        ShaderResourceKind Kind = ShaderResourceKind::StorageBuffer;
        BufferBinding      Buffer;
        Access             Declared = Access::None;
    };

    // The per-exec parameter block. Built from the PassContext the exec lambda receives; not copyable and
    // not movable, so it cannot be returned out of the lambda or stored in a renderer. Each Add* resolves its
    // ref through PassContext::GetTexture / GetBuffer immediately, so an undeclared resource, an access other
    // than the declared one, or a ref from another graph fails HERE, naming the pass, the resource and the
    // shader slot. The first failure is kept (GetStatus) and the consumer refuses a block that has one: a
    // renderer does not have to check every call, and a half-bound dispatch is never recorded.
    class PassBindings
    {
    public:
        explicit PassBindings( const PassContext& context );

        PassBindings( const PassBindings& )            = delete;
        PassBindings& operator=( const PassBindings& ) = delete;
        PassBindings( PassBindings&& )                 = delete;
        PassBindings& operator=( PassBindings&& )      = delete;

        // A texture read through @p sampler. @p declared is the access the setup declared for @p texture over
        // @p range (SampledCompute / SampledGraphics). @p range = Mip(m) gives a view of that mip alone (the
        // shader samples it at lod 0), All the whole image.
        PassBindings& Sampled( std::string_view shaderName, TextureRef texture, Access declared,
                               SubresourceRange range, SamplerDesc sampler );
        // A storage image, one mip over every layer. @p declared is StorageWrite or StorageRead as declared
        // for Mip(@p mip).
        PassBindings& Storage( std::string_view shaderName, TextureRef texture, Access declared,
                               uint32_t mip = 0 );
        // A graph buffer bound as a uniform or storage buffer, the whole buffer.
        PassBindings& Uniform( std::string_view shaderName, BufferRef buffer );
        PassBindings& Storage( std::string_view shaderName, BufferRef buffer, Access declared );
        // The push-constant block of the draw / dispatch, copied. Its size is checked against the shader's
        // declared push-constant range by the consumer.
        PassBindings& PushConstants( const void* data, uint32_t size );

        // Success, or the first failed entry: "<pass>: '<shader name>' <- '<resource>': <why>".
        Common::BoolResultStr GetStatus() const;

        const PassContext&            GetContext() const;
        std::span<const BoundTexture> GetTextures() const;
        std::span<const BoundBuffer>  GetBuffers() const;
        std::span<const std::byte>    GetPushConstants() const;

    private:
        const PassContext&        m_Context;
        std::vector<BoundTexture> m_Textures;
        std::vector<BoundBuffer>  m_Buffers;
        std::vector<std::byte>    m_PushConstants;
        std::string               m_FirstError; // empty while every entry resolved
    };
} // namespace Desert::Graphic::RDG
