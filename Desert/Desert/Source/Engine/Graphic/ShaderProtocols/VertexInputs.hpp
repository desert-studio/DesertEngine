#pragma once

#include <Engine/Graphic/ShaderDataType.hpp>
#include <Engine/Graphic/ShaderProtocols/Std140Block.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>

// ONE TRUTH FOR A VERTEX INPUT (UE's FVertexFactory declaration + FVertexFactoryInput): the C++ list is the source
// and the shader's `layout( location = N ) in` lines are GENERATED from it, the way the uniform blocks are
// (Std140Block.hpp). A vertex format lists its attributes once (an X-macro), the list expands into an array of
// VertexInputField — name, type, location, binding — and:
//
//   * the C++ VertexBufferLayout the pipeline is built with is made from that array (MeshVertexLayout.hpp,
//     Render2D.cpp), so the vertex-input state and the shader declaration cannot disagree;
//   * GenerateVertexInputsGlsl writes the shader's inputs from the same array, and the shader includer serves that
//     text for the format's include path (GeneratedProtocolIncludes.hpp) — no hand-written twin to drift;
//   * the DSL parser refuses a vertex stage that declares its own inputs (`In(N)` or `layout(location) in`), so a
//     second spelling does not compile, and VulkanShaderReflection checks every input a stage reads against the
//     layout field at its location by name and type.
//
// Locations: binding 0 counts from 0 in list order; the optional streams (binding 1) count from their own first
// location, which is part of the format.
namespace Desert::Graphic::ShaderProtocols
{
    /// The GLSL spelling of one vertex attribute type. Deliberately undefined for types no vertex input uses
    /// (`Bool`, `None`): a format listing one does not compile.
    template <ShaderDataType T>
    struct VertexInputGlslType;

#define DESERT_VERTEX_INPUT_TYPE( Type, GlslName )                                                                \
    template <>                                                                                                   \
    struct VertexInputGlslType<ShaderDataType::Type>                                                              \
    {                                                                                                             \
        static constexpr std::string_view Glsl = GlslName;                                                        \
    };

    DESERT_VERTEX_INPUT_TYPE( Float, "float" )
    DESERT_VERTEX_INPUT_TYPE( Float2, "vec2" )
    DESERT_VERTEX_INPUT_TYPE( Float3, "vec3" )
    DESERT_VERTEX_INPUT_TYPE( Float4, "vec4" )
    DESERT_VERTEX_INPUT_TYPE( Int, "int" )
    DESERT_VERTEX_INPUT_TYPE( Int2, "ivec2" )
    DESERT_VERTEX_INPUT_TYPE( Int3, "ivec3" )
    DESERT_VERTEX_INPUT_TYPE( Int4, "ivec4" )
    // RGBA8 UNORM in the buffer, four 0..1 floats in the shader.
    DESERT_VERTEX_INPUT_TYPE( UNorm8x4, "vec4" )
#undef DESERT_VERTEX_INPUT_TYPE

    /// The same spelling for a type known only at run time (a VertexBufferElement's), for the reflection check of a
    /// compiled stage against its layout. Empty for a type no vertex input uses.
    [[nodiscard]] constexpr std::string_view VertexInputGlsl( const ShaderDataType type )
    {
        switch ( type )
        {
            case ShaderDataType::Float: return VertexInputGlslType<ShaderDataType::Float>::Glsl;
            case ShaderDataType::Float2: return VertexInputGlslType<ShaderDataType::Float2>::Glsl;
            case ShaderDataType::Float3: return VertexInputGlslType<ShaderDataType::Float3>::Glsl;
            case ShaderDataType::Float4: return VertexInputGlslType<ShaderDataType::Float4>::Glsl;
            case ShaderDataType::Int: return VertexInputGlslType<ShaderDataType::Int>::Glsl;
            case ShaderDataType::Int2: return VertexInputGlslType<ShaderDataType::Int2>::Glsl;
            case ShaderDataType::Int3: return VertexInputGlslType<ShaderDataType::Int3>::Glsl;
            case ShaderDataType::Int4: return VertexInputGlslType<ShaderDataType::Int4>::Glsl;
            case ShaderDataType::UNorm8x4: return VertexInputGlslType<ShaderDataType::UNorm8x4>::Glsl;
            case ShaderDataType::Bool:
            case ShaderDataType::None: break;
        }
        return {};
    }

