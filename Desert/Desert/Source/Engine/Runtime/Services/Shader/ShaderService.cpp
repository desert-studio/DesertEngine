#include "ShaderService.hpp"

#include <Common/Core/Logger.hpp>

#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>

#include <format>

namespace
{
    /// The `Medium { ... }` body of @p source AND the Properties block beside it, or an empty body when it
    /// has none. ONE PARSE FOR BOTH: they describe the same file and a second parse could see a different
    /// revision of it after a hot reload.
    ///
    /// THE RESULT-RETURNING PARSER AND NOT ShaderPreprocess::ParseProgramMeta, and the difference is a
    /// crash. ParseProgramMeta ends in a DESERT_VERIFY on anything that is not DSL text — right for a
    /// shader the engine is about to compile, fatal for a QUESTION asked about arbitrary content. The
    /// first version of this asked it once per frame about an asset the eviction sweep had unloaded,
    /// and an emptied asset is not DSL text: the editor died three seconds after the medium applied.
    struct ParsedMedium
    {
        std::string                                     Body;
        std::vector<Desert::Core::Formats::ShaderParam> Properties;
    };

    ParsedMedium MediumOf( const std::string& source )
    {
        if ( source.empty() || !Desert::Core::Preprocess::DShaderParser::MayDeclareMedium( source ) ||
             !Desert::Core::Preprocess::DShaderParser::IsDShader( source ) )
            return {};
        auto parsed = Desert::Core::Preprocess::DShaderParser::Parse( source );
        if ( !parsed.IsSuccess() || parsed.GetValue().Meta.MediumSource.empty() )
            return {};
        return { std::move( parsed.GetValue().Meta.MediumSource ), std::move( parsed.GetValue().Meta.Params ) };
    }
} // namespace

namespace Desert::Runtime
{

    Common::BoolResultStr ShaderService::Register( const std::shared_ptr<Assets::ShaderAsset>& shaderAsset )
    {
        if ( !shaderAsset->GetMetadata().IsValid() )
        {
            return Common::MakeError( "Shader asset is invalid" );
        }

        // A MEDIUM PROGRAM IS SOURCE, NOT A PROGRAM. It declares no stages because it is compiled INTO
        // four other programs as the substitution for one of their includes; building a Shader object for
        // it would produce one with no modules, and the honest complaint below ("registered but has no
        // compiled stages — every material using it will not draw") would be a lie about a file that is
        // working exactly as intended.
        //
        // THE TEXT IS COPIED HERE AND THE ASSET IS NOT KEPT, which is the fix for a measured crash. The
        // first version read the body back out of the asset on every frame, and the asset eviction sweep
        // unloads a shader asset nothing holds — so three seconds after a medium was applied the source
        // was empty, and an empty asset is not DSL text. Holding the ASSET instead would have fought the
        // sweep for the sake of a few kilobytes of text; holding the text is bounded, immune to the
        // sweep, and re-read by RefreshMediumSource when the file on disk changes.
        if ( ParsedMedium medium = MediumOf( shaderAsset->GetShaderContent() ); !medium.Body.empty() )
        {
            const auto name         = shaderAsset->GetMetadata().Filepath.stem().string();
            m_NameToHandleMap[name] = shaderAsset->GetMetadata().Handle;
            LOG_INFO( "[ShaderService] '{}' is a Volume medium ({} bytes of authored source, {} own "
                      "propert(ies)); it compiles into the programs that sample the cloud field rather "
                      "than into one of its own.",
                      name, medium.Body.size(), medium.Properties.size() );
            m_Media[shaderAsset->GetMetadata().Handle] =
                 MediumEntry{ std::move( medium.Body ), std::move( medium.Properties ) };
            return BOOLSUCCESS;
        }

        const auto shader                            = Graphic::Shader::Create( shaderAsset );
        m_Shaders[shaderAsset->GetMetadata().Handle] = shader;
        m_NameToHandleMap[shader->GetName()]         = shaderAsset->GetMetadata().Handle;
        // Kept so AcquireVariant can compile the SAME source under a substitution later. Weak: the
        // asset manager owns the asset, and this service must not extend its life.
        m_ShaderAssets[shaderAsset->GetMetadata().Handle] = shaderAsset;
        // Whose the shader is, in the ledger — see Engine/Graphic/ResourceLedger.hpp.
        shader->ClaimOwnership( Graphic::ResourceOwner::AssetService, shaderAsset->GetMetadata().Handle );

        // Registered either way, deliberately: a shader that fails to compile must keep its NAME, or the
        // material referencing it silently falls back to the standard one and the artist is told nothing.
        // It is registered and unusable, and it says so once, here, naming itself — the per-stage error
        // above names a file and a line, which is not the same as naming the shader a material asks for.
        //
        // "WILL NOT DRAW" IS A PROMISE, AND UNTIL Г21 THIS SERVICE WAS THE ONLY PLACE THAT MADE IT.
        // GetByName() hands this object to anyone who asks, and what happened next was neither a skipped
        // draw nor a refusal: the compute path read the empty stage list at [0] and the process died,
        // and the graphics path bound VK_NULL_HANDLE at every submit entry. Three sites keep the promise
        // now, and they are named here because a promise whose keeper is not written down is how this one
        // went unkept for the life of the engine:
        //   * ComputePipeline::Create              refuses to build, and its caller sees the reason;
        //   * VulkanPipeline::Invalidate           refuses to build, leaving the handle null;
        //   * VulkanRendererAPI::BindGraphicsPipeline  skips every draw through that null handle.
        if ( !shader->IsCompiled() )
            LOG_ERROR( "[ShaderService] '{}' registered but has no compiled stages — every material using "
                       "it will not draw until it compiles ({}).",
                       shader->GetName(), shaderAsset->GetMetadata().Filepath.string() );

        // DSL multi-pass shaders: every named pass is its own program, addressable as
        // "<Shader>/<Pass>" (e.g. GetByName("Unlit/Shadow")).
        for ( const auto& passName : shader->GetProgramMeta().PassNames )
        {
            auto passShader                      = Graphic::Shader::Create( shaderAsset, {}, passName );
            m_PassShaders[passShader->GetName()] = passShader;
            passShader->ClaimOwnership( Graphic::ResourceOwner::AssetService, shaderAsset->GetMetadata().Handle );
        }

        return BOOLSUCCESS;
    }

