#include <Engine/VFX/VFXStackCompiler.hpp>

#include <Engine/VFX/VFXCurveLUT.hpp>

#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>

#include <Common/Core/Constants.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

namespace Desert::VFX
{
    namespace S = Assets::Serialization;

    namespace
    {
        // Bumped when the generated text changes shape, so no cache keyed by an older generator is reused.
        constexpr std::string_view kGeneratorTag = "VFXStackCompiler 1";

        uint64_t Fnv1a64( std::string_view bytes )
        {
            uint64_t hash = 0xcbf29ce484222325ull;
            for ( const char c : bytes )
            {
                hash ^= static_cast<unsigned char>( c );
                hash *= 0x100000001b3ull;
            }
            return hash;
        }

        bool IsIdentStart( char c )
        {
            return std::isalpha( static_cast<unsigned char>( c ) ) != 0 || c == '_';
        }

        bool IsIdentChar( char c )
        {
            return std::isalnum( static_cast<unsigned char>( c ) ) != 0 || c == '_';
        }

        bool IsIdentifier( std::string_view s )
        {
            return !s.empty() && IsIdentStart( s.front() ) && std::all_of( s.begin(), s.end(), IsIdentChar );
        }

        bool ParseType( std::string_view word, VFXValueType& out )
        {
            static constexpr std::pair<std::string_view, VFXValueType> kTypes[] = {
                 { "float", VFXValueType::Float }, { "vec2", VFXValueType::Vec2 },
                 { "vec3", VFXValueType::Vec3 },   { "vec4", VFXValueType::Vec4 },
                 { "int", VFXValueType::Int },     { "bool", VFXValueType::Bool } };
            for ( const auto& [name, type] : kTypes )
                if ( name == word )
                {
                    out = type;
                    return true;
                }
            return false;
        }

        std::string_view GroupName( VFXStackGroup group )
        {
            return group == VFXStackGroup::ParticleSpawn ? "ParticleSpawn" : "ParticleUpdate";
        }

        const std::vector<S::VFXModuleUse>& GroupOf( const S::VFXEmitterData& emitter, VFXStackGroup group )
        {
            return group == VFXStackGroup::ParticleSpawn ? emitter.Stack.ParticleSpawn
                                                         : emitter.Stack.ParticleUpdate;
        }

        /// Every whole-word Module / ModuleInputs in @p body renamed to the module's own function and input
        /// struct.
        std::string RenameModuleSymbols( const std::string& body, std::string_view function,
                                         std::string_view inputs )
        {
            std::string out;
            out.reserve( body.size() + 64 );
            std::size_t i = 0;
            while ( i < body.size() )
            {
                if ( !IsIdentStart( body[i] ) )
                {
                    out += body[i++];
                    continue;
                }
                const std::size_t start = i;
                while ( i < body.size() && IsIdentChar( body[i] ) )
                    ++i;
                const std::string_view word( body.data() + start, i - start );
                if ( word == "Module" )
                    out += function;
                else if ( word == "ModuleInputs" )
                    out += inputs;
                else
                    out += word;
            }
            return out;
        }

        /// The swizzle that narrows a stored vec4 to a float type.
        std::string_view Swizzle( VFXValueType type )
        {
            switch ( type )
            {
                case VFXValueType::Vec2:
                    return ".xy";
                case VFXValueType::Vec3:
                    return ".xyz";
                case VFXValueType::Vec4:
                    return "";
                default:
                    return ".x";
            }
        }

        /// The GLSL expression reading parameter row @p slot as @p type.
        std::string ParamExpr( uint32_t slot, VFXValueType type )
        {
            if ( type == VFXValueType::Int )
                return std::format( "int( VFX_Param( {}u ).x )", slot );
            if ( type == VFXValueType::Bool )
                return std::format( "( VFX_Param( {}u ).x != 0.0 )", slot );
            return std::format( "VFX_Param( {}u ){}", slot, Swizzle( type ) );
        }

