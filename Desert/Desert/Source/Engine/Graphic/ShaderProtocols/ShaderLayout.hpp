#pragma once
// THE layout of one GPU struct, declared once in C++ (X-macro) and checked against GLSL std140/std430 at compile
// time. Tools/ShaderProtocolGen turns these declarations into GLSL at build time; nothing here formats text.
#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace Desert::Graphic::ShaderProtocols
{
    enum class LayoutRule : uint8_t
    {
        Std140, // uniform block member list
        Std430, // storage block member list / runtime-array element
        Both    // used in both (checked against each)
    };

    struct ShaderStructDecl;

    struct ShaderField
    {
        std::string_view        Glsl;             // GLSL type: "float".."mat4", or Nested->GlslName
        std::string_view        Name;             // GLSL member name (the X-macro's GLSL column)
        uint32_t                Offset       = 0; // offsetof in the C++ struct
        uint32_t                ElementSize  = 0; // sizeof one element
        uint32_t                Count        = 0; // 0 = not an array, N = [N]
        uint32_t                BaseAlign140 = 0; // GLSL base alignment of one element, std140
        uint32_t                BaseAlign430 = 0; // same, std430
        const ShaderStructDecl* Nested       = nullptr;
    };

    struct ShaderStructDecl
    {
        std::string_view             GlslName;
        std::span<const ShaderField> Fields;
        uint32_t                     CppSize  = 0;
        uint32_t                     CppAlign = 0;
        LayoutRule                   Rule     = LayoutRule::Std430;
    };

    // GLSL type of a C++ member type. bool is refused (GLSL bool is 4 bytes, C++ 1): use uint32_t.
    template <class T>
    struct GlslTypeOf;
    template <>
    struct GlslTypeOf<bool>; // declared, never defined: a bool member does not compile
#define DESERT_GLSL_TYPE( Cpp, GlslName, ByteSize, A140, A430 )                                                   \
    template <>                                                                                                   \
    struct GlslTypeOf<Cpp>                                                                                        \
    {                                                                                                             \
        static constexpr std::string_view        Glsl = GlslName;                                                 \
        static constexpr uint32_t                Size = ByteSize, Align140 = A140, Align430 = A430;               \
        static constexpr const ShaderStructDecl* Nested = nullptr;                                                \
    };
    DESERT_GLSL_TYPE( float, "float", 4, 4, 4 )
    DESERT_GLSL_TYPE( int32_t, "int", 4, 4, 4 )
    DESERT_GLSL_TYPE( uint32_t, "uint", 4, 4, 4 )
    DESERT_GLSL_TYPE( glm::vec2, "vec2", 8, 8, 8 )
    DESERT_GLSL_TYPE( glm::vec3, "vec3", 12, 16, 16 )
    DESERT_GLSL_TYPE( glm::vec4, "vec4", 16, 16, 16 )
    DESERT_GLSL_TYPE( glm::ivec2, "ivec2", 8, 8, 8 )
    DESERT_GLSL_TYPE( glm::ivec3, "ivec3", 12, 16, 16 )
    DESERT_GLSL_TYPE( glm::ivec4, "ivec4", 16, 16, 16 )
    DESERT_GLSL_TYPE( glm::uvec2, "uvec2", 8, 8, 8 )
    DESERT_GLSL_TYPE( glm::uvec3, "uvec3", 12, 16, 16 )
    DESERT_GLSL_TYPE( glm::uvec4, "uvec4", 16, 16, 16 )
    DESERT_GLSL_TYPE( glm::mat4, "mat4", 64, 16, 16 )
#undef DESERT_GLSL_TYPE

    // A DESERT_SHADER_STRUCT type: found through ADL on std::type_identity<T> (the macro defines ShaderStructOf).
    template <class T>
        requires requires { ShaderStructOf( std::type_identity<T>{} ); }
    struct GlslTypeOf<T>
    {
        static constexpr const ShaderStructDecl* Nested   = &ShaderStructOf( std::type_identity<T>{} );
        static constexpr std::string_view        Glsl     = Nested->GlslName;
        static constexpr uint32_t                Size     = Nested->CppSize;
        static constexpr uint32_t                Align140 = 16; // std140: struct alignment rounds up to vec4
        static constexpr uint32_t Align430 = 0; // std430: max member alignment, computed by StructBaseAlign430
    };

    template <class T, std::size_t N>
    struct GlslTypeOf<std::array<T, N>> : GlslTypeOf<T>
    {
        static constexpr uint32_t Count = static_cast<uint32_t>( N );
    };

    template <class T>
    consteval ShaderField MakeShaderField( std::string_view name, std::size_t offset )
    {
        using G        = GlslTypeOf<T>;
        uint32_t count = 0;
        if constexpr ( requires { G::Count; } )
            count = G::Count;
        // The C++ element is the size its GLSL type is (a struct: its sizeof), so the C++ array stride is the
        // element size and ShaderLayoutMatches compares it with the GLSL stride.
        static_assert( sizeof( T ) == static_cast<std::size_t>( G::Size ) * ( count == 0 ? 1u : count ),
                       "the C++ member type is not the size of its GLSL type" );
        return ShaderField{ G::Glsl,     name,     static_cast<uint32_t>( offset ), G::Size, count, G::Align140,
                            G::Align430, G::Nested };
    }

    namespace LayoutDetail
    {
        consteval uint32_t AlignUp( const uint32_t value, const uint32_t alignment )
        {
            return alignment == 0 ? value : ( value + alignment - 1 ) / alignment * alignment;
        }

        consteval uint32_t StructAlign( const ShaderStructDecl& s, LayoutRule rule );
        consteval uint32_t StructSize( const ShaderStructDecl& s, LayoutRule rule );

        // GLSL 4.60 §7.6.2.2: the base alignment of one member (rules 1-3, 9; a nested struct its own), an array's
        // rounded up to a vec4 under std140 (rule 4).
        consteval uint32_t FieldAlign( const ShaderField& f, const LayoutRule rule )
        {
            uint32_t align = f.Nested != nullptr
                                  ? StructAlign( *f.Nested, rule )
                                  : ( rule == LayoutRule::Std140 ? f.BaseAlign140 : f.BaseAlign430 );
            if ( rule == LayoutRule::Std140 && f.Count != 0 )
                align = AlignUp( align, 16 );
            return align;
        }

        // The GLSL size of one element: a scalar/vector/matrix its own, a struct its padded size under `rule`.
        consteval uint32_t ElementSize( const ShaderField& f, const LayoutRule rule )
        {
            return f.Nested != nullptr ? StructSize( *f.Nested, rule ) : f.ElementSize;
        }

        // The array stride (rule 4 / std430): the element size rounded up to the array's base alignment.
        consteval uint32_t ArrayStride( const ShaderField& f, const LayoutRule rule )
        {
            return AlignUp( ElementSize( f, rule ), FieldAlign( f, rule ) );
        }

        consteval uint32_t StructAlign( const ShaderStructDecl& s, const LayoutRule rule )
        {
            uint32_t align = 1;
            for ( const ShaderField& f : s.Fields )
            {
                const uint32_t a = FieldAlign( f, rule );
                align            = a > align ? a : align;
            }
            return rule == LayoutRule::Std140 ? AlignUp( align, 16 ) : align;
        }

        // Offsets in member order; the end rounded up to the struct's base alignment (rule 9).
        consteval uint32_t StructSize( const ShaderStructDecl& s, const LayoutRule rule )
        {
            uint32_t offset = 0;
            for ( const ShaderField& f : s.Fields )
            {
                offset = AlignUp( offset, FieldAlign( f, rule ) );
                offset += f.Count != 0 ? ArrayStride( f, rule ) * f.Count : ElementSize( f, rule );
            }
            return AlignUp( offset, StructAlign( s, rule ) );
        }

        consteval bool LayoutMatchesRule( const ShaderStructDecl& s, const LayoutRule rule )
        {
            uint32_t offset = 0;
            for ( const ShaderField& f : s.Fields )
            {
                offset = AlignUp( offset, FieldAlign( f, rule ) );
                if ( f.Offset != offset )
                    return false;
                if ( f.Nested != nullptr && !LayoutMatchesRule( *f.Nested, rule ) )
                    return false;
                if ( f.Count != 0 )
                {
                    // The C++ element stride is ElementSize (MakeShaderField checks sizeof); GLSL's must equal it.
                    if ( ArrayStride( f, rule ) != f.ElementSize )
                        return false;
                    offset += f.ElementSize * f.Count;
                }
                else
                {
                    if ( ElementSize( f, rule ) != f.ElementSize )
                        return false;
                    offset += f.ElementSize;
                }
            }
            return AlignUp( offset, StructAlign( s, rule ) ) == s.CppSize;
        }
    } // namespace LayoutDetail

    // std430 base alignment of a struct = max member base alignment; std140 rounds it and array strides up to 16.
    consteval uint32_t StructBaseAlign430( const ShaderStructDecl& s )
    {
        return LayoutDetail::StructAlign( s, LayoutRule::Std430 );
    }
    // The offsets GLSL assigns under `rule`, member by member, equal the C++ offsets, every array stride equals
    // the C++ element stride, and the C++ size equals the GLSL size rounded to the struct alignment.
    consteval bool ShaderLayoutMatches( const ShaderStructDecl& s, LayoutRule rule )
    {
        if ( rule == LayoutRule::Both )
            return LayoutDetail::LayoutMatchesRule( s, LayoutRule::Std140 ) &&
                   LayoutDetail::LayoutMatchesRule( s, LayoutRule::Std430 );
        return LayoutDetail::LayoutMatchesRule( s, rule );
    }
} // namespace Desert::Graphic::ShaderProtocols