    std::shared_ptr<Graphic::Shader> ShaderService::GetByName( const std::string& name ) const
    {
        auto handleIt = m_NameToHandleMap.find( name );
        if ( handleIt != m_NameToHandleMap.end() )
        {
            return Get( handleIt->second );
        }

        auto passIt = m_PassShaders.find( name );
        if ( passIt != m_PassShaders.end() )
        {
            return passIt->second;
        }
        return nullptr;
    }

    std::shared_ptr<Desert::Graphic::Shader> ShaderService::Get( const Assets::AssetHandle& handle ) const
    {
        auto it = m_Shaders.find( handle );
        return ( it != m_Shaders.end() ) ? it->second : nullptr;
    }

    std::shared_ptr<Graphic::Shader> ShaderService::AcquireVariant( const std::string&            name,
                                                                    const Graphic::ShaderVariant& variant )
    {
        // The default variant is GetByName's question, and answering it here would build an unregistered
        // second copy of a program that already exists — two objects, two sets of modules, and a
        // material picking whichever it was handed.
        if ( variant.IsDefault() )
        {
            LOG_ERROR( "[ShaderService] AcquireVariant('{}') was asked for the DEFAULT variant. That is "
                       "GetByName's question; serving it here would build a second copy of a registered "
                       "program.",
                       name );
            return nullptr;
        }

        const auto handleIt = m_NameToHandleMap.find( name );
        if ( handleIt == m_NameToHandleMap.end() )
        {
            LOG_ERROR( "[ShaderService] AcquireVariant: no program named '{}' is registered.", name );
            return nullptr;
        }

        const std::string key =
             std::format( "{}#{:016x}", name, static_cast<unsigned long long>( variant.Hash() ) );

        if ( const auto it = m_Variants.find( key ); it != m_Variants.end() )
        {
            if ( auto live = it->second.Program.lock() )
                return live;
            m_Variants.erase( it ); // the last holder let it go; build a fresh one below
        }

        const auto assetIt = m_ShaderAssets.find( handleIt->second );
        auto       asset   = assetIt != m_ShaderAssets.end() ? assetIt->second.lock() : nullptr;
        if ( !asset )
        {
            LOG_ERROR( "[ShaderService] AcquireVariant('{}'): the shader asset behind that name is gone.", name );
            return nullptr;
        }

        // RE-READ IF THE SWEEP HAS BEEN THROUGH. A registered shader compiles ONCE at startup and never
        // looks at its asset again, so the asset eviction sweep is free to release the text — and it
        // does, marking two hundred assets cold as soon as a scene finishes loading. A variant is
        // compiled LATER, from that same text, and the parser's answer to an empty file is a fatal
        // engine error. This cost a thumbnail sweep and the whole editor with it, three seconds after a
        // cloud medium applied, and it is the ordinary idiom besides: the hot-reload poll loads before
        // it reloads, for the same reason.
        if ( asset->GetShaderContent().empty() )
        {
            if ( const auto loaded = asset->Load(); !loaded )
            {
                LOG_ERROR( "[ShaderService] AcquireVariant('{}'): the shader's source had been released "
                           "and could not be re-read: {}",
                           name, loaded.GetError() );
                return nullptr;
            }
        }

        auto program = Graphic::Shader::Create( asset, variant );
        program->ClaimOwnership( Graphic::ResourceOwner::AssetService, handleIt->second );
        if ( !program->IsCompiled() )
        {
            // A REFUSAL, NOT A RESULT — and it used to be handed back with a comment saying "the caller
            // decides whether to draw with the default instead". No caller decided. Both of them
            // (VolumetricCloudRenderer::BuildMediumPipelines and ComputeImages::BakeProceduralPanorama)
            // test the pointer and nothing else, so each already carries a fall-back-to-the-shipped-medium
            // branch that could never run, and each handed this object to ComputePipeline::Create instead.
            // VulkanPipelineCompute::Invalidate then reads GetPipelineShaderStageCreateInfos()[0] — of a
            // vector a failed compile leaves EMPTY.
            //
            // That is reachable from a graph an artist can draw: the compile fails on any GLSL error the
            // emitter can produce, and — the case О1-G is about — on a medium that declares its own
            // resource at a binding one of the four consumers already holds, which glslang accepts in
            // silence and ShaderReflection::ReflectStage refuses by name (Г17).
            //
            // Refusing here rather than at each call site is what makes those two branches true. It costs
            // nothing the header did not already say: the doc lists the refusals and the log names which
            // one this is, so "unknown name" and "did not compile" are still distinguishable to a reader —
            // they were never distinguishable to the CODE, which is what mattered.
            LOG_ERROR( "[ShaderService] Variant {} of '{}' has no compiled stages — the substituted "
                       "source did not compile. The caller falls back to the shipped program.",
                       key, name );
            return nullptr;
        }

        m_Variants[key] = VariantEntry{ handleIt->second, program };
        return program;
    }