        /// A uniform value between rows @p slot (Min) and @p slot + 1 (Max), keyed by @p slot. An int range is
        /// inclusive at both ends (UE's random int).
        std::string RandomExpr( uint32_t slot, VFXValueType type )
        {
            if ( type == VFXValueType::Int )
                return std::format( "int( min( floor( mix( VFX_Param( {0}u ).x, VFX_Param( {1}u ).x + 1.0, "
                                    "VFX_Random( sim, {0}u ).x ) ), VFX_Param( {1}u ).x ) )",
                                    slot, slot + 1 );
            const std::string_view sw = Swizzle( type );
            return std::format( "mix( VFX_Param( {0}u ){2}, VFX_Param( {1}u ){2}, VFX_Random( sim, {0}u ){2} )",
                                slot, slot + 1, sw );
        }

        /// The curve input whose LUT row is @p slot, sampled at the particle's normalised age (VFX-05): the keys
        /// are in the system's curve atlas and the row only says where, so a key edit never reaches the text.
        std::string CurveExpr( uint32_t slot, VFXValueType type )
        {
            return std::format( "VFX_CurveSample( {}u, VFX_NormalizedAge( p.Age, p.Lifetime ), {}u ){}", slot,
                                S::ComponentCount( type ), Swizzle( type ) );
        }

        /// A zero of @p type in GLSL.
        std::string ZeroOf( VFXValueType type )
        {
            switch ( type )
            {
                case VFXValueType::Int:
                    return "0";
                case VFXValueType::Bool:
                    return "false";
                case VFXValueType::Float:
                    return "0.0";
                default:
                    return std::format( "{}( 0.0 )", GlslTypeName( type ) );
            }
        }

        Common::ResultStr<std::string> ReadText( const std::filesystem::path& path )
        {
            std::ifstream in( path, std::ios::binary );
            if ( !in )
                return Common::MakeFormattedError<std::string>( "cannot read '{}'", path.string() );
            std::ostringstream out;
            out << in.rdbuf();
            return Common::MakeSuccess( out.str() );
        }

        /// One distinct module the stack uses, resolved and parsed.
        struct ResolvedModule
        {
            std::string       Function; ///< VFXMod_E_<Name> / VFXMod_L_<Id>
            std::string       Inputs;   ///< VFXIn_E_<Name> / VFXIn_L_<Id>
            VFXParticleModule Module;
        };

        Common::ResultStr<ResolvedModule> ResolveModule( const S::VFXSystemData& system, const std::string& ref,
                                                         const std::filesystem::path& engineModuleDir,
                                                         const std::string&           where )
        {
            std::string text;
            std::string suffix;
            if ( ref.starts_with( S::kVFXEnginePrefix ) )
            {
                const std::string name = ref.substr( S::kVFXEnginePrefix.size() );
                if ( !IsIdentifier( name ) )
                    return Common::MakeFormattedError<ResolvedModule>(
                         "{}: engine module name '{}' is not an identifier", where, name );
                const std::filesystem::path path = engineModuleDir / std::format( "{}.shader", name );
                auto                        read = ReadText( path );
                if ( !read.IsSuccess() )
                    return Common::MakeFormattedError<ResolvedModule>( "{}: engine module '{}' has no file — {}",
                                                                       where, name, read.GetError() );
                text   = read.ExtractValue();
                suffix = std::format( "E_{}", name );
            }
            else if ( ref.starts_with( S::kVFXLocalPrefix ) )
            {
                const std::string id = ref.substr( S::kVFXLocalPrefix.size() );
                if ( !IsIdentifier( id ) )
                    return Common::MakeFormattedError<ResolvedModule>(
                         "{}: local module id '{}' is not an identifier", where, id );
                const auto it = std::find_if( system.LocalModules.begin(), system.LocalModules.end(),
                                              [&]( const S::VFXLocalModule& m ) { return m.Id == id; } );
                if ( it == system.LocalModules.end() )
                    return Common::MakeFormattedError<ResolvedModule>( "{}: no LocalModules row '{}'", where, id );
                text   = it->Source;
                suffix = std::format( "L_{}", id );
            }
            else
                return Common::MakeFormattedError<ResolvedModule>(
                     "{}: '{}' is neither engine:<Name> nor local:<Id>", where, ref );

            auto parsed = ParseParticleModule( text, where );
            if ( !parsed.IsSuccess() )
                return Common::MakeError<ResolvedModule>( parsed.GetError() );
            ResolvedModule resolved;
            resolved.Function = std::format( "VFXMod_{}", suffix );
            resolved.Inputs   = std::format( "VFXIn_{}", suffix );
            resolved.Module   = parsed.ExtractValue();
            return Common::MakeSuccess( std::move( resolved ) );
        }
    } // namespace

