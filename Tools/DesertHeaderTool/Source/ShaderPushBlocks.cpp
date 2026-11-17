#include "ShaderPushBlocks.hpp"

#include "HeaderScan.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <format>
#include <map>
#include <optional>
#include <string_view>

namespace Desert::HeaderTool
{
    namespace
    {
        // A GLSL member type the push block may hold, with its C++ twin and std430 size/alignment.
        struct PushType
        {
            std::string_view Glsl;
            std::string_view Cpp;
            std::uint32_t    Size;
            std::uint32_t    Align;
        };

        // mat2/mat3 and the bool/double types are absent on purpose: std430 pads a mat3 column to 16 bytes and
        // a bool is 4 bytes in GLSL, so neither has a glm twin of the same bytes; a block using one is an error.
        constexpr PushType kPushTypes[] = {
             { "float", "float", 4u, 4u },        { "int", "std::int32_t", 4u, 4u },
             { "uint", "std::uint32_t", 4u, 4u }, { "vec2", "glm::vec2", 8u, 8u },
             { "vec3", "glm::vec3", 12u, 16u },   { "vec4", "glm::vec4", 16u, 16u },
             { "ivec2", "glm::ivec2", 8u, 8u },   { "ivec3", "glm::ivec3", 12u, 16u },
             { "ivec4", "glm::ivec4", 16u, 16u }, { "uvec2", "glm::uvec2", 8u, 8u },
             { "uvec3", "glm::uvec3", 12u, 16u }, { "uvec4", "glm::uvec4", 16u, 16u },
             { "mat4", "glm::mat4", 64u, 16u },
        };

        // The block labels of a .shader: `Pass "Name" {` and the stage blocks (`Compute {`, `Vertex {`, ...).
        constexpr std::string_view kStages[] = {
             "Vertex", "Fragment",   "Compute", "Geometry",     "Mesh",     "Task",        "RayGen",
             "Miss",   "ClosestHit", "AnyHit",  "Intersection", "Callable", "TessControl", "TessEvaluation" };

        const PushType* FindType( std::string_view glsl ) noexcept
        {
            for ( const PushType& type : kPushTypes )
                if ( type.Glsl == glsl )
                    return &type;
            return nullptr;
        }

        bool IsStage( std::string_view word ) noexcept
        {
            return std::find( std::begin( kStages ), std::end( kStages ), word ) != std::end( kStages );
        }

        bool IdentStart( char c ) noexcept
        {
            return std::isalpha( static_cast<unsigned char>( c ) ) != 0 || c == '_';
        }

        bool IdentChar( char c ) noexcept
        {
            return std::isalnum( static_cast<unsigned char>( c ) ) != 0 || c == '_';
        }

        size_t SkipSpace( const std::string& text, size_t i ) noexcept
        {
            while ( i < text.size() && std::isspace( static_cast<unsigned char>( text[i] ) ) != 0 )
                ++i;
            return i;
        }

        std::string ReadWord( const std::string& text, size_t& i ) noexcept
        {
            const size_t start = i;
            while ( i < text.size() && IdentChar( text[i] ) )
                ++i;
            return text.substr( start, i - start );
        }

        // `"..."` at i (i on the opening quote): its content, i past the closing quote.
        std::optional<std::string> ReadString( const std::string& text, size_t& i ) noexcept
        {
            if ( i >= text.size() || text[i] != '"' )
                return std::nullopt;
            const size_t close = text.find( '"', i + 1 );
            if ( close == std::string::npos )
                return std::nullopt;
            std::string value = text.substr( i + 1, close - i - 1 );
            i                 = close + 1;
            return value;
        }

        int LineOf( const std::string& text, size_t offset ) noexcept
        {
            return 1 + static_cast<int>( std::count(
                            text.begin(), text.begin() + static_cast<std::ptrdiff_t>( offset ), '\n' ) );
        }

        // A C++ identifier from a shader/pass/stage name: alphanumerics kept, a letter after a dropped character
        // upper-cased ("Raster binning" -> "RasterBinning").
        std::string Identifier( std::string_view name ) noexcept
        {
            std::string out;
            bool        upper = false;
            for ( const char c : name )
            {
                if ( std::isalnum( static_cast<unsigned char>( c ) ) == 0 )
                {
                    upper = true;
                    continue;
                }
                out += upper ? static_cast<char>( std::toupper( static_cast<unsigned char>( c ) ) ) : c;
                upper = false;
            }
            return out;
        }

