#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>

// ONE LAYOUT TRUTH FOR A UNIFORM BLOCK (UE's BEGIN_UNIFORM_BUFFER_STRUCT / FShaderParametersMetadata): the C++
// struct is the source and the GLSL declaration is GENERATED from it. A protocol lists its members once (an
// X-macro in its own header), the list expands into the struct's members AND into an array of Std140Field, and:
//
//   * Std140LayoutMatches checks — at compile time, by static_assert next to the struct — that every member
//     sits at the offset std140 gives it and that sizeof equals the block size, so a C++ member order or type
//     std140 would place differently is a BUILD error, not a test that may be switched off;
//   * GenerateUniformBlockGlsl writes the block's GLSL from the same array, and the shader includer serves
//     that text for the block's include path (GeneratedProtocolIncludes.hpp) — no hand-written twin to drift.
namespace Desert::Graphic::ShaderProtocols
{
    /// The std140 size / base alignment and GLSL spelling of one C++ member type. Deliberately undefined for every
    /// type not listed: a member whose std140 placement nobody wrote down does not compile.
    template <typename T>
    struct Std140Type;

#define DESERT_STD140_TYPE( CppType, GlslName, ByteSize, BaseAlign )                                              \
    template <>                                                                                                   \
    struct Std140Type<CppType>                                                                                    \
    {                                                                                                             \
        static constexpr std::string_view Glsl  = GlslName;                                                       \
        static constexpr std::size_t      Size  = ByteSize;                                                       \
        static constexpr std::size_t      Align = BaseAlign;                                                      \
    };

    DESERT_STD140_TYPE( float, "float", 4, 4 )
    DESERT_STD140_TYPE( int32_t, "int", 4, 4 )
    DESERT_STD140_TYPE( uint32_t, "uint", 4, 4 )
    DESERT_STD140_TYPE( glm::vec2, "vec2", 8, 8 )
    DESERT_STD140_TYPE( glm::vec3, "vec3", 12, 16 )
    DESERT_STD140_TYPE( glm::vec4, "vec4", 16, 16 )
    DESERT_STD140_TYPE( glm::ivec2, "ivec2", 8, 8 )
    DESERT_STD140_TYPE( glm::ivec4, "ivec4", 16, 16 )
    DESERT_STD140_TYPE( glm::uvec2, "uvec2", 8, 8 )
    DESERT_STD140_TYPE( glm::uvec4, "uvec4", 16, 16 )
    DESERT_STD140_TYPE( glm::mat4, "mat4", 64, 16 )
#undef DESERT_STD140_TYPE

    /// One member of a uniform block: its GLSL type and name, where the C++ struct holds it, and where std140
    /// wants it.
    struct Std140Field
    {
        std::string_view Glsl;
        std::string_view Name;
        std::size_t      Offset = 0; // offsetof in the C++ struct
        std::size_t      Size   = 0;
        std::size_t      Align  = 0;
    };

    template <typename T>
    [[nodiscard]] constexpr Std140Field MakeStd140Field( const std::string_view name, const std::size_t offset )
    {
        static_assert( sizeof( T ) == Std140Type<T>::Size,
                       "the C++ type is not the size std140 gives its GLSL twin" );
        return Std140Field{ Std140Type<T>::Glsl, name, offset, Std140Type<T>::Size, Std140Type<T>::Align };
    }

    [[nodiscard]] constexpr std::size_t Std140AlignUp( const std::size_t value, const std::size_t alignment )
    {
        return ( value + alignment - 1 ) / alignment * alignment;
    }

    /// True when every field sits at its std140 offset (in list order) and the block — rounded to its 16-byte base
    /// alignment — is exactly @p structSize bytes. A dropped, reordered or retyped member breaks one of the two.
    [[nodiscard]] constexpr bool Std140LayoutMatches( const std::span<const Std140Field> fields,
                                                      const std::size_t                  structSize )
    {
        std::size_t offset = 0;
        for ( const Std140Field& field : fields )
        {
            offset = Std140AlignUp( offset, field.Align );
            if ( field.Offset != offset )
                return false;
            offset += field.Size;
        }
        return Std140AlignUp( offset, 16 ) == structSize;
    }

    /// The GLSL of a uniform block, generated from its field list. @p binding is the Desert layout sugar's slot
    /// (`Uniform(N)`), @p guard the include guard, @p source the C++ header named in the banner.
    [[nodiscard]] inline std::string
    GenerateUniformBlockGlsl( const std::string_view guard, const std::string_view source, const unsigned binding,
                              const std::string_view blockName, const std::string_view instanceName,
                              const std::span<const Std140Field> fields )
    {
        std::string glsl = std::format( "#ifndef {0}\n#define {0}\n\n"
                                        "// GENERATED from {1} by the shader includer; there is no file on disk.\n"
                                        "Uniform({2}) {3} {{\n",
                                        guard, source, binding, blockName );
        for ( const Std140Field& field : fields )
            glsl += std::format( "    {} {};\n", field.Glsl, field.Name );
        glsl += std::format( "}} {};\n\n#endif\n", instanceName );
        return glsl;
    }
} // namespace Desert::Graphic::ShaderProtocols
