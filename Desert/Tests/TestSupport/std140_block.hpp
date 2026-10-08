#pragma once

// THE std140 LAYOUT OF A GLSL UNIFORM BLOCK, READ FROM THE SHADER TEXT — for C++ <-> GLSL twin censuses.
//
// A C++ struct uploaded whole into a uniform block (SetRawData / a graph buffer) is a second spelling of the
// block; a member reordered or inserted on one side shifts every offset after it and the shader reads the wrong
// matrix without any error. A census parses the block here and compares names + offsets member for member with
// offsetof() of the twin, and the block size with sizeof.
//
// Rules (GLSL 4.50 §7.6.2.2, std140): scalars 4/4; vec2 8/8; vec3 12/16; vec4 16/16; matN = N column vectors of
// vec4 stride; an array's element stride rounds up to 16; the block size rounds up to 16. An unknown member type
// (a nested struct, a double) fails the test instead of guessing.

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace Desert::TestSupport::Std140
{
    struct Member
    {
        std::string Type;
        std::string Name;
        size_t      Offset = 0;
    };

    struct Block
    {
        std::vector<Member> Members;
        size_t              Size = 0; // rounded up to the block's 16-byte base alignment
    };

    inline std::string ReadText( const std::filesystem::path& path )
    {
        const std::ifstream file( path, std::ios::binary );
        EXPECT_TRUE( file.good() ) << "cannot read " << path;
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
    }

    // The text with // and /* */ comments blanked, so a comment naming a member or a block is not parsed.
    inline std::string StripComments( const std::string& text )
    {
        std::string out;
        out.reserve( text.size() );
        for ( size_t i = 0; i < text.size(); )
        {
            if ( text.compare( i, 2, "//" ) == 0 )
            {
                while ( i < text.size() && text[i] != '\n' )
                    ++i;
            }
            else if ( text.compare( i, 2, "/*" ) == 0 )
            {
                const size_t end = text.find( "*/", i + 2 );
                i                = end == std::string::npos ? text.size() : end + 2;
                out += ' ';
            }
            else
            {
                out += text[i++];
            }
        }
        return out;
    }

    inline size_t AlignUp( const size_t value, const size_t alignment )
    {
        return ( value + alignment - 1 ) / alignment * alignment;
    }

    // Size and base alignment of a non-array member of @p type; false for a type these rules do not cover.
    inline bool SizeAndAlignment( const std::string& type, size_t& size, size_t& alignment )
    {
        struct Row
        {
            const char* Type;
            size_t      Size;
            size_t      Alignment;
        };
        static const Row kRows[] = {
             { "float", 4, 4 },   { "int", 4, 4 },     { "uint", 4, 4 },   { "bool", 4, 4 },
             { "vec2", 8, 8 },    { "ivec2", 8, 8 },   { "uvec2", 8, 8 },  { "vec3", 12, 16 },
             { "ivec3", 12, 16 }, { "uvec3", 12, 16 }, { "vec4", 16, 16 }, { "ivec4", 16, 16 },
             { "uvec4", 16, 16 }, { "mat3", 48, 16 },  { "mat4", 64, 16 },
        };
        for ( const Row& row : kRows )
        {
            if ( type == row.Type )
            {
                size      = row.Size;
                alignment = row.Alignment;
                return true;
            }
        }
        return false;
    }

    // Parses the block named @p blockName out of @p shaderText: `Uniform(N) <name> { ... };` in the engine's
    // shader language, `uniform <name> { ... };` in plain GLSL. A missing block or an unknown member type is an
    // ADD_FAILURE and an empty block.
    inline Block ParseBlock( const std::string& shaderText, const std::string& blockName )
    {
        Block             block;
        const std::string code = StripComments( shaderText );
        std::smatch       open;
        if ( !std::regex_search( code, open, std::regex( "\\b" + blockName + "\\s*\\{" ) ) )
        {
            ADD_FAILURE() << "uniform block " << blockName << " not found";
            return block;
        }
        const auto   begin = static_cast<size_t>( open.position( 0 ) + open.length( 0 ) );
        const size_t end   = code.find( '}', begin );
        if ( end == std::string::npos )
        {
            ADD_FAILURE() << "uniform block " << blockName << " is not closed";
            return block;
        }
        const std::string body = code.substr( begin, end - begin );

        const std::regex member( R"(\b([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*(?:\[\s*(\d+)\s*\])?\s*;)" );
        size_t           offset = 0;
        for ( auto it = std::sregex_iterator( body.begin(), body.end(), member ); it != std::sregex_iterator();
              ++it )
        {
            const std::string type      = ( *it )[1];
            size_t            size      = 0;
            size_t            alignment = 0;
            if ( !SizeAndAlignment( type, size, alignment ) )
            {
                ADD_FAILURE() << blockName << ": std140 type '" << type << "' is not handled by this parser";
                return Block{};
            }
            if ( ( *it )[3].matched )
            {
                const size_t count = std::stoul( ( *it )[3].str() );
                alignment          = AlignUp( alignment, 16 );
                size               = AlignUp( size, 16 ) * count;
            }
            offset = AlignUp( offset, alignment );
            block.Members.push_back( { type, ( *it )[2], offset } );
            offset += size;
        }
        block.Size = AlignUp( offset, 16 );
        return block;
    }
} // namespace Desert::TestSupport::Std140