        // One declaration of the block body, whitespace collapsed: "type name" or "type name[N]".
        struct Declaration
        {
            std::string   Type;
            std::string   Name;
            std::uint32_t Count = 0;
        };

        std::optional<Declaration> ParseDeclaration( std::string_view decl ) noexcept
        {
            std::string compact; // spaces only between two identifier characters
            for ( size_t i = 0; i < decl.size(); ++i )
            {
                const char c = decl[i];
                if ( std::isspace( static_cast<unsigned char>( c ) ) == 0 )
                {
                    compact += c;
                    continue;
                }
                if ( !compact.empty() && IdentChar( compact.back() ) && i + 1 < decl.size() )
                {
                    size_t next = i + 1;
                    while ( next < decl.size() && std::isspace( static_cast<unsigned char>( decl[next] ) ) != 0 )
                        ++next;
                    if ( next < decl.size() && IdentChar( decl[next] ) && compact.back() != ' ' )
                        compact += ' ';
                }
            }
            const size_t space = compact.find( ' ' );
            if ( space == std::string::npos )
                return std::nullopt;
            Declaration result;
            result.Type           = compact.substr( 0, space );
            std::string_view rest = std::string_view( compact ).substr( space + 1 );
            const size_t     open = rest.find( '[' );
            result.Name           = std::string( rest.substr( 0, open ) );
            if ( open != std::string_view::npos )
            {
                if ( rest.back() != ']' )
                    return std::nullopt;
                const std::string_view digits = rest.substr( open + 1, rest.size() - open - 2 );
                const auto [end, ec] =
                     std::from_chars( digits.data(), digits.data() + digits.size(), result.Count );
                if ( ec != std::errc() || end != digits.data() + digits.size() || result.Count == 0u )
                    return std::nullopt;
            }
            if ( result.Name.empty() || !IdentStart( result.Name.front() ) ||
                 !std::all_of( result.Name.begin(), result.Name.end(), IdentChar ) )
                return std::nullopt;
            return result;
        }

        struct FoundBlock
        {
            std::string                  Path;
            std::string                  Shader;
            std::string                  Pass;
            std::string                  Stage;
            std::string                  Block;
            std::vector<ShaderPushField> Fields; // with std430 offsets and explicit padding
            std::uint32_t                Size = 0;
            std::vector<std::string>     NameParts;
        };

        // The std430 layout of `decls`: members at their offsets, alignment gaps as Std430PadN uint32 arrays.
        std::optional<std::string> Lay( const std::vector<Declaration>& decls, FoundBlock& block ) noexcept
        {
            std::uint32_t offset = 0;
            std::uint32_t pads   = 0;
            for ( const Declaration& decl : decls )
            {
                const PushType* type = FindType( decl.Type );
                if ( type == nullptr )
                    return std::format( "push member '{} {}': type has no C++ twin of the same std430 bytes",
                                        decl.Type, decl.Name );
                const std::uint32_t aligned = ( offset + type->Align - 1u ) / type->Align * type->Align;
                if ( aligned > offset )
                    block.Fields.push_back( { "std::uint32_t", std::format( "Std430Pad{}", pads++ ),
                                              ( aligned - offset ) / 4u, offset } );
                if ( decl.Count != 0u )
                {
                    const std::uint32_t stride = ( type->Size + type->Align - 1u ) / type->Align * type->Align;
                    if ( stride != type->Size )
                        return std::format(
                             "push member '{} {}[{}]': std430 array stride {} is not the C++ size {}", decl.Type,
                             decl.Name, decl.Count, stride, type->Size );
                    block.Fields.push_back( { std::string( type->Cpp ), decl.Name, decl.Count, aligned } );
                    offset = aligned + stride * decl.Count;
                }
                else
                {
                    block.Fields.push_back( { std::string( type->Cpp ), decl.Name, 0u, aligned } );
                    offset = aligned + type->Size;
                }
            }
            block.Size = offset;
            return std::nullopt;
        }

