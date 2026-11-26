#include "GlslEmit.hpp"

#include <cctype>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <unordered_map>

namespace Desert::ShaderProtocolGen
{
    using Graphic::ShaderProtocols::ShaderBindingKind;
    using Graphic::ShaderProtocols::ShaderBlockDecl;
    using Graphic::ShaderProtocols::ShaderBlockKind;
    using Graphic::ShaderProtocols::ShaderConstantDecl;
    using Graphic::ShaderProtocols::ShaderField;

    uint64_t Fnv1a64( const std::string_view text ) noexcept
    {
        uint64_t hash = 14695981039346656037ull;
        for ( const char c : text )
        {
            hash ^= static_cast<uint8_t>( c );
            hash *= 1099511628211ull;
        }
        return hash;
    }

    namespace
    {
        std::string Refusal( const ShaderIncludeDecl& include, const std::string_view reason )
        {
            return std::format( "{}: {}: {}", include.SourceHeader, include.Path, reason );
        }

        // DESERT_GENERATED_<path upper-cased, every other character '_'>.
        std::string GuardOf( const std::string_view path )
        {
            std::string guard = "DESERT_GENERATED_";
            for ( const char c : path )
                guard += std::isalnum( static_cast<unsigned char>( c ) ) != 0
                              ? static_cast<char>( std::toupper( static_cast<unsigned char>( c ) ) )
                              : '_';
            return guard;
        }

        std::string FieldDeclaration( const ShaderField& field )
        {
            return field.Count != 0 ? std::format( "{} {}[{}];", field.Glsl, field.Name, field.Count )
                                    : std::format( "{} {};", field.Glsl, field.Name );
        }

        std::string BlockMembers( const ShaderBlockDecl& block )
        {
            std::string members;
            if ( block.Members != nullptr )
                for ( const ShaderField& field : block.Members->Fields )
                    members += std::format( "    {}\n", FieldDeclaration( field ) );
            if ( block.RuntimeArrayOf != nullptr )
                members += std::format( "    {} {}[];\n", block.RuntimeArrayOf->GlslName, block.RuntimeArrayName );
            return members;
        }

        std::string_view SugarOf( const ShaderBlockKind kind )
        {
            switch ( kind )
            {
                case ShaderBlockKind::Uniform:
                    return "Uniform";
                case ShaderBlockKind::ReadBuffer:
                    return "ReadBuffer";
                case ShaderBlockKind::WriteBuffer:
                    return "WriteBuffer";
                case ShaderBlockKind::Buffer:
                    return "Buffer";
            }
            return "Uniform";
        }

        // The raw layout of an Overridable block (the DSL sugar takes a number, not a macro): packing, storage.
        std::string_view PackingOf( const ShaderBlockKind kind )
        {
            return kind == ShaderBlockKind::Uniform ? "std140" : "std430";
        }
        std::string_view StorageOf( const ShaderBlockKind kind )
        {
            switch ( kind )
            {
                case ShaderBlockKind::Uniform:
                    return "uniform";
                case ShaderBlockKind::ReadBuffer:
                    return "readonly buffer";
                case ShaderBlockKind::WriteBuffer:
                    return "writeonly buffer";
                case ShaderBlockKind::Buffer:
                    return "buffer";
            }
            return "uniform";
        }

        std::string EmitBlock( const ShaderBlockDecl& block )
        {
            std::string head;
            if ( block.Binding.Kind == ShaderBindingKind::Fixed )
                head = std::format( "{}({}) {}", SugarOf( block.Kind ), block.Binding.Value, block.BlockName );
            else
                head = std::format( "#ifndef {0}\n#define {0} {1}\n#endif\nlayout( {2}, binding = {0} ) {3} {4}",
                                    block.Binding.Macro, block.Binding.Value, PackingOf( block.Kind ),
                                    StorageOf( block.Kind ), block.BlockName );
            const std::string tail = block.InstanceName.empty() ? std::string( "};\n" )
                                                                : std::format( "}} {};\n", block.InstanceName );
            return std::format( "{} {{\n{}{}", head, BlockMembers( block ), tail );
        }

        std::string EmitConstant( const ShaderConstantDecl& constant )
        {
            switch ( constant.Kind )
            {
                case ShaderConstantDecl::Type::Int:
                    return std::format( "const int {} = {};\n", constant.Name,
                                        static_cast<long long>( constant.Value ) );
                case ShaderConstantDecl::Type::Uint:
                    return std::format( "const uint {} = {}u;\n", constant.Name,
                                        static_cast<unsigned long long>( constant.Value ) );
                case ShaderConstantDecl::Type::Float:
                {
                    std::string value = std::format( "{}", constant.Value );
                    if ( value.find_first_of( ".eEn" ) == std::string::npos )
                        value += ".0";
                    return std::format( "const float {} = {};\n", constant.Name, value );
                }
            }
            return {};
        }