    /// One attribute of a vertex format: what the shader calls it and reads it as, and where the pipeline feeds
    /// it.
    struct VertexInputField
    {
        std::string_view Name;
        ShaderDataType   Type = ShaderDataType::None;
        std::string_view Glsl;
        uint32_t         Location   = 0;
        uint32_t         Binding    = 0;
        bool             Normalized = false;
    };

    /// One entry of a format's X-macro before locations are assigned.
    struct VertexInputSpec
    {
        std::string_view Name;
        ShaderDataType   Type = ShaderDataType::None;
        std::string_view Glsl;
        bool             Normalized = false;
    };

    /// The X-macro's FIELD( Type, name ) / FIELD_NORMALIZED( Type, name ) expansions.
#define DESERT_VERTEX_INPUT_SPEC_( Type, Name )                                                                   \
    ::Desert::Graphic::ShaderProtocols::VertexInputSpec{                                                          \
         #Name, ::Desert::Graphic::ShaderDataType::Type,                                                          \
         ::Desert::Graphic::ShaderProtocols::VertexInputGlslType<::Desert::Graphic::ShaderDataType::Type>::Glsl,  \
         false },
#define DESERT_VERTEX_INPUT_SPEC_NORMALIZED_( Type, Name )                                                        \
    ::Desert::Graphic::ShaderProtocols::VertexInputSpec{                                                          \
         #Name, ::Desert::Graphic::ShaderDataType::Type,                                                          \
         ::Desert::Graphic::ShaderProtocols::VertexInputGlslType<::Desert::Graphic::ShaderDataType::Type>::Glsl,  \
         true },
#define DESERT_VERTEX_INPUT_SPECS( FIELDS )                                                                       \
    std::array                                                                                                    \
    {                                                                                                             \
        FIELDS( DESERT_VERTEX_INPUT_SPEC_, DESERT_VERTEX_INPUT_SPEC_NORMALIZED_ )                                 \
    }

    /// A vertex format: its binding-0 attributes at locations 0.., then its streams (binding 1) from
    /// @p streamFirstLocation.
    template <std::size_t VertexCount, std::size_t StreamCount>
    [[nodiscard]] constexpr std::array<VertexInputField, VertexCount + StreamCount>
    MakeVertexInputs( const std::array<VertexInputSpec, VertexCount>& vertex,
                      const std::array<VertexInputSpec, StreamCount>& streams, const uint32_t streamFirstLocation )
    {
        std::array<VertexInputField, VertexCount + StreamCount> fields{};
        for ( std::size_t i = 0; i < VertexCount; ++i )
            fields[i] = { vertex[i].Name,      vertex[i].Type, vertex[i].Glsl, static_cast<uint32_t>( i ), 0u,
                          vertex[i].Normalized };
        for ( std::size_t i = 0; i < StreamCount; ++i )
            fields[VertexCount + i] = { streams[i].Name,
                                        streams[i].Type,
                                        streams[i].Glsl,
                                        streamFirstLocation + static_cast<uint32_t>( i ),
                                        1u,
                                        streams[i].Normalized };
        return fields;
    }

    template <std::size_t VertexCount>
    [[nodiscard]] constexpr std::array<VertexInputField, VertexCount>
    MakeVertexInputs( const std::array<VertexInputSpec, VertexCount>& vertex )
    {
        std::array<VertexInputField, VertexCount> fields{};
        for ( std::size_t i = 0; i < VertexCount; ++i )
            fields[i] = { vertex[i].Name,      vertex[i].Type, vertex[i].Glsl, static_cast<uint32_t>( i ), 0u,
                          vertex[i].Normalized };
        return fields;
    }

    /// True when no two attributes share a location or a name — a stream first location inside binding 0's range
    /// breaks it.
    [[nodiscard]] constexpr bool VertexInputsAreDistinct( const std::span<const VertexInputField> fields )
    {
        for ( std::size_t i = 0; i < fields.size(); ++i )
            for ( std::size_t j = i + 1; j < fields.size(); ++j )
                if ( fields[i].Location == fields[j].Location || fields[i].Name == fields[j].Name )
                    return false;
        return true;
    }

