#pragma once

#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Graphic/RDG/RDGAccess.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

// Resource descriptions, handles and externally owned resources of the render graph. Device-free: a
// TextureDesc is what the graph needs to reason about subresources, lifetimes and memory, not what a
// backend needs to create an image (usage flags are DERIVED from the declared accesses by Compile, so
// they are not a second, hand-written copy of the same fact here).
namespace Desert::Graphic::RDG
{
    struct Extent3D
    {
        uint32_t Width  = 1;
        uint32_t Height = 1;
        uint32_t Depth  = 1;

        constexpr bool operator==( const Extent3D& ) const = default;
    };

    enum class TextureDim : uint8_t
    {
        Tex2D, // Layers >= 1 makes it an array (shadow cascades are layers of one 2D texture)
        Tex3D, // a volume: Layers must be 1, the mip chain spans all three extents
        Cube,  // Layers is a multiple of six
    };

    struct TextureDesc
    {
        Extent3D                   Size;
        Core::Formats::ImageFormat Format = Core::Formats::ImageFormat::RGBA8F;
        uint32_t                   Mips   = 1;
        uint32_t                   Layers = 1;
        TextureDim                 Dim    = TextureDim::Tex2D;

        [[nodiscard]] constexpr uint32_t SubresourceCount() const
        {
            return Mips * Layers;
        }
        // Subresources are numbered layer-major, the order a per-layer (cascade) walk visits them.
        [[nodiscard]] constexpr uint32_t SubresourceIndex( uint32_t mip, uint32_t layer ) const
        {
            return layer * Mips + mip;
        }
    };

    struct BufferDesc
    {
        uint64_t Bytes = 0;
    };

    inline constexpr uint32_t kAllRemaining = std::numeric_limits<uint32_t>::max();

    // A rectangle of mips x layers. kAllRemaining counts extend to the end of the resource; the builder
    // resolves them against the TextureDesc when the use is declared, so Compile only ever sees
    // concrete ranges.
    struct SubresourceRange
    {
        uint32_t BaseMip    = 0;
        uint32_t MipCount   = kAllRemaining;
        uint32_t BaseLayer  = 0;
        uint32_t LayerCount = kAllRemaining;

        static constexpr SubresourceRange All()
        {
            return {};
        }
        static constexpr SubresourceRange Mip( uint32_t mip )
        {
            return { mip, 1, 0, kAllRemaining };
        }
        static constexpr SubresourceRange Layer( uint32_t layer )
        {
            return { 0, kAllRemaining, layer, 1 };
        }
        static constexpr SubresourceRange MipLayer( uint32_t mip, uint32_t layer )
        {
            return { mip, 1, layer, 1 };
        }

        constexpr bool operator==( const SubresourceRange& ) const = default;
    };

    inline constexpr uint32_t kInvalidResource = std::numeric_limits<uint32_t>::max();

    // Handles are indices into ONE builder's resource table; they mean nothing to another builder.
    struct TextureRef
    {
        uint32_t Index = kInvalidResource;

        [[nodiscard]] constexpr bool IsValid() const
        {
            return Index != kInvalidResource;
        }
        constexpr bool operator==( const TextureRef& ) const = default;
    };

    struct BufferRef
    {
        uint32_t Index = kInvalidResource;

        [[nodiscard]] constexpr bool IsValid() const
        {
            return Index != kInvalidResource;
        }
        constexpr bool operator==( const BufferRef& ) const = default;
    };

    // A backend's image / buffer. The core only moves these between the backend, externals and pass
    // bindings; what is inside is the backend's business. An external holds one by shared_ptr, so an
    // extracted transient outlives the graph that created it.
    enum class BackendKind : uint8_t
    {
        Recording, // the suite's fake: records the call sequence, touches no device
        Vulkan,
    };

    class IPhysicalTexture
    {
    public:
        virtual ~IPhysicalTexture()                              = default;
        [[nodiscard]] virtual BackendKind GetBackendKind() const = 0;
    };

    class IPhysicalBuffer
    {
    public:
        virtual ~IPhysicalBuffer()                               = default;
        [[nodiscard]] virtual BackendKind GetBackendKind() const = 0;
    };