    std::string_view GlslTypeName( VFXValueType type )
    {
        switch ( type )
        {
            case VFXValueType::Float:
                return "float";
            case VFXValueType::Vec2:
                return "vec2";
            case VFXValueType::Vec3:
                return "vec3";
            case VFXValueType::Vec4:
                return "vec4";
            case VFXValueType::Int:
                return "int";
            case VFXValueType::Bool:
                return "bool";
        }
        return "float";
    }

    VFXComponentClass ComponentClassOf( VFXValueType type )
    {
        return type == VFXValueType::Int || type == VFXValueType::Bool ? VFXComponentClass::Int
                                                                       : VFXComponentClass::Float;
    }

    Common::ResultStr<VFXParticleModule> ParseParticleModule( const std::string& shaderText,
                                                              std::string_view   where )
    {
        const auto parsed = Core::Preprocess::DShaderParser::Parse( shaderText );
        if ( !parsed.IsSuccess() )
            return Common::MakeFormattedError<VFXParticleModule>( "{}: {}", where, parsed.GetError() );
        const auto& meta = parsed.GetValue().Meta;
        if ( meta.Domain != Core::Formats::ShaderDomain::Particle || meta.ParticleSource.empty() )
            return Common::MakeFormattedError<VFXParticleModule>(
                 "{}: a stack module is a `Domain Particle` shader "
                 "with a Particle block",
                 where );

        VFXParticleModule     module;
        std::set<std::string> attributeNames;
        std::set<std::string> inputNames;
        const std::string&    source = meta.ParticleSource;
        std::size_t           pos    = 0;
        while ( pos < source.size() )
        {
            const std::size_t  end  = std::min( source.find( '\n', pos ), source.size() );
            const std::string  line = source.substr( pos, end - pos );
            std::istringstream words( line );
            std::string        keyword;
            words >> keyword;
            if ( keyword.empty() || keyword.starts_with( "//" ) )
            {
                pos = end + 1;
                continue;
            }
            if ( keyword != "Attribute" && keyword != "Input" )
                break; // the GLSL starts here

            std::string name;
            std::string typeWord;
            std::string extra;
            words >> name >> typeWord >> extra;
            VFXValueType type = VFXValueType::Float;
            if ( !IsIdentifier( name ) || !ParseType( typeWord, type ) || !extra.empty() )
                return Common::MakeFormattedError<VFXParticleModule>(
                     "{}: '{}' is not `{} <Name> <float|vec2|vec3|vec4|int|bool>`", where, line, keyword );
            auto& names = keyword == "Attribute" ? attributeNames : inputNames;
            if ( !names.insert( name ).second )
                return Common::MakeFormattedError<VFXParticleModule>( "{}: {} '{}' is declared twice", where,
                                                                      keyword, name );
            ( keyword == "Attribute" ? module.Attributes : module.Inputs ).push_back( { name, type } );
            pos = end + 1;
        }
        module.Body = pos < source.size() ? source.substr( pos ) : std::string{};

        // The function the stack calls must be there; anything else in the body is the shader compiler's to judge.
        bool        definesModule = false;
        std::size_t at            = 0;
        while ( ( at = module.Body.find( "Module", at ) ) != std::string::npos )
        {
            const bool startOk = at == 0 || !IsIdentChar( module.Body[at - 1] );
            const bool endOk   = at + 6 >= module.Body.size() || !IsIdentChar( module.Body[at + 6] );
            definesModule |= startOk && endOk;
            at += 6;
        }
        if ( !definesModule )
            return Common::MakeFormattedError<VFXParticleModule>(
                 "{}: the Particle block defines no `void Module( inout ParticleCtx p, inout VFXSim sim, in "
                 "ModuleInputs i )`",
                 where );
        return Common::MakeSuccess( std::move( module ) );
    }