    /// The field fed at @p location, or nullptr.
    [[nodiscard]] constexpr const VertexInputField*
    FindVertexInput( const std::span<const VertexInputField> fields, const uint32_t location )
    {
        for ( const VertexInputField& field : fields )
            if ( field.Location == location )
                return &field;
        return nullptr;
    }

    /// `layout( location = N ) in <type> <name>;` for every attribute, in list order.
    [[nodiscard]] inline std::string GenerateVertexInputsGlsl( const std::span<const VertexInputField> fields )
    {
        std::string glsl;
        for ( const VertexInputField& field : fields )
            glsl += std::format( "layout( location = {} ) in {} {};\n", field.Location, field.Glsl, field.Name );
        return glsl;
    }

    // ---------------------------------------------------------------------------------------------------------
    // THE MESH VERTEX (Desert::Vertex, Geometry/MeshTypes.hpp) — every mesh pipeline MeshRenderer builds, the 3D
    // text quad, the overdraw / silhouette re-draws. Binding 0 is the mesh's own vertices; the skinned path adds
    // its bone indices / weights; binding 1 is the optional streams (MeshVertexStreams): vertex colour (RGBA8
    // UNORM) and UV1, from kMeshVertexStreamFirstLocation, always bound (a mesh without them binds the shared
    // default buffer, MeshVertexLayout.hpp DefaultVertexStreamsFor).
    // ---------------------------------------------------------------------------------------------------------
#define DESERT_MESH_VERTEX_FIELDS( FIELD, FIELD_NORMALIZED )                                                      \
    FIELD( Float3, a_Position )                                                                                   \
    FIELD( Float3, a_Normal )                                                                                     \
    FIELD( Float3, a_Tangent )                                                                                    \
    FIELD( Float3, a_Bitangent )                                                                                  \
    FIELD( Float2, a_TextureCoord )
#define DESERT_MESH_SKINNED_VERTEX_FIELDS( FIELD, FIELD_NORMALIZED )                                              \
    DESERT_MESH_VERTEX_FIELDS( FIELD, FIELD_NORMALIZED )                                                          \
    FIELD( Int4, a_BoneIndices )                                                                                  \
    FIELD( Float4, a_BoneWeights )
#define DESERT_MESH_VERTEX_STREAM_FIELDS( FIELD, FIELD_NORMALIZED )                                               \
    FIELD_NORMALIZED( UNorm8x4, a_Color )                                                                         \
    FIELD( Float2, a_TexCoord1 )

    inline constexpr uint32_t kMeshVertexStreamFirstLocation = 7;

    inline constexpr auto kMeshVertexInputs = MakeVertexInputs(
         DESERT_VERTEX_INPUT_SPECS( DESERT_MESH_VERTEX_FIELDS ),
         DESERT_VERTEX_INPUT_SPECS( DESERT_MESH_VERTEX_STREAM_FIELDS ), kMeshVertexStreamFirstLocation );
    inline constexpr auto kMeshSkinnedVertexInputs = MakeVertexInputs(
         DESERT_VERTEX_INPUT_SPECS( DESERT_MESH_SKINNED_VERTEX_FIELDS ),
         DESERT_VERTEX_INPUT_SPECS( DESERT_MESH_VERTEX_STREAM_FIELDS ), kMeshVertexStreamFirstLocation );
    static_assert( VertexInputsAreDistinct( kMeshVertexInputs ) &&
                        VertexInputsAreDistinct( kMeshSkinnedVertexInputs ),
                   "the mesh streams' first location falls inside binding 0's attributes" );

    inline constexpr std::string_view kMeshVertexInputsInclude        = "Mesh/MeshVertexInputs.glslh";
    inline constexpr std::string_view kMeshSkinnedVertexInputsInclude = "Mesh/MeshVertexInputsSkinned.glslh";
    inline constexpr std::string_view kMeshVertexAccessInclude        = "Mesh/MeshVertexAccess.glslh";

