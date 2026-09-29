#pragma once

#include <Engine/Graphic/RendererTypes.hpp>
#include <Engine/Graphic/DynamicResources.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>

// For DESERT_VERIFY in ShaderDataTypeSize below. Not implicit: this header is reached from
// Geometry/Mesh.hpp by translation units that never include Core.hpp on their own.
#include <Common/Core/Core.hpp>

#include <vector>

namespace Desert::Graphic
{
    enum class ShaderDataType
    {
        None = 0,
        Float,
        Float2,
        Float3,
        Float4,
        Int,
        Int2,
        Int3,
        Int4,
        Bool,
        // Four 8-bit channels read as 0..1 floats (VK_FORMAT_R8G8B8A8_UNORM): the vertex colour stream, as
        // UE's FColor vertex colours.
        UNorm8x4
    };

    inline uint32_t ShaderDataTypeSize( ShaderDataType type )
    {
        switch ( type )
        {
            case ShaderDataType::Float:
            case ShaderDataType::Int:
                return 4;

            case ShaderDataType::Float2:
            case ShaderDataType::Int2:
                return 4 * 2;

            case ShaderDataType::Float3:
            case ShaderDataType::Int3:
                return 4 * 3;

            case ShaderDataType::Float4:
            case ShaderDataType::Int4:
                return 4 * 4;

            case ShaderDataType::Bool:
                return 1;

            case ShaderDataType::UNorm8x4:
                return 4;

            // `None` is the enum's ZERO, so a default-constructed VertexBufferElement carries it. It
            // was the one value with no case and no fallthrough return, which made the whole function
            // fall off its end — undefined behaviour returning whatever the ABI's return register
            // happened to hold, straight into a vertex layout's stride.
            case ShaderDataType::None:
                break;
        }

        // An invariant, not an error channel: the only caller is VertexBufferElement's constructor and
        // every layout in the engine is written by hand with a literal type, so `None` here means
        // engine code built a layout out of an unset attribute. Same treatment the unreachable
        // renderer-API cases in VertexBuffer.cpp already get. Returning 0 quietly would have handed
        // back a zero-stride layout, which is the silent emptiness this project forbids.
        DESERT_VERIFY( false, "ShaderDataTypeSize: ShaderDataType::None has no size — a vertex layout "
                              "was built from an unset attribute type" );
        return 0;
    }

    struct VertexBufferElement
    {
        ShaderDataType Type;
        std::string    Name;
        std::uint32_t  Offset;
        std::uint32_t  Size;
        bool           Normalized;

        VertexBufferElement() = default;

        VertexBufferElement( ShaderDataType type, const std::string& name, bool normalized = false )
             : Type( type ), Name( name ), Offset( 0 ), Size( ShaderDataTypeSize( type ) ),
               Normalized( normalized )
        {
        }

        uint32_t GetComponentCount() const;
    };

    class VertexBufferLayout
    {
    public:
        VertexBufferLayout()
        {
        }
        VertexBufferLayout( const std::initializer_list<VertexBufferElement>& elements ) : m_Elements( elements )
        {
            CalculateOffsetsAndStride();
        }

        inline uint32_t GetStride() const
        {
            return m_Stride;
        }
        inline const std::vector<VertexBufferElement>& GetElements() const
        {
            return m_Elements;
        }

        uint32_t GetElementCount() const
        {
            return (uint32_t)m_Elements.size();
        }

        std::vector<VertexBufferElement>::iterator begin()
        {
            return m_Elements.begin();
        }
        std::vector<VertexBufferElement>::iterator end()
        {
            return m_Elements.end();
        }
        std::vector<VertexBufferElement>::const_iterator begin() const
        {
            return m_Elements.begin();
        }
        std::vector<VertexBufferElement>::const_iterator end() const
        {
            return m_Elements.end();
        }

    private:
        void CalculateOffsetsAndStride()
        {
            std::uint32_t offset = 0;
            m_Stride             = 0;
            for ( auto& element : m_Elements )
            {
                element.Offset = offset;
                offset += element.Size;
                m_Stride += element.Size;
            }
        }

