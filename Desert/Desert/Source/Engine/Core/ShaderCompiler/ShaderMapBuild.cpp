#include "ShaderMapBuild.hpp"

#include <Engine/Core/ShaderCompiler/ShaderCacheKey.hpp>
#include <Engine/Core/ShaderCompiler/ShaderCompiler.hpp>
#include <Engine/Core/ShaderCompiler/ShaderPreprocess/ShaderPreprocessor.hpp>
#include <Engine/Core/ShaderCompiler/ShaderSpirvCache.hpp>
#include <Engine/Core/ShaderCompiler/SharedSamplerBinding.hpp>
#include <Engine/Graphic/Shader.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/JobSystem.hpp>

#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include <format>

namespace Desert::Core
{
    namespace
    {
        uint64_t MapKeyOf( const ShaderMapRequest& request )
        {
            const ScopedShaderPhase timer( ShaderPhase::ShaderMap );
            return ComputeShaderMapKey( request.Source, request.Path, request.PassName, SpirvDebugInfoThisBuild(),
                                        request.Variant );
        }

        Common::ResultStr<ShaderMap> BuildShaderMapUnderKey( const ShaderMapRequest& request,
                                                             const uint64_t          mapKey )
        {
            // THE WARM PATH: the shader map is keyed by the raw text + include files, so a hit is the whole
            // program — metadata and SPIR-V — without parsing the DShader or preprocessing a stage.
            ShaderMapLookup lookup;
            {
                const ScopedShaderPhase timer( ShaderPhase::ShaderMap );
                lookup = TryLoadShaderMap( mapKey );
            }
            if ( lookup.Map )
            {
                CountShaderMapHit();
                return Common::MakeSuccess( std::move( *lookup.Map ) );
            }
            CountShaderMapMiss();
            if ( !lookup.Rejected.empty() )
                LOG_WARN( "[ShaderMap] '{}': cached entry rejected, rebuilding it ({})", request.Name,
                          lookup.Rejected );

            ShaderMap                                             built;
            std::unordered_map<Formats::ShaderStage, std::string> stages;
            {
                const ScopedShaderPhase timer( ShaderPhase::Preprocess );
                // A source that does not parse is THIS shader's refusal, named by its file (the preprocessor
                // prefixes it) and by the program asked for — never a verify on a job-system worker.
                auto preprocessed = Preprocess::ShaderPreprocess::PreProcessPass( request.Source, request.Path,
                                                                                  request.PassName );
                if ( !preprocessed.IsSuccess() )
                    return Common::MakeError<ShaderMap>( std::format(
                         "shader '{}'{}: {}", request.Name,
                         request.PassName.empty() ? std::string() : std::format( " pass '{}'", request.PassName ),
                         preprocessed.GetError() ) );
                auto pass  = preprocessed.ExtractValue();
                built.Meta = std::move( pass.Meta );
                stages     = std::move( pass.Stages );
            }
            if ( built.Meta.HasParams() || built.Meta.State.Topology.has_value() )
                LOG_INFO( "Shader '{}': parsed {} param(s) + render-state from shader metadata", request.Name,
                          built.Meta.Params.size() );

            for ( const auto& [stage, source] : stages )
            {
                auto spirvResult =
                     ShaderCompiler::CompileGLSLToSPIRV( stage, source, request.Path.string(), request.Variant );
                if ( !spirvResult.IsSuccess() )
                    return Common::MakeError<ShaderMap>( spirvResult.GetError() );
                built.Stages.push_back( { stage, spirvResult.GetValue() } );
            }
            std::sort( built.Stages.begin(), built.Stages.end(),
                       []( const ShaderMapStage& a, const ShaderMapStage& b )
                       { return static_cast<uint32_t>( a.Stage ) < static_cast<uint32_t>( b.Stage ); } );
            // Every stage is compiled: the one place every binding of the program is a number (SharedSamplerBinding).
            if ( const auto numbered = AssignSharedSamplerBindings( built.Stages, request.Name ); !numbered )
                return Common::MakeError<ShaderMap>( numbered.GetError() );
            if ( const auto stored = StoreShaderMap( mapKey, built ); !stored )
                LOG_WARN( "[ShaderMap] '{}': could not store the shader map {:016x}: {}", request.Name, mapKey,
                          stored.GetError() );
            return Common::MakeSuccess( std::move( built ) );
        }
    } // namespace