    const VFXAttributeLayout* VFXDataSetLayout::Find( std::string_view name ) const
    {
        const auto it = std::find_if( Attributes.begin(), Attributes.end(),
                                      [&]( const VFXAttributeLayout& a ) { return a.Name == name; } );
        return it == Attributes.end() ? nullptr : &*it;
    }

    // Port of FNiagaraDataSetCompiledData::BuildLayout (NiagaraDataSet.cpp:1863-1884): per variable the type's
    // component counts by class (NiagaraTypes.cpp:850-903 GenerateLayoutInfoInternal — a float vector is N
    // floats, an int32/bool one int32), the start of each class = the running total before it.
    VFXDataSetLayout BuildLayout( const std::vector<VFXModuleDecl>& variables )
    {
        VFXDataSetLayout layout;
        for ( const VFXModuleDecl& var : variables )
        {
            VFXAttributeLayout info;
            info.Name        = var.Name;
            info.Type        = var.Type;
            const uint32_t n = S::ComponentCount( var.Type );
            info.FloatCount  = ComponentClassOf( var.Type ) == VFXComponentClass::Float ? n : 0;
            info.IntCount    = ComponentClassOf( var.Type ) == VFXComponentClass::Int ? n : 0;
            info.FloatStart  = layout.TotalFloatComponents;
            info.IntStart    = layout.TotalIntComponents;
            layout.TotalFloatComponents += info.FloatCount;
            layout.TotalIntComponents += info.IntCount;
            layout.Attributes.push_back( std::move( info ) );
        }
        return layout;
    }

    std::filesystem::path EngineModuleDir()
    {
        return Common::Constants::Path::ShaderDir() / "VFX" / "Modules";
    }