        std::vector<VertexBufferElement> m_Elements;
        std::uint32_t                    m_Stride = 0;

    public:
        // THE OPTIONAL VERTEX STREAMS (UE FVertexFactory's Color / TexCoord1 streams): a second vertex binding
        // (binding 1), read from `firstLocation` on, that a mesh may or may not carry. A mesh without it is
        // drawn from one shared buffer at stride 0 — every vertex reads the same default — which in Vulkan is
        // a different vertex-input state, so the backend builds the pipeline twice (VulkanPipeline) and the
        // draw picks the variant by the mesh (VulkanRendererAPI::RenderMesh). Empty = no binding 1 at all.
        VertexBufferLayout& WithStreams( const uint32_t firstLocation,
                                         const std::initializer_list<VertexBufferElement>& elements )
        {
            m_StreamElements      = elements;
            m_StreamFirstLocation = firstLocation;
            m_StreamStride        = 0;
            for ( auto& element : m_StreamElements )
            {
                element.Offset = m_StreamStride;
                m_StreamStride += element.Size;
            }
            return *this;
        }
        [[nodiscard]] bool HasStreams() const
        {
            return !m_StreamElements.empty();
        }
        [[nodiscard]] const std::vector<VertexBufferElement>& GetStreamElements() const
        {
            return m_StreamElements;
        }
        [[nodiscard]] uint32_t GetStreamStride() const
        {
            return m_StreamStride;
        }
        [[nodiscard]] uint32_t GetStreamFirstLocation() const
        {
            return m_StreamFirstLocation;
        }

    private:
        std::vector<VertexBufferElement> m_StreamElements;
        std::uint32_t                    m_StreamStride        = 0;
        std::uint32_t                    m_StreamFirstLocation = 0;
    };

    class VertexBuffer : public DynamicResources
    {
    public:
        // The ledger row, opened here so that every backend's vertex buffer is counted without any of them
        // remembering to. See Engine/Graphic/ResourceLedger.hpp.
        VertexBuffer() : m_Accounting( ResourceOwnership::Take( ResourceKind::VertexBuffer ) )
        {
        }

        virtual ~VertexBuffer() = default;

        /// Overwrite @p size bytes at @p offset. Refuses, having written nothing, when the buffer is not
        /// dynamic, when its memory is not mapped, or when the range does not fit.
        ///
        /// IT ANSWERS, AND THAT IS THE WHOLE POINT OF THIS SIGNATURE. It was `void` until Г13, which
        /// meant the refusal Г7-C had just made honest one layer down (Graphic/MappedMemory.hpp) could
        /// only ever reach a LOG_ERROR: the frame was then drawn from vertices that are not there and
        /// nothing above had anything to ask. A contract guarded by "somebody will read the log" is not
        /// guarded. NO_DISCARD because a caller who wants to ignore it must say so in writing.
        NO_DISCARD virtual Common::BoolResultStr SetData( void* data, uint32_t size, uint32_t offset = 0 ) = 0;

        void ClaimOwnership( const ResourceOwner owner, const Common::AssetHandle asset = Common::AssetHandle{} )
        {
            m_Accounting.Claim( owner, asset );
        }

        /// What this buffer costs on the device. Recorded by whoever knows — the base cannot ask
        /// GetSize() from its own constructor, the backend has not allocated yet at that point.
        void RecordDeviceBytes( const std::size_t bytes )
        {
            m_Accounting.RecordBytes( bytes );
        }

        [[nodiscard]] virtual unsigned int GetSize() const = 0;

        [[nodiscard]] virtual Common::BoolResultStr RT_Invalidate() = 0;

        static std::shared_ptr<VertexBuffer> Create( void* data, uint32_t size,
                                                     BufferUsage usage = BufferUsage::Static );
        static std::shared_ptr<VertexBuffer> Create( uint32_t size, BufferUsage usage = BufferUsage::Dynamic );

    private:
        ResourceOwnership m_Accounting;
    };
} // namespace Desert::Graphic