        // Every include path @p include reaches through Requires, itself first; unknown paths are skipped (refused
        // separately).
        void Closure( const ShaderIncludeDecl&                                              include,
                      const std::unordered_map<std::string_view, const ShaderIncludeDecl*>& byPath,
                      std::vector<const ShaderIncludeDecl*>&                                out )
        {
            for ( const ShaderIncludeDecl* seen : out )
                if ( seen == &include )
                    return;
            out.push_back( &include );
            for ( const std::string_view required : include.Requires )
                if ( const auto it = byPath.find( required ); it != byPath.end() )
                    Closure( *it->second, byPath, out );
        }

        void CheckNestedDeclared( const ShaderIncludeDecl& include, const ShaderStructDecl& layout,
                                  const std::set<std::string_view>& declared, const std::string_view user,
                                  std::vector<std::string>& errors )
        {
            for ( const ShaderField& field : layout.Fields )
                if ( field.Nested != nullptr && !declared.contains( field.Nested->GlslName ) )
                    errors.push_back( Refusal(
                         include, std::format( "{} uses struct {} before any include of its closure declares it",
                                               user, field.Nested->GlslName ) ) );
        }
    } // namespace

    std::vector<std::string> CheckIncludes( const std::span<const ShaderIncludeDecl* const> includes,
                                            const std::span<const ShaderIncludeDecl* const> sceneIncludes,
                                            const std::filesystem::path&                    shaderRoot )
    {
        std::vector<std::string>                                       errors;
        std::unordered_map<std::string_view, const ShaderIncludeDecl*> byPath;
        std::map<std::string_view, const ShaderIncludeDecl*>           byName;
        for ( const ShaderIncludeDecl* include : includes )
        {
            if ( std::filesystem::exists( shaderRoot / include->Path ) )
                errors.push_back(
                     Refusal( *include, std::format( "collides with the hand-written file {}",
                                                     ( shaderRoot / include->Path ).generic_string() ) ) );
            if ( !byPath.emplace( include->Path, include ).second )
                errors.push_back( Refusal( *include, std::format( "a second descriptor of this path (the first is "
                                                                  "{})",
                                                                  byPath[include->Path]->SourceHeader ) ) );
            const auto name = [&]( const std::string_view glsl, const std::string_view what )
            {
                if ( const auto [it, added] = byName.emplace( glsl, include ); !added )
                    errors.push_back(
                         Refusal( *include, std::format( "{} {} is also emitted by {} ({})", what, glsl,
                                                         it->second->Path, it->second->SourceHeader ) ) );
            };
            for ( const ShaderStructDecl* layout : include->Structs )
                name( layout->GlslName, "struct" );
            for ( const ShaderBlockDecl& block : include->Blocks )
                name( block.BlockName, "block" );
            for ( const ShaderBlockDecl& block : include->Blocks )
                if ( block.Binding.Kind == Graphic::ShaderProtocols::ShaderBindingKind::Overridable &&
                     block.Binding.Macro.empty() )
                    errors.push_back( Refusal(
                         *include, std::format( "block {} is Overridable without a macro", block.BlockName ) ) );
        }

        for ( const ShaderIncludeDecl* include : includes )
        {
            for ( const std::string_view required : include->Requires )
                if ( !byPath.contains( required ) )
                    errors.push_back( Refusal(
                         *include, std::format( "requires {}, which no descriptor generates", required ) ) );

            std::vector<const ShaderIncludeDecl*> closure;
            Closure( *include, byPath, closure );

            // Structs the closure declares before this include's own (its Requires are #included first).
            std::set<std::string_view> declared;
            for ( size_t i = 1; i < closure.size(); ++i )
                for ( const ShaderStructDecl* layout : closure[i]->Structs )
                    declared.insert( layout->GlslName );
            for ( const ShaderStructDecl* layout : include->Structs )
            {
                CheckNestedDeclared( *include, *layout, declared, std::format( "struct {}", layout->GlslName ),
                                     errors );
                declared.insert( layout->GlslName );
            }
            for ( const ShaderBlockDecl& block : include->Blocks )
            {
                if ( block.Members == nullptr && block.RuntimeArrayOf == nullptr )
                    errors.push_back(
                         Refusal( *include, std::format( "block {} has no member", block.BlockName ) ) );
                if ( block.Members != nullptr )
                    CheckNestedDeclared( *include, *block.Members, declared,
                                         std::format( "block {}", block.BlockName ), errors );
                if ( block.RuntimeArrayOf != nullptr && !declared.contains( block.RuntimeArrayOf->GlslName ) )
                    errors.push_back( Refusal(
                         *include, std::format( "block {} holds struct {} before any include of its closure "
                                                "declares it",
                                                block.BlockName, block.RuntimeArrayOf->GlslName ) ) );
                if ( block.RuntimeArrayOf != nullptr && block.RuntimeArrayName.empty() )
                    errors.push_back( Refusal(
                         *include, std::format( "block {} names no runtime array member", block.BlockName ) ) );
            }

            // One Fixed number per slot within what one program sees through this include.
            std::map<uint32_t, std::string_view> fixed;
            for ( const ShaderIncludeDecl* reached : closure )
                for ( const ShaderBlockDecl& block : reached->Blocks )
                    if ( block.Binding.Kind == Graphic::ShaderProtocols::ShaderBindingKind::Fixed )
                        if ( const auto [it, added] = fixed.emplace( block.Binding.Value, block.BlockName );
                             !added && it->second != block.BlockName )
                            errors.push_back( Refusal(
                                 *include, std::format( "blocks {} and {} are both generated at binding {}",
                                                        it->second, block.BlockName, block.Binding.Value ) ) );
        }

        // The scene protocols are included side by side by every lit program: one number each across the part.
        std::map<uint32_t, const ShaderBlockDecl*> scene;
        for ( const ShaderIncludeDecl* include : sceneIncludes )
            for ( const ShaderBlockDecl& block : include->Blocks )
                if ( block.Binding.Kind == Graphic::ShaderProtocols::ShaderBindingKind::Fixed )
                    if ( const auto [it, added] = scene.emplace( block.Binding.Value, &block ); !added )
                        errors.push_back(
                             Refusal( *include, std::format( "scene blocks {} and {} are both generated at "
                                                             "binding {}",
                                                             it->second->BlockName, block.BlockName,
                                                             block.Binding.Value ) ) );
        return errors;
    }