    Common::ResultStr<VFXCompiledEmitter> CompileEmitterStack( const S::VFXSystemData&      system,
                                                               std::size_t                  emitterIndex,
                                                               const std::filesystem::path& engineModuleDir )
    {
        using Result = VFXCompiledEmitter;
        if ( emitterIndex >= system.Emitters.size() )
            return Common::MakeFormattedError<Result>( "emitter {} of {}", emitterIndex, system.Emitters.size() );
        // The format's own rules (every source has exactly its member, a User binding names a row of its type, a
        // Random range is ordered) are checked in one place; the compiler relies on them below.
        if ( auto valid = S::ValidateVFXSystemData( system ); !valid.IsSuccess() )
            return Common::MakeError<Result>( valid.GetError() );
        const S::VFXEmitterData& emitter      = system.Emitters[emitterIndex];
        const std::string        emitterWhere = std::format( "emitter '{}'", emitter.Name );

        // 1. Resolve every enabled module once, in first-use order; derive the attribute set.
        std::map<std::string, ResolvedModule> modules;
        std::vector<std::string>              moduleOrder;
        std::map<std::string, VFXValueType>   attributes; // sorted by name: the layout order
        const auto                            addAttribute = [&]( const std::string& name, VFXValueType type,
                                       const std::string& where ) -> Common::BoolResultStr
        {
            const auto [it, inserted] = attributes.emplace( name, type );
            if ( !inserted && it->second != type )
                return Common::MakeFormattedError<bool>(
                     "{}: attribute '{}' is {} here and {} elsewhere in the stack", where, name,
                     GlslTypeName( type ), GlslTypeName( it->second ) );
            return Common::MakeSuccess( true );
        };

        for ( const VFXStackGroup group : { VFXStackGroup::ParticleSpawn, VFXStackGroup::ParticleUpdate } )
        {
            const auto& uses = GroupOf( emitter, group );
            for ( std::size_t m = 0; m < uses.size(); ++m )
            {
                const S::VFXModuleUse& use = uses[m];
                if ( !use.Enabled )
                    continue;
                const std::string where =
                     std::format( "{} {} module {} '{}'", emitterWhere, GroupName( group ), m, use.Module );
                if ( !modules.contains( use.Module ) )
                {
                    auto resolved = ResolveModule( system, use.Module, engineModuleDir, where );
                    if ( !resolved.IsSuccess() )
                        return Common::MakeError<Result>( resolved.GetError() );
                    modules.emplace( use.Module, resolved.ExtractValue() );
                    moduleOrder.push_back( use.Module );
                }
                const VFXParticleModule& module = modules.at( use.Module ).Module;
                for ( const VFXModuleDecl& a : module.Attributes )
                    if ( auto ok = addAttribute( a.Name, a.Type, where ); !ok.IsSuccess() )
                        return Common::MakeError<Result>( ok.GetError() );

                std::set<std::string> seen;
                for ( const S::VFXModuleInput& in : use.Inputs )
                {
                    const std::string inWhere = std::format( "{} input '{}'", where, in.Name );
                    if ( !seen.insert( in.Name ).second )
                        return Common::MakeFormattedError<Result>( "{}: given twice", inWhere );
                    const auto decl = std::find_if( module.Inputs.begin(), module.Inputs.end(),
                                                    [&]( const VFXModuleDecl& d ) { return d.Name == in.Name; } );
                    if ( decl == module.Inputs.end() )
                        return Common::MakeFormattedError<Result>( "{}: the module declares no such input",
                                                                   inWhere );
                    if ( decl->Type != in.Type )
                        return Common::MakeFormattedError<Result>( "{}: the row says {}, the module declares {}",
                                                                   inWhere, GlslTypeName( in.Type ),
                                                                   GlslTypeName( decl->Type ) );
                    // A particle-group curve's time axis is the normalised age, read from these two attributes.
                    if ( in.Source == S::VFXInputSource::Curve )
                        for ( const char* name : { "Age", "Lifetime" } )
                            if ( auto ok = addAttribute( name, VFXValueType::Float, inWhere ); !ok.IsSuccess() )
                                return Common::MakeError<Result>( ok.GetError() );
                    if ( in.Source == S::VFXInputSource::Binding && in.Binding &&
                         in.Binding->starts_with( S::kVFXParticlesPrefix ) )
                        if ( auto ok = addAttribute( in.Binding->substr( S::kVFXParticlesPrefix.size() ), in.Type,
                                                     inWhere );
                             !ok.IsSuccess() )
                            return Common::MakeError<Result>( ok.GetError() );
                }
                for ( const VFXModuleDecl& d : module.Inputs )
                    if ( !seen.contains( d.Name ) )
                        return Common::MakeFormattedError<Result>(
                             "{}: input '{}' is declared by the module and not "
                             "given by the stack row",
                             where, d.Name );
            }
        }
        if ( attributes.empty() )
            return Common::MakeFormattedError<Result>( "{}: the stack declares no particle attribute — there is "
                                                       "nothing to simulate",
                                                       emitterWhere );

        // 2. The layout (sorted by name: one stack, one layout, whatever order the modules declared them in).
        std::vector<VFXModuleDecl> variables;
        for ( const auto& [name, type] : attributes )
            variables.push_back( { name, type } );
        VFXCompiledEmitter compiled;
        compiled.Layout = BuildLayout( variables );

        // 3. The parameter slots and the calls, spawn group then update group, inputs in module-declaration order.
        std::map<std::string, uint32_t> userSlots;
        std::string                     calls[2];
        for ( const VFXStackGroup group : { VFXStackGroup::ParticleSpawn, VFXStackGroup::ParticleUpdate } )
        {
            std::string& out  = calls[group == VFXStackGroup::ParticleSpawn ? 0 : 1];
            const auto&  uses = GroupOf( emitter, group );
            for ( std::size_t m = 0; m < uses.size(); ++m )
            {
                const S::VFXModuleUse& use = uses[m];
                if ( !use.Enabled )
                    continue;
                const ResolvedModule& resolved = modules.at( use.Module );
                std::format_to( std::back_inserter( out ), "        {{\n            {} i;\n", resolved.Inputs );
                // The call's own random key (VFX_ModuleRandom): its place in the stack, top bit up — structure,
                // not a value, so it belongs in the text.
                std::format_to( std::back_inserter( out ), "            i.VFXModuleKey = {}u;\n",
                                0x80000000u + ( ( group == VFXStackGroup::ParticleSpawn ? 0u : 1u ) * 4096u +
                                                static_cast<uint32_t>( m ) ) *
                                                   16u );
                for ( const VFXModuleDecl& d : resolved.Module.Inputs )
                {
                    const S::VFXModuleInput& in = *std::find_if(
                         use.Inputs.begin(), use.Inputs.end(), [&]( const auto& x ) { return x.Name == d.Name; } );
                    const auto  slot  = static_cast<uint32_t>( compiled.Slots.size() );
                    const auto  index = static_cast<uint32_t>( m );
                    std::string expr;
                    switch ( in.Source )
                    {
                        case S::VFXInputSource::Value:
                            compiled.Slots.push_back( { VFXParamSlot::Kind::Value, group, index, d.Name, {} } );
                            expr = ParamExpr( slot, d.Type );
                            break;
                        case S::VFXInputSource::Random:
                            compiled.Slots.push_back(
                                 { VFXParamSlot::Kind::RandomMin, group, index, d.Name, {} } );
                            compiled.Slots.push_back(
                                 { VFXParamSlot::Kind::RandomMax, group, index, d.Name, {} } );
                            expr = RandomExpr( slot, d.Type );
                            break;
                        case S::VFXInputSource::Binding:
                            if ( in.Binding->starts_with( S::kVFXUserPrefix ) )
                            {
                                const std::string user    = in.Binding->substr( S::kVFXUserPrefix.size() );
                                const auto [it, inserted] = userSlots.emplace( user, slot );
                                if ( inserted )
                                    compiled.Slots.push_back(
                                         { VFXParamSlot::Kind::User, group, index, {}, user } );
                                expr = ParamExpr( it->second, d.Type );
                            }
                            else
                                expr = std::format( "p.{}", in.Binding->substr( S::kVFXParticlesPrefix.size() ) );
                            break;
                        case S::VFXInputSource::Curve:
                            compiled.Slots.push_back( { VFXParamSlot::Kind::Curve, group, index, d.Name, {} } );
                            expr = CurveExpr( slot, d.Type );
                            break;
                    }
                    std::format_to( std::back_inserter( out ), "            i.{} = {};\n", d.Name, expr );
                }
                std::format_to( std::back_inserter( out ), "            {}( p, sim, i );\n        }}\n",
                                resolved.Function );
            }
        }

        // 4. The fragment's body: everything that defines the program, and nothing that is a value.
        std::string body = "#include <Common/VFXParticleContract.glslh>\n\nstruct ParticleCtx\n{\n";
        for ( const VFXAttributeLayout& a : compiled.Layout.Attributes )
            std::format_to( std::back_inserter( body ), "    {} {};\n", GlslTypeName( a.Type ), a.Name );
        body += "};\n";
        for ( const std::string& ref : moduleOrder )
        {
            const ResolvedModule& resolved = modules.at( ref );
            std::format_to( std::back_inserter( body ), "\n// {}\nstruct {}\n{{\n", ref, resolved.Inputs );
            body += "    uint VFXModuleKey;\n";
            for ( const VFXModuleDecl& d : resolved.Module.Inputs )
                std::format_to( std::back_inserter( body ), "    {} {};\n", GlslTypeName( d.Type ), d.Name );
            body += "};\n";
            body += RenameModuleSymbols( resolved.Module.Body, resolved.Function, resolved.Inputs );
            if ( !body.ends_with( '\n' ) )
                body += '\n';
        }

        std::string read  = "\nParticleCtx VFX_ReadParticle( uint particle )\n{\n    ParticleCtx p;\n";
        std::string write = "\nvoid VFX_WriteParticle( uint particle, in ParticleCtx p )\n{\n";
        std::string zero  = "\nParticleCtx VFX_ZeroParticle()\n{\n    ParticleCtx p;\n";
        static constexpr std::string_view kLane[] = { "x", "y", "z", "w" };
        for ( const VFXAttributeLayout& a : compiled.Layout.Attributes )
        {
            std::format_to( std::back_inserter( zero ), "    p.{} = {};\n", a.Name, ZeroOf( a.Type ) );
            if ( a.Type == VFXValueType::Int )
            {
                std::format_to( std::back_inserter( read ), "    p.{} = VFX_ReadInt( particle, {}u );\n", a.Name,
                                a.IntStart );
                std::format_to( std::back_inserter( write ), "    VFX_WriteInt( particle, {}u, p.{} );\n",
                                a.IntStart, a.Name );
            }
            else if ( a.Type == VFXValueType::Bool )
            {
                std::format_to( std::back_inserter( read ), "    p.{} = VFX_ReadInt( particle, {}u ) != 0;\n",
                                a.Name, a.IntStart );
                std::format_to( std::back_inserter( write ), "    VFX_WriteInt( particle, {}u, p.{} ? 1 : 0 );\n",
                                a.IntStart, a.Name );
            }
            else if ( a.Type == VFXValueType::Float )
            {
                std::format_to( std::back_inserter( read ), "    p.{} = VFX_ReadFloat( particle, {}u );\n", a.Name,
                                a.FloatStart );
                std::format_to( std::back_inserter( write ), "    VFX_WriteFloat( particle, {}u, p.{} );\n",
                                a.FloatStart, a.Name );
            }
            else
                for ( uint32_t c = 0; c < a.FloatCount; ++c )
                {
                    std::format_to( std::back_inserter( read ), "    p.{}.{} = VFX_ReadFloat( particle, {}u );\n",
                                    a.Name, kLane[c], a.FloatStart + c );
                    std::format_to( std::back_inserter( write ), "    VFX_WriteFloat( particle, {}u, p.{}.{} );\n",
                                    a.FloatStart + c, a.Name, kLane[c] );
                }
        }
        std::format_to( std::back_inserter( body ), "{}    return p;\n}}\n{}}}\n{}    return p;\n}}\n", read, write, zero );
        std::format_to( std::back_inserter( body ),
                        "\nvoid VFX_SimulateParticle( uint particle, inout VFXSim sim )\n{{\n"
                        "    ParticleCtx p = sim.Spawned ? VFX_ZeroParticle() : VFX_ReadParticle( particle );\n"
                        "    if ( sim.Spawned )\n    {{\n{}    }}\n{}"
                        "    VFX_WriteParticle( particle, p );\n}}\n",
                        calls[0], calls[1] );

        // 5. The key is the body's hash: the body is a function of the stack's structure and the layout only (no
        //    value reaches it), so equal structures share one program and any structural change moves it. The
        //    included contract is covered by the shader cache key when the program compiles.
        compiled.Key        = Fnv1a64( std::format( "{}\n{}", kGeneratorTag, body ) );
        compiled.ShaderName = std::format( "VFX/Emitter/{:016x}", compiled.Key );

        std::string        indented;
        std::istringstream lines( body );
        for ( std::string line; std::getline( lines, line ); )
        {
            if ( line.empty() )
                indented += '\n';
            else
                std::format_to( std::back_inserter( indented ), "        {}\n", line );
        }
        compiled.ShaderText =
             std::format( "Shader \"{}\"\n{{\n    Domain Particle\n    Particle\n    {{\n{}    }}\n}}\n",
                          compiled.ShaderName, indented );
        return Common::MakeSuccess( std::move( compiled ) );
    }

