#include <Engine/VFX/VFXStackCompiler.hpp>

#include <Engine/VFX/VFXCurveLUT.hpp>
#include <Engine/VFX/VFXEmitterSpawn.hpp>
#include <Engine/VFX/VFXModuleFormat.hpp>

#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>

#include <Common/Core/Constants.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

namespace Desert::VFX
{
    namespace S = Assets::Serialization;

    namespace
    {
        // Bumped when the generated text changes shape, so no cache keyed by an older generator is reused.
        constexpr std::string_view kGeneratorTag = "VFXStackCompiler 2";

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

        /// One distinct module the stack uses, resolved and parsed.
        struct ResolvedModule
        {
            std::string       Name;     ///< the module's DisplayName, for the generated text and messages
            std::string       Function; ///< VFXMod_<guid hex>
            std::string       Inputs;   ///< VFXIn_<guid hex>
            VFXParticleModule Module; ///< Usage, declarations and GLSL of the `.dfxmod`
        };

        /// The key a module is held by in one compile: its GUID, spelled canonically.
        std::string ModuleKey( const S::VFXModuleUse& use )
        {
            const auto guid = Common::Content::AssetGuidFromText( use.Module.Guid );
            return guid ? Common::Content::AssetGuidToText( guid.GetValue() ) : use.Module.Guid;
        }

        Common::ResultStr<ResolvedModule> ResolveModule( const VFXModuleResolver& resolver,
                                                         const S::VFXModuleUse& use, const std::string& where )
        {
            const auto guid = Common::Content::AssetGuidFromText( use.Module.Guid );
            if ( !guid || guid.GetValue().IsNull() )
                return Common::MakeFormattedError<ResolvedModule>( "{}: module GUID '{}' is not well-formed", where,
                                                                   use.Module.Guid );
            auto definition = resolver.Resolve( guid.GetValue() );
            if ( !definition.IsSuccess() )
                return Common::MakeFormattedError<ResolvedModule>( "{}: module {} ('{}') does not resolve - {}",
                                                                   where, use.Module.Guid, use.Module.Path,
                                                                   definition.GetError() );
            VFXModuleDefinition module = definition.ExtractValue();
            const std::string   hex    = Common::Content::AssetGuidToText( guid.GetValue() );
            ResolvedModule      resolved;
            resolved.Name     = module.DisplayName;
            resolved.Function = std::format( "VFXMod_{}", hex );
            resolved.Inputs   = std::format( "VFXIn_{}", hex );
            resolved.Module   = std::move( module.Block );
            return Common::MakeSuccess( std::move( resolved ) );
        }
    } // namespace

    std::string_view StageName( const VFXStage stage )
    {
        switch ( stage )
        {
            case VFXStage::EmitterSpawn:
                return "EmitterSpawn";
            case VFXStage::EmitterUpdate:
                return "EmitterUpdate";
            case VFXStage::ParticleSpawn:
                return "ParticleSpawn";
            case VFXStage::ParticleUpdate:
                return "ParticleUpdate";
        }
        return "EmitterUpdate";
    }

    Common::ResultStr<VFXModuleDescription> DescribeModule( const Assets::AssetGuidRef& module,
                                                            const VFXModuleResolver&    modules )
    {
        const auto guid = Common::Content::AssetGuidFromText( module.Guid );
        if ( !guid || guid.GetValue().IsNull() )
            return Common::MakeFormattedError<VFXModuleDescription>( "module GUID '{}' ('{}') is not well-formed",
                                                                     module.Guid, module.Path );
        auto definition = modules.Resolve( guid.GetValue() );
        if ( !definition.IsSuccess() )
            return Common::MakeFormattedError<VFXModuleDescription>( "module {} ('{}') does not resolve - {}",
                                                                     module.Guid, module.Path, definition.GetError() );
        VFXModuleDefinition  read = definition.ExtractValue();
        VFXModuleDescription description;
        description.Module = module;
        description.Name   = std::move( read.DisplayName );
        description.Usage  = read.Block.Usage;
        description.Inputs = std::move( read.Block.Inputs );
        return Common::MakeSuccess( std::move( description ) );
    }

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