// FIELDS(X) calls X( CppType, member, "GlslName", initialiser ) once per member.
#define DESERT_SHADER_FIELD_MEMBER( Type, Member, Glsl, Init ) Type Member = Init;
#define DESERT_SHADER_FIELD_DESC( Type, Member, Glsl, Init )                                                      \
    ::Desert::Graphic::ShaderProtocols::MakeShaderField<Type>( Glsl,                                              \
                                                               __builtin_offsetof( DesertShaderSelf, Member ) ),

// Namespace scope only. Name = C++ type, GlslName = GLSL struct name, Rule = LayoutRule::…, extras = more members
// of the C++ struct that are NOT on the GPU (constructors, constexpr statics without storage in the layout).
#define DESERT_SHADER_STRUCT( Name, GlslName, Rule, FIELDS, ... )                                                 \
    struct Name                                                                                                   \
    {                                                                                                             \
        FIELDS( DESERT_SHADER_FIELD_MEMBER )                                                                      \
        __VA_ARGS__                                                                                               \
    };                                                                                                            \
    namespace Name##ShaderLayout                                                                                  \
    {                                                                                                             \
        using DesertShaderSelf                                                     = Name;                        \
        inline constexpr ::Desert::Graphic::ShaderProtocols::ShaderField kFields[] = {                            \
             FIELDS( DESERT_SHADER_FIELD_DESC ) };                                                                \
    }                                                                                                             \
    inline constexpr ::Desert::Graphic::ShaderProtocols::ShaderStructDecl k##Name##Struct{                        \
         GlslName, Name##ShaderLayout::kFields, sizeof( Name ), alignof( Name ),                                  \
         ::Desert::Graphic::ShaderProtocols::LayoutRule::Rule };                                                  \
    consteval const ::Desert::Graphic::ShaderProtocols::ShaderStructDecl& ShaderStructOf(                         \
         std::type_identity<Name> )                                                                               \
    {                                                                                                             \
        return k##Name##Struct;                                                                                   \
    }                                                                                                             \
    static_assert( ::Desert::Graphic::ShaderProtocols::ShaderLayoutMatches(                                       \
                        k##Name##Struct, ::Desert::Graphic::ShaderProtocols::LayoutRule::Rule ),                  \
                   #Name ": the C++ layout is not the GLSL " #Rule " layout" )