    Common::ResultStr<std::vector<glm::vec4>> BuildEmitterParams( const VFXCompiledEmitter& compiled,
                                                                  const S::VFXSystemData&   system,
                                                                  std::size_t               emitterIndex,
                                                                  const VFXCurveAtlas&      curves )
    {
        using Result = std::vector<glm::vec4>;
        if ( emitterIndex >= system.Emitters.size() )
            return Common::MakeFormattedError<Result>( "emitter {} of {}", emitterIndex, system.Emitters.size() );
        const S::VFXEmitterData& emitter = system.Emitters[emitterIndex];
        Result                   rows;
        rows.reserve( compiled.Slots.size() );
        // A slot that no longer finds its row means the stack's STRUCTURE changed after the compile: the program
        // is stale and must be recompiled, so this is an error, never a zero.
        for ( std::size_t s = 0; s < compiled.Slots.size(); ++s )
        {
            const VFXParamSlot& slot = compiled.Slots[s];
            if ( slot.SlotKind == VFXParamSlot::Kind::User )
            {
                const auto it = std::find_if( system.UserParams.begin(), system.UserParams.end(),
                                              [&]( const S::VFXUserParam& p ) { return p.Name == slot.User; } );
                if ( it == system.UserParams.end() )
                    return Common::MakeFormattedError<Result>(
                         "slot {}: no UserParams row '{}' - recompile the stack", s, slot.User );
                rows.push_back( it->Default );
                continue;
            }
            const auto&              uses = GroupOf( emitter, slot.Group );
            const S::VFXModuleInput* in   = nullptr;
            if ( slot.Module < uses.size() )
                for ( const S::VFXModuleInput& x : uses[slot.Module].Inputs )
                    if ( x.Name == slot.Input )
                        in = &x;
            const bool random =
                 slot.SlotKind == VFXParamSlot::Kind::RandomMin || slot.SlotKind == VFXParamSlot::Kind::RandomMax;
            const bool curve   = slot.SlotKind == VFXParamSlot::Kind::Curve;
            const bool present = in != nullptr && ( random  ? in->Random.has_value()
                                                    : curve ? in->Curve.has_value()
                                                            : in->Value.has_value() );
            if ( !present )
                return Common::MakeFormattedError<Result>(
                     "slot {}: emitter '{}' {} module {} input '{}' is gone or "
                     "changed source - recompile the stack",
                     s, emitter.Name, GroupName( slot.Group ), slot.Module, slot.Input );
            if ( curve )
            {
                const VFXCurveLUTEntry* entry =
                     curves.Find( VFXCurveRef{ emitterIndex, slot.Group, slot.Module, slot.Input } );
                if ( entry == nullptr || entry->Channels != in->Curve->size() )
                    return Common::MakeFormattedError<Result>(
                         "slot {}: emitter '{}' {} module {} input '{}' has no table in the curve atlas - rebuild "
                         "the atlas",
                         s, emitter.Name, GroupName( slot.Group ), slot.Module, slot.Input );
                rows.push_back( CurveParamRow( *entry ) );
            }
            else if ( !random )
                rows.push_back( *in->Value );
            else
                rows.push_back( slot.SlotKind == VFXParamSlot::Kind::RandomMin ? in->Random->Min
                                                                               : in->Random->Max );
        }
        return Common::MakeSuccess( std::move( rows ) );
    }
} // namespace Desert::VFX