    Common::ResultStr<VFXParticleModule> ParseModuleBlock( const std::string& source, std::string_view where )
    {
        VFXParticleModule     module;
        bool                  sawUsage = false;
        std::set<std::string> attributeNames;
        std::set<std::string> inputNames;
        std::size_t           pos    = 0;
        // Blank and comment lines after the last declaration belong to the GLSL that follows them (a note above
        // the function stays with it through a parse and a write); those before any declaration are skipped.
        std::size_t bodyStart = std::string::npos;
        while ( pos < source.size() )
        {
            const std::size_t  end  = std::min( source.find( '\n', pos ), source.size() );
            const std::string  line = source.substr( pos, end - pos );
            std::istringstream words( line );
            std::string        keyword;
            words >> keyword;
            if ( keyword.empty() || keyword.starts_with( "//" ) )
            {
                if ( bodyStart == std::string::npos )
                    bodyStart = pos;
                pos = end + 1;
                continue;
            }
            if ( keyword == "Usage" )
            {
                if ( sawUsage )
                    return Common::MakeFormattedError<VFXParticleModule>( "{}: `Usage` is declared twice", where );
                sawUsage = true;
                std::string stage;
                while ( words >> stage )
                {
                    uint8_t bit = 0;
                    for ( const VFXStage known : { VFXStage::EmitterSpawn, VFXStage::EmitterUpdate,
                                                   VFXStage::ParticleSpawn, VFXStage::ParticleUpdate } )
                        if ( stage == StageName( known ) )
                            bit = UsageBit( known );
                    if ( bit == 0 )
                        return Common::MakeFormattedError<VFXParticleModule>(
                             "{}: '{}' is not `Usage <EmitterSpawn|EmitterUpdate|ParticleSpawn|ParticleUpdate>...`",
                             where, line );
                    module.Usage |= bit;
                }
                if ( module.Usage == 0 )
                    return Common::MakeFormattedError<VFXParticleModule>( "{}: `Usage` names no stage", where );
                bodyStart = std::string::npos;
                pos       = end + 1;
                continue;
            }
            if ( keyword != "Attribute" && keyword != "Input" )
                break; // the GLSL starts here

            const bool  isInput = keyword == "Input";
            std::string name;
            std::string typeWord;
            words >> name >> typeWord;
            VFXValueType type = VFXValueType::Float;
            bool         ok   = IsIdentifier( name ) && ParseType( typeWord, type );
            glm::vec4    initial( 0.0f );
            bool         bindingRequired = false;
            std::string  extra;
            if ( ok && isInput )
            {
                // `= <one number per component>`: the default a new stack row starts from.
                std::string equals;
                words >> equals;
                ok = equals == "=";
                const uint32_t components = S::ComponentCount( type );
                // `= Binding`: no Value default; a new row binds the input to what the user picks.
                if ( ok && words.peek() != EOF )
                {
                    const std::streampos at = words.tellg();
                    std::string          word;
                    words >> word;
                    if ( word == "Binding" )
                        bindingRequired = true;
                    else
                        words.seekg( at );
                }
                for ( uint32_t c = 0; ok && !bindingRequired && c < components; ++c )
                {
                    std::string number;
                    words >> number;
                    char*       stop  = nullptr;
                    const float value = number.empty() ? 0.0f : std::strtof( number.c_str(), &stop );
                    ok                = !number.empty() && stop != nullptr && *stop == '\0';
                    if ( ok && type == VFXValueType::Bool )
                        ok = value == 0.0f || value == 1.0f;
                    if ( ok && type == VFXValueType::Int )
                        ok = value == static_cast<float>( static_cast<int>( value ) );
                    initial[static_cast<int>( c )] = value;
                }
            }
            words >> extra;
            if ( ( !ok || !extra.empty() ) && isInput )
                return Common::MakeFormattedError<VFXParticleModule>(
                     "{}: '{}' is not `Input <Name> <float|vec2|vec3|vec4|int|bool> = <one number per component | Binding>`",
                     where, line );
            if ( !ok || !extra.empty() )
                return Common::MakeFormattedError<VFXParticleModule>(
                     "{}: '{}' is not `Attribute <Name> <float|vec2|vec3|vec4|int|bool>`", where, line );
            auto& names = isInput ? inputNames : attributeNames;
            if ( !names.insert( name ).second )
                return Common::MakeFormattedError<VFXParticleModule>( "{}: {} '{}' is declared twice", where,
                                                                      keyword, name );
            if ( isInput )
                module.Inputs.push_back( { name, type, initial, bindingRequired } );
            else
                module.Attributes.push_back( { name, type } );
            bodyStart = std::string::npos;
            pos       = end + 1;
        }
        // The comment lines right above the GLSL are its own (the module editor keeps them with the body).
        if ( pos >= source.size() )
            module.Body.clear(); // no GLSL (a CPU-only module): trailing blank lines are not a body
        else
            module.Body = source.substr( bodyStart != std::string::npos ? bodyStart : pos );
        if ( !sawUsage )
            return Common::MakeFormattedError<VFXParticleModule>(
                 "{}: the module declares no `Usage <EmitterSpawn|EmitterUpdate|ParticleSpawn|ParticleUpdate>...`",
                 where );
        // A Particle stage runs the block's GLSL on the GPU; an Emitter stage runs engine C++ registered under the
        // module's GUID (VFXEmitterSpawn). One module is one or the other: its inputs are read by one side.
        const bool particle = ( module.Usage & kVFXParticleUsage ) != 0;
        if ( particle && ( module.Usage & kVFXCpuUsage ) != 0 )
            return Common::MakeFormattedError<VFXParticleModule>(
                 "{}: `Usage` mixes Emitter (CPU) and Particle (GPU) stages", where );

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
        if ( !particle )
        {
            // A CPU module declares inputs only: no attribute is touched and no GLSL runs.
            if ( !module.Attributes.empty() )
                return Common::MakeFormattedError<VFXParticleModule>(
                     "{}: an Emitter-stage module declares no `Attribute` (it runs on the CPU)", where );
            if ( definesModule )
                return Common::MakeFormattedError<VFXParticleModule>(
                     "{}: an Emitter-stage module has no GLSL (its behaviour is engine C++)", where );
            return Common::MakeSuccess( std::move( module ) );
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

    Common::ResultStr<VFXCompiledEmitter> CompileEmitterStack( const S::VFXSystemData&      system,
                                                               std::size_t                  emitterIndex,
                                                               const VFXModuleResolver&     moduleResolver )
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
                const std::string where = std::format( "{} {} module {} '{}' ({})", emitterWhere,
                                                       GroupName( group ), m, use.Module.Path, use.Module.Guid );
                const std::string key   = ModuleKey( use );
                if ( !modules.contains( key ) )
                {
                    auto resolved = ResolveModule( moduleResolver, use, where );
                    if ( !resolved.IsSuccess() )
                        return Common::MakeError<Result>( resolved.GetError() );
                    modules.emplace( key, resolved.ExtractValue() );
                    moduleOrder.push_back( key );
                }
                const ResolvedModule& resolvedUse = modules.at( key );
                const VFXStage        stage       = group == VFXStackGroup::ParticleSpawn ? VFXStage::ParticleSpawn
                                                                                          : VFXStage::ParticleUpdate;
                if ( ( resolvedUse.Module.Usage & UsageBit( stage ) ) == 0 )
                    return Common::MakeFormattedError<Result>( "{}: module '{}' does not state Usage {}", where,
                                                               resolvedUse.Name, StageName( stage ) );
                const VFXParticleModule& module = resolvedUse.Module;
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
                                                    [&]( const VFXModuleInputDecl& d ) { return d.Name == in.Name; } );
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
                for ( const VFXModuleInputDecl& d : module.Inputs )
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
        variables.reserve( attributes.size() );
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
                const ResolvedModule& resolved = modules.at( ModuleKey( use ) );
                std::format_to( std::back_inserter( out ), "        {{\n            {} i;\n", resolved.Inputs );
                // The call's own random key (VFX_ModuleRandom): its place in the stack, top bit up — structure,
                // not a value, so it belongs in the text.
                std::format_to( std::back_inserter( out ), "            i.VFXModuleKey = {}u;\n",
                                0x80000000u + ( ( group == VFXStackGroup::ParticleSpawn ? 0u : 1u ) * 4096u +
                                                static_cast<uint32_t>( m ) ) *
                                                   16u );
                for ( const VFXModuleInputDecl& d : resolved.Module.Inputs )
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
                            if ( in.Binding->starts_with( S::kVFXPointSetPrefix ) )
                            {
                                // VFX-15: one row naming where the placement's set sits in the emitter's own
                                // point-set buffer (first, count); the particle picks a point by its weight
                                // there (VFX_PointSet: binary search of the set's cumulative weights).
                                const std::string set = in.Binding->substr( S::kVFXPointSetPrefix.size() );
                                compiled.Slots.push_back(
                                     { VFXParamSlot::Kind::PointSet, group, index, d.Name, set } );
                                expr = std::format( "VFX_PointSet( VFX_Param( {0}u ), VFX_Random( sim, {0}u ).x )",
                                                    slot );
                            }
                            else if ( in.Binding->starts_with( S::kVFXUserPrefix ) )
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
            std::format_to( std::back_inserter( body ), "\n// {} ({})\nstruct {}\n{{\n", resolved.Name, ref,
                            resolved.Inputs );
            body += "    uint VFXModuleKey;\n";
            for ( const VFXModuleInputDecl& d : resolved.Module.Inputs )
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
        std::format_to( std::back_inserter( body ), "{}    return p;\n}}\n{}}}\n{}    return p;\n}}\n", read,
                        write, zero );
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
            if ( slot.SlotKind == VFXParamSlot::Kind::PointSet )
            {
                rows.emplace_back( 0.0f ); // the placement's set (first, count), written by the GPU world (VFX-15)
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
            const bool curve = slot.SlotKind == VFXParamSlot::Kind::Curve;
            const auto gone  = [&]
            {
                return Common::MakeFormattedError<Result>(
                     "slot {}: emitter '{}' {} module {} input '{}' is gone or "
                     "changed source - recompile the stack",
                     s, emitter.Name, GroupName( slot.Group ), slot.Module, slot.Input );
            };
            if ( in == nullptr )
                return gone();
            if ( curve )
            {
                const auto& channels = in->Curve;
                if ( !channels.has_value() )
                    return gone();
                const VFXCurveLUTEntry* entry =
                     curves.Find( VFXCurveRef{ emitterIndex, slot.Group, slot.Module, slot.Input } );
                if ( entry == nullptr || entry->Channels != channels->size() )
                    return Common::MakeFormattedError<Result>(
                         "slot {}: emitter '{}' {} module {} input '{}' has no table in the curve atlas - rebuild "
                         "the atlas",
                         s, emitter.Name, GroupName( slot.Group ), slot.Module, slot.Input );
                rows.push_back( CurveParamRow( *entry ) );
            }
            else if ( random )
            {
                const auto& range = in->Random;
                if ( !range.has_value() )
                    return gone();
                rows.push_back( slot.SlotKind == VFXParamSlot::Kind::RandomMin ? range->Min : range->Max );
            }
            else
            {
                const auto& value = in->Value;
                if ( !value.has_value() )
                    return gone();
                rows.push_back( *value );
            }
        }
        return Common::MakeSuccess( std::move( rows ) );
    }
} // namespace Desert::VFX