    namespace
    {
        // The few SPIR-V words the shared-sampler rule reads (SPIR-V 1.0 spec, section 3): opcodes and the two
        // decorations. Spelled here rather than taken from spirv.hpp because this module is device-free and
        // reads nothing else of the format.
        constexpr uint32_t kSpvMagic             = 0x07230203u;
        constexpr size_t   kSpvHeaderWords       = 5;
        constexpr uint32_t kSpvOpName            = 5;
        constexpr uint32_t kSpvOpTypeSampler     = 26;
        constexpr uint32_t kSpvOpTypePointer     = 32;
        constexpr uint32_t kSpvOpVariable        = 59;
        constexpr uint32_t kSpvOpLoad            = 61;
        constexpr uint32_t kSpvOpDecorate        = 71;
        constexpr uint32_t kSpvDecorationBinding = 33;
        constexpr uint32_t kSpvDecorationSet     = 34;

        // One descriptor-backed variable of one stage, as the words declare it.
        struct SpvResource
        {
            std::string Name;
            uint32_t    Set         = 0;
            uint32_t    Binding     = 0;
            size_t      BindingWord = 0; // index of the Binding literal in the stage's words
            bool        IsSampler   = false;
            bool        Sampled     = false; // loaded by the stage's code
        };

        // A nul-terminated UTF-8 literal packed four bytes per word, in words [at, end).
        std::string SpvString( const std::vector<uint32_t>& words, const size_t at, const size_t end )
        {
            std::string out;
            for ( size_t w = at; w < end; ++w )
            {
                for ( uint32_t shift = 0; shift < 32; shift += 8 )
                {
                    const char ch = static_cast<char>( ( words[w] >> shift ) & 0xffu );
                    if ( ch == '\0' )
                        return out;
                    out.push_back( ch );
                }
            }
            return out;
        }

        Common::ResultStr<std::vector<SpvResource>> ReadSpvResources( const std::vector<uint32_t>& words )
        {
            if ( words.size() < kSpvHeaderWords || words[0] != kSpvMagic )
                return Common::MakeError<std::vector<SpvResource>>( "the stage is not SPIR-V (bad header)" );
            std::unordered_map<uint32_t, std::string>  names;
            std::unordered_map<uint32_t, uint32_t>     sets;
            std::unordered_map<uint32_t, size_t>       bindingWords;
            std::unordered_set<uint32_t>               samplerTypes;
            std::unordered_map<uint32_t, uint32_t>     pointee;   // pointer type -> pointed-to type
            std::vector<std::pair<uint32_t, uint32_t>> variables; // (id, pointer type)
            std::unordered_set<uint32_t>               loaded;
            for ( size_t at = kSpvHeaderWords; at < words.size(); )
            {
                const uint32_t count  = words[at] >> 16;
                const uint32_t opcode = words[at] & 0xffffu;
                if ( count == 0 || at + count > words.size() )
                    return Common::MakeError<std::vector<SpvResource>>(
                         std::format( "malformed SPIR-V instruction at word {}", at ) );
                if ( opcode == kSpvOpName && count >= 3 )
                    names[words[at + 1]] = SpvString( words, at + 2, at + count );
                else if ( opcode == kSpvOpTypeSampler && count >= 2 )
                    samplerTypes.insert( words[at + 1] );
                else if ( opcode == kSpvOpTypePointer && count >= 4 )
                    pointee[words[at + 1]] = words[at + 3];
                else if ( opcode == kSpvOpVariable && count >= 4 )
                    variables.emplace_back( words[at + 2], words[at + 1] );
                else if ( opcode == kSpvOpLoad && count >= 4 )
                    loaded.insert( words[at + 3] );
                else if ( opcode == kSpvOpDecorate && count >= 4 )
                {
                    if ( words[at + 2] == kSpvDecorationBinding )
                        bindingWords[words[at + 1]] = at + 3;
                    else if ( words[at + 2] == kSpvDecorationSet )
                        sets[words[at + 1]] = words[at + 3];
                }
                at += count;
            }
            std::vector<SpvResource> resources;
            for ( const auto& [id, pointerType] : variables )
            {
                const auto binding = bindingWords.find( id );
                if ( binding == bindingWords.end() )
                    continue; // no descriptor: an input, an output, a push block, a private
                SpvResource resource;
                const auto  name     = names.find( id );
                resource.Name        = name != names.end() ? name->second : std::string();
                const auto set       = sets.find( id );
                resource.Set         = set != sets.end() ? set->second : 0u;
                resource.BindingWord = binding->second;
                resource.Binding     = words[binding->second];
                const auto pointed   = pointee.find( pointerType );
                resource.IsSampler   = pointed != pointee.end() && samplerTypes.contains( pointed->second );
                resource.Sampled     = loaded.contains( id );
                resources.push_back( std::move( resource ) );
            }
            return Common::MakeSuccess( std::move( resources ) );
        }
    } // namespace

