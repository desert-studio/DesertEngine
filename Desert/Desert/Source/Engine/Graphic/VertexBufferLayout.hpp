#pragma once

#include <Engine/Graphic/ShaderDataType.hpp>
#include <Engine/Graphic/ShaderProtocols/VertexInputs.hpp>

// For DESERT_VERIFY in ShaderDataTypeSize below. Not implicit: this header is reached by translation units
// that never include Core.hpp on their own.
#include <Common/Core/Core.hpp>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <vector>

// A vertex format, apart from the buffer that holds it: a pipeline description names its input layout
// without seeing the GPU buffer, its ledger row or its dynamic-resource base (UE: FVertexDeclarationElementList
// beside FVertexBuffer).
namespace Desert::Graphic
{
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

        /// THE LAYOUT OF A GENERATED VERTEX FORMAT (ShaderProtocols/VertexInputs.hpp): binding 0's attributes in
        /// location order, then the streams (binding 1) from their first location. The shader's inputs are
        /// generated from the same fields, so this is the only way a pipeline is handed a format's layout.
        [[nodiscard]] static VertexBufferLayout
        FromVertexInputs( const std::span<const ShaderProtocols::VertexInputField> fields )
        {
            VertexBufferLayout layout;
            for ( const ShaderProtocols::VertexInputField& field : fields )
            {
                VertexBufferElement element( field.Type, std::string( field.Name ), field.Normalized );
                if ( field.Binding == 0 )
                {
                    DESERT_VERIFY( field.Location == layout.m_Elements.size(),
                                   "a vertex format's binding-0 attributes are not at consecutive locations" );
                    layout.m_Elements.push_back( element );
                    continue;
                }
                if ( field.Binding == ShaderProtocols::kMeshPreviousPositionBinding )
                {
                    // The previous positions: their own buffer of one float3 per vertex, offset 0, read at
                    // ShaderProtocols::kMeshPreviousPositionStride (UE's previous position stream).
                    DESERT_VERIFY( !layout.m_PreviousPositionLocation.has_value() &&
                                        field.Type == ShaderDataType::Float3,
                                   "a vertex format's previous positions are not one Float3" );
                    layout.m_PreviousPositionLocation = field.Location;
                    element.Offset                    = 0;
                    layout.m_PreviousPosition         = element;
                    continue;
                }
                if ( layout.m_StreamElements.empty() )
                    layout.m_StreamFirstLocation = field.Location;
                DESERT_VERIFY( field.Binding == 1 && field.Location == layout.m_StreamFirstLocation +
                                                                             layout.m_StreamElements.size(),
                               "a vertex format's streams are not binding 1 at consecutive locations" );
                element.Offset = layout.m_StreamStride;
                layout.m_StreamStride += element.Size;
                layout.m_StreamElements.push_back( element );
            }
            layout.CalculateOffsetsAndStride();
            return layout;
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
        // (binding 1), read from `firstLocation` on at the streams' stride, that a mesh may or may not carry.
        // A mesh without it binds one shared default buffer at the same stride, as long as the largest mesh
        // (VulkanRendererAPI::RenderMesh, MeshVertexLayout.hpp DefaultVertexStreamsFor) — one vertex-input
        // state, one pipeline. Empty = no binding 1 at all.
        VertexBufferLayout& WithStreams( const uint32_t                                    firstLocation,
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

        // THE PREVIOUS-POSITION BINDING (ShaderProtocols::kMeshPreviousPositionBinding, the skinned format only):
        // one Float3 per vertex, the position the vertex had last frame, read at GetPreviousPositionLocation(),
        // offset 0, at kMeshPreviousPositionStride - its own buffer, never this layout's vertices. A mesh without
        // one (MeshGeometry::PreviousVertices null) binds its own vertices there and its row says the stage must
        // not read them (GpuDriven::PrimitiveFlags::kPreviousPositions clear).
        [[nodiscard]] std::optional<uint32_t> GetPreviousPositionLocation() const
        {
            return m_PreviousPositionLocation;
        }
        // Meaningful only when GetPreviousPositionLocation() has a value.
        [[nodiscard]] const VertexBufferElement& GetPreviousPositionElement() const
        {
            return m_PreviousPosition;
        }

    private:
        std::vector<VertexBufferElement> m_StreamElements;
        std::uint32_t                    m_StreamStride        = 0;
        std::uint32_t                    m_StreamFirstLocation = 0;
        std::optional<uint32_t>          m_PreviousPositionLocation;
        VertexBufferElement              m_PreviousPosition;
    };
} // namespace Desert::Graphic