    /// THE ONE MESH UV CONVENTION, beside the attribute it reads. A mesh vertex stores its texture coordinates as
    /// imported (origin bottom-left, V up); a texture is addressed from its top-left. Every reader of a mesh
    /// vertex's UV turns the stored value into the texture's coordinate with this function: the raster vertex
    /// paths (through the vertex inputs, which carry it) and the ray-traced readers that fetch the vertex from its
    /// buffer (Mesh/Surface/Pass_RayHit.glslh, Common/RayQueryVisibility.glslh, through kMeshVertexAccessInclude).
    /// A second spelling of it is how the hit group sampled an atlas mirrored (PT-PARITY-2). Not for geometry with
    /// its own vertex convention (landscape tiles); 3D text stores this one (TextECSSystem).
    [[nodiscard]] inline std::string MeshVertexAccessGlsl()
    {
        return "#ifndef DESERT_MESH_VERTEX_ACCESS\n#define DESERT_MESH_VERTEX_ACCESS\n"
               "vec2 DesertMeshTextureUV( vec2 stored )\n{\n    return vec2( stored.x, 1.0 - stored.y );\n}\n"
               "#endif\n";
    }

    [[nodiscard]] inline const std::string& MeshVertexAccessIncludeGlsl()
    {
        static const std::string glsl =
             GenerateGuardedGlsl( "DESERT_MESH_VERTEX_ACCESS_GLSLH",
                                  "Engine/Graphic/ShaderProtocols/VertexInputs.hpp", MeshVertexAccessGlsl() );
        return glsl;
    }

    [[nodiscard]] inline const std::string& MeshVertexInputsGlsl()
    {
        static const std::string glsl = GenerateGuardedGlsl(
             "DESERT_MESH_VERTEX_INPUTS_GLSLH", "Engine/Graphic/ShaderProtocols/VertexInputs.hpp",
             GenerateVertexInputsGlsl( kMeshVertexInputs ) + MeshVertexAccessGlsl() );
        return glsl;
    }

    [[nodiscard]] inline const std::string& MeshSkinnedVertexInputsGlsl()
    {
        static const std::string glsl = GenerateGuardedGlsl(
             "DESERT_MESH_VERTEX_INPUTS_SKINNED_GLSLH", "Engine/Graphic/ShaderProtocols/VertexInputs.hpp",
             GenerateVertexInputsGlsl( kMeshSkinnedVertexInputs ) + MeshVertexAccessGlsl() );
        return glsl;
    }

    // ---------------------------------------------------------------------------------------------------------
    // THE 2D VERTEX (Render2D::Vertex2D) — the batcher's pixel-space corners, drawn by every UI pipeline.
    // ---------------------------------------------------------------------------------------------------------
#define DESERT_UI_VERTEX_FIELDS( FIELD, FIELD_NORMALIZED )                                                        \
    FIELD( Float2, a_Position )                                                                                   \
    FIELD( Float2, a_TexCoord )                                                                                   \
    FIELD( Float4, a_Color )

    inline constexpr auto kUIVertexInputs =
         MakeVertexInputs( DESERT_VERTEX_INPUT_SPECS( DESERT_UI_VERTEX_FIELDS ) );
    static_assert( VertexInputsAreDistinct( kUIVertexInputs ) );

    inline constexpr std::string_view kUIVertexInputsInclude = "Common/UIVertexInputs.glslh";

    [[nodiscard]] inline const std::string& UIVertexInputsGlsl()
    {
        static const std::string glsl = GenerateGuardedGlsl( "DESERT_UI_VERTEX_INPUTS_GLSLH",
                                                             "Engine/Graphic/ShaderProtocols/VertexInputs.hpp",
                                                             GenerateVertexInputsGlsl( kUIVertexInputs ) );
        return glsl;
    }

    /// True for a name a generated vertex format owns. The shader includer refuses a header that declares one as
    /// an input by hand: that would be a second spelling of the format beside the generated one.
    [[nodiscard]] constexpr bool IsGeneratedVertexInputName( const std::string_view name )
    {
        for ( const VertexInputField& field : kMeshSkinnedVertexInputs )
            if ( field.Name == name )
                return true;
        for ( const VertexInputField& field : kUIVertexInputs )
            if ( field.Name == name )
                return true;
        return false;
    }
} // namespace Desert::Graphic::ShaderProtocols