    Common::BoolResultStr AssignSharedSamplerBindings( std::vector<ShaderMapStage>& stages,
                                                       const std::string_view       shaderName )
    {
        struct Shared
        {
            uint32_t                               Set     = 0;
            bool                                   Sampled = false;
            std::vector<std::pair<size_t, size_t>> Words; // (stage index, binding word)
            std::string                            Declaring;
        };
        std::vector<std::vector<SpvResource>> perStage;
        perStage.reserve( stages.size() );
        for ( const ShaderMapStage& stage : stages )
        {
            auto read = ReadSpvResources( stage.Spirv );
            if ( !read.IsSuccess() )
                return Common::MakeFormattedError( "shader '{}' [{}]: {}", shaderName,
                                                   Graphic::Shader::GetStringShaderStage( stage.Stage ),
                                                   read.GetError() );
            perStage.push_back( read.ExtractValue() );
        }

        // Every NUMBERED slot of the program, from every stage, gathered before any sampler is judged: a sampler
        // declared first and a buffer declared after it on its binding are the same collision.
        struct Holder
        {
            std::string Name;
            std::string Stage;
            bool        IsSampler = false;
        };
        std::map<std::pair<uint32_t, uint32_t>, std::vector<Holder>> claimed;     // (set, binding) -> holders
        std::map<uint32_t, uint32_t>                                  lastBinding; // set -> one past the highest
        std::map<std::string, Shared>                                 shared;      // ordered: numbered by name
        for ( size_t s = 0; s < stages.size(); ++s )
        {
            const std::string stageName = Graphic::Shader::GetStringShaderStage( stages[s].Stage );
            for ( const SpvResource& resource : perStage[s] )
            {
                if ( resource.Name.empty() )
                    return Common::MakeFormattedError(
                         "shader '{}' [{}]: a resource at set {}, binding {} has no name; the shared-sampler rule "
                         "and the pass bindings identify a resource by its name",
                         shaderName, stageName, resource.Set, resource.Binding );
                if ( resource.Binding == kAutoSamplerBinding )
                {
                    if ( !resource.IsSampler )
                        return Common::MakeFormattedError(
                             "shader '{}' [{}]: '{}' sits on binding {}, the shared-sampler marker; only `Uniform "
                             "sampler <name>;` takes it - give the resource its own binding",
                             shaderName, stageName, resource.Name, kAutoSamplerBinding );
                    Shared& entry = shared[resource.Name];
                    if ( !entry.Words.empty() && entry.Set != resource.Set )
                        return Common::MakeFormattedError( "shader '{}': the shared sampler '{}' is declared in set "
                                                           "{} and in set {}; one sampler is one slot",
                                                           shaderName, resource.Name, entry.Set, resource.Set );
                    entry.Set = resource.Set;
                    entry.Sampled |= resource.Sampled;
                    entry.Words.emplace_back( s, resource.BindingWord );
                    entry.Declaring += std::format( "{}{}", entry.Declaring.empty() ? "" : ", ", stageName );
                    continue;
                }
                claimed[{ resource.Set, resource.Binding }].push_back(
                     { resource.Name, stageName, resource.IsSampler } );
                uint32_t& last = lastBinding[resource.Set];
                last           = std::max( last, resource.Binding + 1 );
            }
        }

        // One slot, one resource. The same name in two stages is one resource declared twice (the merge reflection
        // does); two names on one slot is the collision. Refused here, with both names, when either is a sampler:
        // that is the defect this rule exists for. Every other kind is refused by ShaderReflection::ReflectStage.
        for ( const auto& [slot, holders] : claimed )
        {
            for ( const Holder& sampler : holders )
            {
                if ( !sampler.IsSampler )
                    continue;
                for ( const Holder& other : holders )
                {
                    if ( other.Name == sampler.Name )
                        continue;
                    return Common::MakeFormattedError(
                         "shader '{}' [{}]: the sampler '{}' is on set {}, binding {}, which '{}' [{}] also holds; "
                         "declare a shared sampler as `Uniform sampler {};` and the rule numbers it after the "
                         "program's last resource",
                         shaderName, sampler.Stage, sampler.Name, slot.first, slot.second, other.Name, other.Stage,
                         sampler.Name );
                }
            }
        }

        for ( auto& [name, entry] : shared )
        {
            if ( !entry.Sampled )
                return Common::MakeFormattedError(
                     "shader '{}': the shared sampler '{}' is declared in [{}] and no stage samples through it; "
                     "declare it in the stage that samples, or remove it",
                     shaderName, name, entry.Declaring );
            // After the program's last resource in the set; each next shared sampler of the set after the one before.
            uint32_t&      last    = lastBinding[entry.Set];
            const uint32_t binding = last;
            last                   = binding + 1;
            if ( binding >= kAutoSamplerBinding )
                return Common::MakeFormattedError(
                     "shader '{}': the shared sampler '{}' would be numbered {}, at or past the marker {}", shaderName,
                     name, binding, kAutoSamplerBinding );
            for ( const auto& [stage, word] : entry.Words )
                stages[stage].Spirv[word] = binding;
        }
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<ShaderMap> BuildShaderMap( const ShaderMapRequest& request )
    {
        return BuildShaderMapUnderKey( request, MapKeyOf( request ) );
    }

    std::vector<ShaderMapOutcome> BuildShaderMaps( const std::span<const ShaderMapRequest> requests )
    {
        auto& jobs = ::Common::JobSystem::Get();

        // ONE BUILD PER MAP KEY. Two requests under one key are one artifact (without debug info the key does
        // not name the file, so two files with the same text are one key): built by two workers at once they
        // would both write that key's one <file>.tmp. The first index builds, the others copy its answer.
        std::vector<uint64_t> keys( requests.size() );
        jobs.ParallelFor( requests.size(), [&]( const size_t i ) { keys[i] = MapKeyOf( requests[i] ); } );
        std::vector<size_t>                  builders; // indices that build, in request order
        std::vector<size_t>                  builderOf( requests.size() );
        std::unordered_map<uint64_t, size_t> firstIndexOfKey;
        for ( size_t i = 0; i < requests.size(); ++i )
        {
            const auto [first, inserted] = firstIndexOfKey.try_emplace( keys[i], i );
            builderOf[i]                 = first->second;
            if ( inserted )
                builders.push_back( i );
        }

        std::vector<ShaderMapOutcome> outcomes( requests.size() );
        // One program per claim: a program is milliseconds of shaderc, far above the cost of a claim, and
        // programs differ in cost by 10x, so small claims keep every worker busy to the end.
        jobs.ParallelFor( builders.size(),
                          [&]( const size_t b )
                          {
                              const size_t i     = builders[b];
                              auto         built = BuildShaderMapUnderKey( requests[i], keys[i] );
                              if ( built.IsSuccess() )
                                  outcomes[i].Map = built.GetValue();
                              else
                                  outcomes[i].Error = built.GetError();
                          } );
        for ( size_t i = 0; i < requests.size(); ++i )
            if ( builderOf[i] != i )
                outcomes[i] = outcomes[builderOf[i]];
        return outcomes;
    }
} // namespace Desert::Core