    // A texture that outlives the graph (swapchain image, history buffer, a baked cube). It carries its
    // own synchronisation state PER SUBRESOURCE: the graph reads it when the texture is registered and
    // writes the final state back when Execute finishes, so the next graph (next frame, or a GpuBatch)
    // starts from the truth. The state is an AccessState rather than an Access because a graph that ends
    // on merged reads (sampled in fragment AND compute) leaves the texture in a state no single Access
    // names; storing one of them would lose the stages the next frame's barrier must wait on.
    struct ExternalTexture
    {
        TextureDesc              Desc;
        std::vector<AccessState> SubresourceStates; // Desc.SubresourceCount() entries, layer-major
        // The image itself. Set by whoever owns the texture before registering it; for an extraction
        // target, Execute sets it to the transient's image.
        std::shared_ptr<IPhysicalTexture> Physical;

        ExternalTexture() = default;
        ExternalTexture( const TextureDesc& desc, Access initial )
             : Desc( desc ), SubresourceStates( desc.SubresourceCount(), GetAccessState( initial ) )
        {
        }
    };

    struct ExternalBuffer
    {
        BufferDesc                       Desc;
        AccessState                      State;
        std::shared_ptr<IPhysicalBuffer> Physical;

        ExternalBuffer() = default;
        ExternalBuffer( const BufferDesc& desc, Access initial ) : Desc( desc ), State( GetAccessState( initial ) )
        {
        }
    };

    struct ClearValue
    {
        std::array<float, 4> Color   = { 0.0f, 0.0f, 0.0f, 0.0f };
        float                Depth   = 0.0f;
        uint32_t             Stencil = 0;
    };

    enum class LoadAction : uint8_t
    {
        Load,
        Clear,
        DontCare,
    };

    enum class StoreAction : uint8_t
    {
        Store,
        DontCare,
    };

    // What a pass ASKS for on an attachment. Compile may still turn a Load into DontCare (first use of a
    // transient: there is nothing to load), which is why the decision lives in the CompileResult.
    struct LoadOp
    {
        LoadAction Action = LoadAction::Load;
        ClearValue Value;

        static LoadOp Load()
        {
            return {};
        }
        static LoadOp DontCare()
        {
            return { LoadAction::DontCare, {} };
        }
        static LoadOp ClearColor( float r, float g, float b, float a )
        {
            LoadOp op;
            op.Action      = LoadAction::Clear;
            op.Value.Color = { r, g, b, a };
            return op;
        }
        static LoadOp ClearDepth( float depth, uint32_t stencil = 0 )
        {
            LoadOp op;
            op.Action        = LoadAction::Clear;
            op.Value.Depth   = depth;
            op.Value.Stencil = stencil;
            return op;
        }
    };

    enum class PassFlags : uint8_t
    {
        None      = 0,
        Raster    = 1u << 0,
        Compute   = 1u << 1,
        Copy      = 1u << 2,
        NeverCull = 1u << 3, // a culling root even if nothing reads what it writes (readbacks, debug capture)
        // Old code recorded through Builder::AddLegacyPass: it records its own render passes and expects
        // every image it touches in SHADER_READ_ONLY before and leaves them there. Removed in RDG-Z.
        Legacy = 1u << 4,
    };

    constexpr PassFlags operator|( PassFlags a, PassFlags b )
    {
        return static_cast<PassFlags>( static_cast<uint8_t>( a ) | static_cast<uint8_t>( b ) );
    }

    constexpr bool HasFlag( PassFlags flags, PassFlags flag )
    {
        return ( static_cast<uint8_t>( flags ) & static_cast<uint8_t>( flag ) ) != 0;
    }

    // Transient memory is packed per class: images and buffers never share a heap here, which keeps the
    // device-free plan valid under any bufferImageGranularity the device later reports.
    enum class MemoryClass : uint8_t
    {
        Texture,
        Buffer,
        Count
    };

    inline constexpr uint32_t kMemoryClassCount = static_cast<uint32_t>( MemoryClass::Count );
} // namespace Desert::Graphic::RDG