        bool SameLayout( const FoundBlock& a, const FoundBlock& b ) noexcept
        {
            if ( a.Size != b.Size || a.Fields.size() != b.Fields.size() )
                return false;
            for ( size_t i = 0; i < a.Fields.size(); ++i )
            {
                const ShaderPushField& x = a.Fields[i];
                const ShaderPushField& y = b.Fields[i];
                if ( x.CppType != y.CppType || x.Name != y.Name || x.Count != y.Count || x.Offset != y.Offset )
                    return false;
            }
            return true;
        }

        std::string JoinedName( const FoundBlock& block ) noexcept
        {
            std::string name;
            for ( const std::string& part : block.NameParts )
                name += ( name.empty() ? "" : "_" ) + part;
            return name;
        }

        // Names held by more than one distinct layout.
        std::vector<std::string> Collisions( const std::vector<FoundBlock>& blocks ) noexcept
        {
            std::map<std::string, std::vector<const FoundBlock*>> byName;
            for ( const FoundBlock& block : blocks )
                byName[JoinedName( block )].push_back( &block );
            std::vector<std::string> names;
            for ( const auto& [name, members] : byName )
                for ( const FoundBlock* other : members )
                    if ( !SameLayout( *members.front(), *other ) )
                    {
                        names.push_back( name );
                        break;
                    }
            return names;
        }

        void ScanFile( const ShaderSource& source, std::vector<FoundBlock>& blocks,
                       std::vector<std::string>& errors ) noexcept
        {
            const std::string text = StripComments( source.Text );
            enum class Label
            {
                Other,
                Pass,
                Stage
            };
            struct Frame
            {
                Label       Kind = Label::Other;
                std::string Name;
            };
            std::vector<Frame> stack;
            std::string        shader;
            std::string        lastWord;
            size_t             lastWordEnd = std::string::npos;

            for ( size_t i = 0; i < text.size(); )
            {
                const char c = text[i];
                if ( c == '"' )
                {
                    if ( !ReadString( text, i ) )
                        i = text.size();
                    lastWord.clear();
                    continue;
                }
                if ( c == '{' )
                {
                    const bool labelled = !lastWord.empty() && SkipSpace( text, lastWordEnd ) == i;
                    stack.push_back( labelled && IsStage( lastWord ) ? Frame{ Label::Stage, lastWord } : Frame{} );
                    lastWord.clear();
                    ++i;
                    continue;
                }
                if ( c == '}' )
                {
                    if ( !stack.empty() )
                        stack.pop_back();
                    lastWord.clear();
                    ++i;
                    continue;
                }
                if ( !IdentStart( c ) || ( i > 0 && IdentChar( text[i - 1] ) ) )
                {
                    if ( std::isspace( static_cast<unsigned char>( c ) ) == 0 )
                        lastWord.clear();
                    ++i;
                    continue;
                }
                const size_t      wordStart = i;
                const std::string word      = ReadWord( text, i );
                if ( word == "Shader" && shader.empty() )
                {
                    size_t at = SkipSpace( text, i );
                    if ( auto name = ReadString( text, at ) )
                    {
                        shader = *name;
                        i      = at;
                    }
                    lastWord.clear();
                    continue;
                }
                if ( word == "Pass" )
                {
                    size_t at = SkipSpace( text, i );
                    if ( auto name = ReadString( text, at ) )
                    {
                        at = SkipSpace( text, at );
                        if ( at < text.size() && text[at] == '{' )
                        {
                            stack.push_back( Frame{ Label::Pass, *name } );
                            i = at + 1;
                            lastWord.clear();
                            continue;
                        }
                    }
                }
                if ( word != "PushConstant" )
                {
                    lastWord    = word;
                    lastWordEnd = i;
                    continue;
                }

                // PushConstant <Block> { <members> } [instance];
                const std::string where = std::format( "{}:{}", source.RelativePath, LineOf( text, wordStart ) );
                size_t            at    = SkipSpace( text, i );
                const std::string block = ReadWord( text, at );
                at                      = SkipSpace( text, at );
                const size_t close =
                     at < text.size() && text[at] == '{' ? text.find( '}', at ) : std::string::npos;
                if ( block.empty() || close == std::string::npos )
                {
                    errors.push_back( where + ": PushConstant without a `<Name> { ... }` block" );
                    i = text.size();
                    continue;
                }
                i = close + 1;
                lastWord.clear();
                const std::string body = text.substr( at + 1, close - at - 1 );
                if ( shader.empty() )
                {
                    errors.push_back( where + ": PushConstant before the file's `Shader \"Name\"`" );
                    continue;
                }
                if ( body.find( '#' ) != std::string::npos )
                {
                    errors.push_back( where + ": a preprocessor directive inside the push block - one layout per "
                                              "block, a variant declares its own block" );
                    continue;
                }
                std::vector<Declaration> decls;
                bool                     parsed = true;
                for ( size_t from = 0; from < body.size(); )
                {
                    size_t semi = body.find( ';', from );
                    if ( semi == std::string::npos )
                        semi = body.size();
                    const std::string_view piece( body.data() + from, semi - from );
                    from = semi + 1;
                    if ( std::all_of( piece.begin(), piece.end(), []( char ch )
                                      { return std::isspace( static_cast<unsigned char>( ch ) ) != 0; } ) )
                        continue;
                    auto decl = ParseDeclaration( piece );
                    if ( !decl )
                    {
                        errors.push_back( std::format( "{}: push member '{}' is not `<type> <name>` or "
                                                       "`<type> <name>[N]`",
                                                       where, std::string( piece ) ) );
                        parsed = false;
                        break;
                    }
                    decls.push_back( std::move( *decl ) );
                }
                if ( !parsed )
                    continue;

                FoundBlock found;
                found.Path   = source.RelativePath;
                found.Shader = shader;
                found.Block  = block;
                for ( auto frame = stack.rbegin(); frame != stack.rend(); ++frame )
                {
                    if ( frame->Kind == Label::Stage && found.Stage.empty() )
                        found.Stage = frame->Name;
                    if ( frame->Kind == Label::Pass && found.Pass.empty() )
                        found.Pass = frame->Name;
                }
                if ( auto failed = Lay( decls, found ) )
                {
                    errors.push_back( where + ": " + *failed );
                    continue;
                }
                found.NameParts = { Identifier( shader ) };
                blocks.push_back( std::move( found ) );
            }
        }
    } // namespace