    std::string EmitGlsl( const ShaderIncludeDecl& include )
    {
        const std::string guard = GuardOf( include.Path );
        std::string       glsl  = std::format( "#ifndef {0}\n#define {0}\n\n"
                                                      "// GENERATED from {1} by ShaderProtocolGen: the C++ declaration is "
                                                      "the one layout; edit it, never this text.\n",
                                               guard, include.SourceHeader );
        for ( const std::string_view required : include.Requires )
            glsl += std::format( "#include <{}>\n", required );

        for ( const ShaderStructDecl* layout : include.Structs )
        {
            std::string members;
            std::string macro;
            for ( const ShaderField& field : layout->Fields )
            {
                members += std::format( "    {}\n", FieldDeclaration( field ) );
                macro += std::format( " {}", FieldDeclaration( field ) );
            }
            // The macro is one line: a block whose binding the program owns writes `{ DESERT_<Glsl>_MEMBERS }`.
            glsl += std::format( "\nstruct {0} {{\n{1}}};\n#define DESERT_{0}_MEMBERS{2}\n", layout->GlslName,
                                 members, macro );
        }

        if ( !include.Constants.empty() )
        {
            glsl += "\n";
            for ( const ShaderConstantDecl& constant : include.Constants )
                glsl += EmitConstant( constant );
        }

        for ( const ShaderBlockDecl& block : include.Blocks )
            glsl += "\n" + EmitBlock( block );

        if ( include.Text != nullptr )
            glsl += "\n" + include.Text();

        glsl += "\n#endif\n";
        return glsl;
    }

    std::vector<GeneratedShape> CollectShapes( const std::span<const ShaderIncludeDecl* const> includes )
    {
        std::vector<GeneratedShape> shapes;
        for ( const ShaderIncludeDecl* include : includes )
        {
            for ( const ShaderStructDecl* layout : include->Structs )
            {
                GeneratedShape shape{ std::string( layout->GlslName ), {}, include->Path, include->SourceHeader };
                for ( const ShaderField& field : layout->Fields )
                    shape.Members.emplace_back( field.Name );
                shapes.push_back( std::move( shape ) );
            }
            for ( const ShaderBlockDecl& block : include->Blocks )
            {
                GeneratedShape shape{ std::string( block.BlockName ), {}, include->Path, include->SourceHeader };
                if ( block.Members != nullptr )
                    for ( const ShaderField& field : block.Members->Fields )
                        shape.Members.emplace_back( field.Name );
                if ( block.RuntimeArrayOf != nullptr )
                    shape.Members.emplace_back( block.RuntimeArrayName );
                shapes.push_back( std::move( shape ) );
            }
        }
        return shapes;
    }
} // namespace Desert::ShaderProtocolGen