    std::string ShaderService::MediumSourceOf( const Assets::AssetHandle& handle )
    {
        if ( handle.IsNull() )
            return {};

        if ( const auto it = m_Media.find( handle ); it != m_Media.end() )
            return it->second.Source;

        // A HANDLE THAT IS NOT A MEDIUM, and the two ways to get there are an unregistered shader and a
        // Medium slot pointed at an ordinary one. Both draw the shipped medium, which is a picture that
        // looks exactly like "I forgot to set it" — so it is said, once per handle, with the handle in
        // it. Once, because this is asked every frame.
        if ( m_WarnedNotAMedium.insert( static_cast<uint64_t>( handle ) ).second )
        {
            LOG_ERROR( "[ShaderService] a cloud material's Medium slot names shader {}, which is not a "
                       "registered Volume medium (no `Medium {{ ... }}` block, or not loaded). The layer "
                       "draws the DEFAULT medium.",
                       static_cast<uint64_t>( handle ) );
        }
        return {};
    }

    const std::vector<Core::Formats::ShaderParam>*
    ShaderService::MediumSchemaOf( const Assets::AssetHandle& handle ) const
    {
        if ( handle.IsNull() )
            return nullptr;

        // NO WARNING HERE, and that is deliberate rather than an omission: MediumSourceOf is asked the same
        // question about the same handle in the same frame and latches the one message. Two refusals for
        // one mistake is how a log stops being read.
        const auto it = m_Media.find( handle );
        return it != m_Media.end() ? &it->second.Properties : nullptr;
    }

    bool ShaderService::RefreshMediumSource( const Assets::AssetHandle& handle, const std::string& content )
    {
        ParsedMedium medium = MediumOf( content );
        if ( medium.Body.empty() )
            return false;

        // A file that STOPS being a medium keeps its old body rather than silently reverting the sky to
        // the engine's: an artist mid-edit has a half-saved file for a moment, and a sky that flickered
        // back to the default every time they saved would be worse than one frame of stale text. The
        // empty case above is therefore "not a medium", not "an empty medium" — the parser refuses an
        // empty Medium block outright, for the same reason.
        m_Media[handle] = MediumEntry{ std::move( medium.Body ), std::move( medium.Properties ) };
        m_WarnedNotAMedium.erase( static_cast<uint64_t>( handle ) );
        return true;
    }

    int ShaderService::ReloadVariantsOf( const Assets::AssetHandle& handle )
    {
        int reloaded = 0;
        for ( auto it = m_Variants.begin(); it != m_Variants.end(); )
        {
            auto program = it->second.Handle == handle ? it->second.Program.lock() : nullptr;
            if ( it->second.Program.expired() )
            {
                it = m_Variants.erase( it );
                continue;
            }
            if ( program )
            {
                const auto res = program->Reload();
                if ( !res )
                {
                    LOG_ERROR( "[ShaderService] Variant {} failed to recompile: {}", it->first, res.GetError() );
                }
                else
                {
                    ++reloaded;
                }
            }
            ++it;
        }
        return reloaded;
    }

    void ShaderService::Clear()
    {
        // Was an empty body. Shaders own VkShaderModules and the pipeline layouts built from them.
        m_Shaders.clear();
        m_PassShaders.clear();
        m_NameToHandleMap.clear();
        m_ShaderAssets.clear();
        m_Media.clear();
        m_WarnedNotAMedium.clear();
        // Only the weak bookkeeping — a variant's modules belong to whoever still holds it, and freeing
        // them from here would leave that holder with a program made of destroyed modules.
        m_Variants.clear();
    }

    std::vector<std::string> ShaderService::GetAllNames() const
    {
        std::vector<std::string> names;
        names.reserve( m_NameToHandleMap.size() );
        for ( const auto& [name, handle] : m_NameToHandleMap )
            names.push_back( name );
        return names;
    }

} // namespace Desert::Runtime