    ShaderPushScan ScanShaderPushBlocks( const std::vector<ShaderSource>& sources ) noexcept
    {
        ShaderPushScan          scan;
        std::vector<FoundBlock> blocks;
        for ( const ShaderSource& source : sources )
            ScanFile( source, blocks, scan.Errors );

        // Naming: the shader's name; a name two different layouts share takes the pass, then the stage, then the
        // block name. Blocks of one name and one layout (a program's stages, or passes sharing a block) are one
        // struct.
        for ( std::string FoundBlock::*extra : { &FoundBlock::Pass, &FoundBlock::Stage, &FoundBlock::Block } )
        {
            const std::vector<std::string> clashing = Collisions( blocks );
            for ( FoundBlock& block : blocks )
                if ( !( block.*extra ).empty() &&
                     std::find( clashing.begin(), clashing.end(), JoinedName( block ) ) != clashing.end() )
                    block.NameParts.push_back( Identifier( block.*extra ) );
        }
        for ( const std::string& name : Collisions( blocks ) )
            scan.Errors.push_back(
                 std::format( "push struct '{}': two different push layouts with the same shader, "
                              "pass, stage and block names",
                              name ) );

        std::map<std::string, const FoundBlock*> unique;
        for ( const FoundBlock& block : blocks )
            unique.try_emplace( JoinedName( block ), &block );
        for ( const auto& [name, block] : unique )
        {
            if ( name.empty() || !IdentStart( name.front() ) )
            {
                scan.Errors.push_back(
                     std::format( "{}: shader name '{}' gives no C++ identifier", block->Path, block->Shader ) );
                continue;
            }
            std::string where = block->Path;
            if ( !block->Pass.empty() || !block->Stage.empty() )
                where += std::format( " ({}{}{})", block->Pass,
                                      block->Pass.empty() || block->Stage.empty() ? "" : " ", block->Stage );
            scan.Structs.push_back( { name, where, block->Size, block->Fields } );
        }
        return scan;
    }
} // namespace Desert::HeaderTool
