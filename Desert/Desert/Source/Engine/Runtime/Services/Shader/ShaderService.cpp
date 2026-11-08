#include "ShaderService.hpp"

#include <Common/Core/Logger.hpp>
#include <Common/Settings/Scalability.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/PipelineBuilds.hpp>

#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>

#include <algorithm>
#include <format>
#include <mutex>
#include <shared_mutex>
#include <utility>

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

    /// The body of a `Domain DeformStamp` file's Stamp block, or empty (the same result-returning parse as MediumOf).
    std::string StampOf( const std::string& source )
    {
        if ( source.empty() || !Desert::Core::Preprocess::DShaderParser::MayDeclareStamp( source ) ||
             !Desert::Core::Preprocess::DShaderParser::IsDShader( source ) )
            return {};
        auto parsed = Desert::Core::Preprocess::DShaderParser::Parse( source );
        if ( !parsed.IsSuccess() )
            return {};
        return std::move( parsed.GetValue().Meta.StampSource );
    }

    bool IsParticleFragment( const std::string& source )
    {
        if ( source.empty() || !Desert::Core::Preprocess::DShaderParser::MayDeclareParticle( source ) ||
             !Desert::Core::Preprocess::DShaderParser::IsDShader( source ) )
            return false;
        const auto parsed = Desert::Core::Preprocess::DShaderParser::Parse( source );
        return parsed.IsSuccess() && !parsed.GetValue().Meta.ParticleSource.empty();
    }

    /// The one complaint about a registered program that has no compiled stages, for the default program AND
    /// every named pass ("<Shader>/<Pass>" is a program a renderer asks for by name exactly as it asks for the
    /// default one). A device refusal is a warning (the same source builds on a device with the feature);
    /// anything else is an error that names the program, the file and the build's own reason.
    void ReportUnbuiltProgram( const Desert::Graphic::Shader& shader, const std::filesystem::path& file )
    {
        if ( shader.IsCompiled() )
            return;
        if ( const std::string refusal = shader.GetDeviceRefusal(); !refusal.empty() )
        {
            LOG_WARN( "[ShaderService] '{}' is not built for this device: {} ({}).", shader.GetName(), refusal,
                      file.string() );
            return;
        }
        std::string why = shader.GetCompileError();
        if ( why.empty() )
            why = "the build gave no reason";
        LOG_ERROR( "[ShaderService] '{}' registered but has no compiled stages — every material using it will not "
                   "draw until it compiles ({}): {}",
                   shader.GetName(), file.string(), why );
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
        // A (re-)registered file answers afresh: a refusal that named it, or named its name before it existed,
        // is no longer the service's answer. Done under the same exclusive hold as the write it belongs to.
        const Assets::AssetHandle handle = shaderAsset->GetMetadata().Handle;
        const std::string         stem   = shaderAsset->GetMetadata().Filepath.stem().string();
        const auto                adopt  = [this, &handle, &stem]()
        {
            ForgetRefusalsOfLocked( handle );
            ForgetRefusalsOfProgram( stem );
            m_NameToHandleMap[stem] = handle;
        };

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
        // A PARTICLE FRAGMENT IS SOURCE TOO (VFX-04): a stack module or a compiled emitter stack, read by the
        // stack compiler and compiled into the simulation program. Registered by name so it can be found,
        // never built as a program — the "no compiled stages" complaint below would be a lie about it.
        if ( IsParticleFragment( shaderAsset->GetShaderContent() ) )
        {
            {
                const std::unique_lock write( m_Lock );
                adopt();
            }
            LOG_INFO( "[ShaderService] '{}' is a Particle fragment; it compiles into the particle simulation "
                      "rather than into a program of its own.",
                      stem );
            return BOOLSUCCESS;
        }

        // A DEFORM STAMP IS SOURCE TOO (DEFORM-FIELD): the shape of a deformation stamp, compiled into the field's
        // stamp program as the substitution for Deformation/DeformStampBody.glslh. Found by NAME (World.deform
        // names its stamp), never built as a program of its own.
        if ( std::string stamp = StampOf( shaderAsset->GetShaderContent() ); !stamp.empty() )
        {
            const std::size_t bytes = stamp.size();
            {
                const std::unique_lock write( m_Lock );
                adopt();
                m_Stamps[stem] = std::move( stamp );
            }
            LOG_INFO( "[ShaderService] '{}' is a DeformStamp ({} bytes of authored source); it compiles into "
                      "the deformation field's stamp program rather than into one of its own.",
                      stem, bytes );
            return BOOLSUCCESS;
        }

        if ( ParsedMedium medium = MediumOf( shaderAsset->GetShaderContent() ); !medium.Body.empty() )
        {
            const std::size_t bytes      = medium.Body.size();
            const std::size_t properties = medium.Properties.size();
            auto              schema =
                 std::make_shared<const std::vector<Core::Formats::ShaderParam>>( std::move( medium.Properties ) );
            {
                const std::unique_lock write( m_Lock );
                adopt();
                m_Media[handle] = MediumEntry{ std::move( medium.Body ), std::move( schema ) };
            }
            LOG_INFO( "[ShaderService] '{}' is a Volume medium ({} bytes of authored source, {} own "
                      "propert(ies)); it compiles into the programs that sample the cloud field rather "
                      "than into one of its own.",
                      stem, bytes, properties );
            return BOOLSUCCESS;
        }

        // EVERY PROGRAM OF THE FILE IS COMPILED BEFORE THE LOCK (THREADS): a compile reads the asset alone, and a
        // render worker asking GetByName meanwhile is answered by the programs registered before this one.
        const auto shader = Graphic::Shader::Create( shaderAsset );
        // Whose the shader is, in the ledger — see Engine/Graphic/ResourceLedger.hpp.
        shader->ClaimOwnership( Graphic::ResourceOwner::AssetService, handle );

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
        ReportUnbuiltProgram( *shader, shaderAsset->GetMetadata().Filepath );

        // DSL multi-pass shaders: every named pass is its own program, addressable as
        // "<Shader>/<Pass>" (e.g. GetByName("Unlit/Shadow")).
        //
        // The pass the default program IS (a surface template's default cell, or the first named pass of a shader
        // without top-level stages: ShaderProgramMeta::DefaultPassName) is NOT compiled a second time: its
        // "<Shader>/<Pass>" name answers the default program above, the same object.
        // Ray tracing passes exist on a device with the ray tracing pipeline alone (ShaderProgramMeta::
        // RayTracingPassNames); elsewhere their names stay unregistered and a pass that asks for one refuses,
        // naming the device (Graphic::RayTracingPipeline::Create). The DEVICE's catalog, not QualityState's copy
        // of it: that copy is empty until QualityBoot::Start, and a Register before it would drop every ray
        // tracing pass in silence. The device exists whenever a shader compiles (VulkanShader reads it).
        const auto device = EngineContext::GetInstance().GetDevice();
        std::vector<std::pair<std::string, std::shared_ptr<Graphic::Shader>>> passes;
        if ( device )
        {
            const bool rayTracingPipeline = Common::Scalability::CapabilityCatalog::Offers(
                 device->GetCapabilities().Catalog.RayTracingModes,
                 Common::Scalability::RayTracingMode::RayTracingPipeline );
            const auto& rayTracingPasses = shader->GetProgramMeta().RayTracingPassNames;
            for ( const auto& passName : shader->GetProgramMeta().PassNames )
            {
                if ( !rayTracingPipeline && std::find( rayTracingPasses.begin(), rayTracingPasses.end(),
                                                       passName ) != rayTracingPasses.end() )
                    continue;
                if ( Core::Preprocess::IsDefaultProgramPass( shader->GetProgramMeta(), passName ) )
                {
                    passes.emplace_back( std::format( "{}/{}", shader->GetName(), passName ), shader );
                    continue;
                }
                auto passShader = Graphic::Shader::Create( shaderAsset, {}, passName );
                passShader->ClaimOwnership( Graphic::ResourceOwner::AssetService, handle );
                // Registered either way, for the default program's reason above, and complained about the same
                // way.
                ReportUnbuiltProgram( *passShader, shaderAsset->GetMetadata().Filepath );
                std::string passKey = passShader->GetName();
                passes.emplace_back( std::move( passKey ), std::move( passShader ) );
            }
        }

        // THE ADOPTION, EXCLUSIVE. The programs a re-registration replaces leave the maps under the lock and are
        // released after it (declared before it): a render worker holding one keeps its own reference.
        std::vector<std::shared_ptr<Graphic::Shader>> replaced;
        {
            const std::unique_lock write( m_Lock );
            ForgetRefusalsOfLocked( handle );
            ForgetRefusalsOfProgram( stem );
            if ( auto& slot = m_Shaders[handle]; slot != shader )
                replaced.push_back( std::exchange( slot, shader ) );
            m_NameToHandleMap[shader->GetName()] = handle;
            ForgetRefusalsOfProgram( shader->GetName() );
            // Kept so AcquireVariant can compile the SAME source under a substitution later. Weak: the
            // asset manager owns the asset, and this service must not extend its life.
            m_ShaderAssets[handle] = shaderAsset;
            for ( auto& [passKey, passShader] : passes )
                replaced.push_back( std::exchange( m_PassShaders[passKey], std::move( passShader ) ) );
        }

        if ( !device )
            return Common::MakeFormattedError( "ShaderService: '{}' registered before the device exists",
                                               shader->GetName() );
        return BOOLSUCCESS;
    }

    std::shared_ptr<Graphic::Shader> ShaderService::GetByName( const std::string& name ) const
    {
        const std::shared_lock read( m_Lock );
        auto                   handleIt = m_NameToHandleMap.find( name );
        if ( handleIt != m_NameToHandleMap.end() )
        {
            return GetLocked( handleIt->second );
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
        const std::shared_lock read( m_Lock );
        return GetLocked( handle );
    }

    std::shared_ptr<Graphic::Shader> ShaderService::GetLocked( const Assets::AssetHandle& handle ) const
    {
        auto it = m_Shaders.find( handle );
        return ( it != m_Shaders.end() ) ? it->second : nullptr;
    }

    Common::ResultStr<std::shared_ptr<Graphic::Shader>>
    ShaderService::AcquireVariant( const std::string& name, const Graphic::ShaderVariant& variant )
    {
        auto lookup = BeginVariant( name, variant );
        // The compile runs unlocked, so another thread may move the service between the steps (a recompile, a
        // finish for the same key): FinishVariant then answers BeginVariant's way, and this asks again.
        while ( lookup.IsSuccess() && lookup.GetValue().Compile )
        {
            const VariantCompile ticket = *lookup.GetValue().Compile;
            lookup                      = FinishVariant( ticket, Core::BuildShaderMap( ticket.Request ) );
        }
        if ( !lookup.IsSuccess() )
            return Common::MakeError<std::shared_ptr<Graphic::Shader>>( lookup.GetError() );
        return Common::MakeSuccess( lookup.GetValue().Ready );
    }

    Common::ResultStr<ShaderService::VariantLookup>
    ShaderService::BeginVariant( const std::string& name, const Graphic::ShaderVariant& variant )
    {
        const std::string key =
             std::format( "{}#{:016x}", name, static_cast<unsigned long long>( variant.Hash() ) );
        // THE STEADY FRAME READS (THREADS): a live variant or a standing refusal under the shared lock, so the
        // render workers asking every frame do not queue behind one another.
        {
            const std::shared_lock read( m_Lock );
            if ( auto settled = SettledVariantLocked( variant, key ) )
                return std::move( *settled );
        }
        const std::unique_lock write( m_Lock );
        return BeginVariantLocked( name, variant, key );
    }

    std::optional<Common::ResultStr<ShaderService::VariantLookup>>
    ShaderService::SettledVariantLocked( const Graphic::ShaderVariant& variant, const std::string& key ) const
    {
        // Each step is BeginVariantLocked's, in its order; whatever writes (the default refusal, the cook census'
        // first record, an expired entry) is left to it.
        if ( variant.IsDefault() || ( variant.ShippedByCook && !m_ShippedByCookRequests.contains( key ) ) )
            return std::nullopt;
        const auto standing = m_Refusals.find( key );
        if ( standing != m_Refusals.end() && standing->second.Compile )
            return Common::MakeError<VariantLookup>( standing->second.Reason );
        if ( const auto it = m_Variants.find( key ); it != m_Variants.end() )
            if ( auto live = it->second.Program.lock() )
                return Common::MakeSuccess( VariantLookup{ .Ready = std::move( live ) } );
        return std::nullopt;
    }

    Common::ResultStr<ShaderService::VariantLookup> ShaderService::BeginVariantLocked(
         const std::string& name, const Graphic::ShaderVariant& variant, const std::string& key )
    {
        const auto refused = [this]( const std::string& refusedKey, Refusal refusal )
        {
            return Common::MakeError<VariantLookup>( Refuse( refusedKey, std::move( refusal ) ).GetError() );
        };

        // The default variant is GetByName's question, and answering it here would build an unregistered
        // second copy of a program that already exists — two objects, two sets of modules, and a
        // material picking whichever it was handed.
        if ( variant.IsDefault() )
        {
            return refused( key,
                           { .Program = name,
                             .Reason  = std::format(
                                  "AcquireVariant('{}') was asked for the DEFAULT variant. That is GetByName's "
                                   "question; serving it here would build a second copy of a registered program.",
                                  name ) } );
        }

        // A NAMED PASS ("<Shader>/<Pass>", as GetByName answers it) is compiled from its shader's asset with
        // that pass selected — the RSM draws the G-buffer CELL of the default surface under its own macro.
        // A surface template's default cell IS the default program, so it is compiled with no pass name,
        // exactly as RegisterShader built it.
        std::string programName = name;
        std::string passName;
        if ( !m_NameToHandleMap.contains( name ) )
        {
            const auto passIt = m_PassShaders.find( name );
            const auto slash  = name.rfind( '/' );
            if ( passIt != m_PassShaders.end() && slash != std::string::npos )
            {
                programName                = name.substr( 0, slash );
                const auto programHandleIt = m_NameToHandleMap.find( programName );
                if ( programHandleIt != m_NameToHandleMap.end() &&
                     GetLocked( programHandleIt->second ) != passIt->second )
                    passName = name.substr( slash + 1 );
            }
        }

        const auto handleIt = m_NameToHandleMap.find( programName );
        if ( handleIt == m_NameToHandleMap.end() )
        {
            return refused( key, { .Program = programName,
                                  .Reason  = std::format(
                                       "AcquireVariant: no program or pass named '{}' is registered.", name ) } );
        }

        if ( variant.ShippedByCook )
            m_ShippedByCookRequests.try_emplace( key, Core::CookedProgramVariant{ name, variant } );

        // A STANDING COMPILE REFUSAL IS THE ANSWER until the file is re-read (ForgetRefusalsOf): the same bytes
        // under the same variant fail the same way, and recompiling them every frame that asks is the cost of a
        // glslang run per frame for a sentence the caller was already given.
        if ( const auto standing = m_Refusals.find( key ); standing != m_Refusals.end() && standing->second.Compile )
            return Common::MakeError<VariantLookup>( standing->second.Reason );

        if ( const auto it = m_Variants.find( key ); it != m_Variants.end() )
        {
            if ( auto live = it->second.Program.lock() )
                return Common::MakeSuccess( VariantLookup{ .Ready = std::move( live ) } );
            m_Variants.erase( it ); // the last holder let it go; build a fresh one below
        }

        const auto assetIt = m_ShaderAssets.find( handleIt->second );
        auto       asset   = assetIt != m_ShaderAssets.end() ? assetIt->second.lock() : nullptr;
        if ( !asset )
        {
            return refused(
                 key, { .Handle  = handleIt->second,
                        .Program = programName,
                        .Reason  = std::format( "AcquireVariant('{}'): the shader asset behind that name is gone.",
                                                name ) } );
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
                return refused(
                     key, { .Handle  = handleIt->second,
                            .Program = programName,
                            .Reason  = std::format( "AcquireVariant('{}'): the shader's source had been released "
                                                     "and could not be re-read: {}",
                                                    name, loaded.GetError() ) } );
            }
        }

        // The compile's whole input, copied: the job reads this and nothing else (VariantCompile).
        const auto&                  path = asset->GetMetadata().Filepath;
        std::string                  shown = path.stem().string();
        if ( !passName.empty() )
            shown = std::format( "{}/{}", shown, passName );
        return Common::MakeSuccess( VariantLookup{
             .Compile = VariantCompile{ .Key        = key,
                                        .Name       = name,
                                        .Program    = programName,
                                        .Handle     = handleIt->second,
                                        .Request    = { asset->GetShaderContent(), path, passName, variant, shown },
                                        .Generation = m_RecompileGeneration.load() } } );
    }

    Common::ResultStr<ShaderService::VariantLookup>
    ShaderService::FinishVariant( const VariantCompile& ticket, const Common::ResultStr<Core::ShaderMap>& built )
    {
        // Declared first, released last: a program built here that is not adopted (a refusal, or another finish
        // for the same key landed first) is destroyed with the lock released (THREADS).
        std::shared_ptr<Graphic::Shader>     program;
        std::shared_ptr<Assets::ShaderAsset> asset;
        // Moved past while it compiled: the file or the program may have been re-read since the copy was taken,
        // and adopting the old text's map would be the stale program the refusal reset was there to drop.
        const auto moved = [this, &ticket]() { return ticket.Generation != m_RecompileGeneration.load(); };
        {
            const std::shared_lock read( m_Lock );
            if ( !moved() )
            {
                // A finish for the same key may have landed first (two callers, one key): its program is the
                // answer.
                if ( const auto it = m_Variants.find( ticket.Key ); it != m_Variants.end() )
                    if ( auto live = it->second.Program.lock() )
                        return Common::MakeSuccess( VariantLookup{ .Ready = std::move( live ) } );
                if ( const auto assetIt = m_ShaderAssets.find( ticket.Handle ); assetIt != m_ShaderAssets.end() )
                    asset = assetIt->second.lock();
            }
        }
        if ( moved() )
            return BeginVariant( ticket.Name, ticket.Request.Variant );

        // The device half, unlocked: it reads the ticket's map and the asset alone.
        if ( asset && built.IsSuccess() )
        {
            program = Graphic::Shader::CreateFromShaderMap( asset, ticket.Request.Variant, ticket.Request.PassName,
                                                            built.GetValue() );
            program->ClaimOwnership( Graphic::ResourceOwner::AssetService, ticket.Handle );
        }

        std::unique_lock write( m_Lock );
        // Moved while the device half ran: the same answer as above.
        if ( moved() )
        {
            write.unlock();
            return BeginVariant( ticket.Name, ticket.Request.Variant );
        }
        if ( const auto it = m_Variants.find( ticket.Key ); it != m_Variants.end() )
            if ( auto live = it->second.Program.lock() )
                return Common::MakeSuccess( VariantLookup{ .Ready = std::move( live ) } );

        const auto refused = [&]( const std::string& reason, const bool compile )
        {
            return Common::MakeError<VariantLookup>(
                 Refuse( ticket.Key,
                         { .Handle = ticket.Handle, .Program = ticket.Program, .Reason = reason, .Compile = compile } )
                      .GetError() );
        };
        if ( !asset )
            return refused( std::format( "AcquireVariant('{}'): the shader asset behind that name is gone.", ticket.Name ),
                            false );

        if ( !program || !program->IsCompiled() )
        {
            // A REFUSAL, NOT A RESULT — and it used to be handed back with a comment saying "the caller
            // decides whether to draw with the default instead". No caller decided. Both of them
            // (VolumetricCloudRenderer::BuildMediumPipelines and SkyCapture::AcquirePanoramaProgram)
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
            // nothing the header did not already say: the doc lists the refusals and the error names which
            // one this is, and since NN-RUNTIME-6 the reason travels in the Result, so the caller that marks
            // a path refused or falls back says WHY in its own log or report instead of a line nobody pairs.
            const std::string why = program ? program->GetCompileError() : built.GetError();
            return refused(
                 std::format( "Variant {} of '{}' has no compiled stages - the source did not compile or "
                              "reflect: {}",
                              ticket.Key, ticket.Name, why ),
                 true );
        }

        m_Refusals.erase( ticket.Key ); // it compiles now: a later refusal is a new one, and is reported
        m_Variants[ticket.Key] = VariantEntry{ ticket.Handle, program };
        return Common::MakeSuccess( VariantLookup{ .Ready = program } );
    }

    Common::ResultStr<std::shared_ptr<Graphic::Shader>> ShaderService::Refuse( const std::string& key,
                                                                               Refusal            refusal )
    {
        auto [it, inserted] = m_Refusals.try_emplace( key, refusal );
        if ( !inserted && it->second.Reason != refusal.Reason )
            it->second = std::move( refusal ); // a NEW reason: Reported starts false, so it is said once again
        return Common::MakeError<std::shared_ptr<Graphic::Shader>>( it->second.Reason );
    }

    bool ShaderService::ClaimRefusalReport( const std::string& name, const Graphic::ShaderVariant& variant )
    {
        const std::string key =
             std::format( "{}#{:016x}", name, static_cast<unsigned long long>( variant.Hash() ) );
        const std::unique_lock write( m_Lock );
        const auto             it = m_Refusals.find( key );
        if ( it == m_Refusals.end() )
            return true;
        return !std::exchange( it->second.Reported, true );
    }

    void ShaderService::ForgetRefusalsOf( const Assets::AssetHandle& handle )
    {
        const std::unique_lock write( m_Lock );
        ForgetRefusalsOfLocked( handle );
    }

    void ShaderService::ForgetRefusalsOfLocked( const Assets::AssetHandle& handle )
    {
        std::erase_if( m_Refusals, [&handle]( const auto& entry ) { return entry.second.Handle == handle; } );
        m_FileGenerations[handle] = ++m_RecompileGeneration;
    }

    void ShaderService::ForgetRefusalsOfProgram( const std::string& program )
    {
        std::erase_if( m_Refusals, [&program]( const auto& entry ) { return entry.second.Program == program; } );
        m_ProgramNameGenerations[program] = ++m_RecompileGeneration;
    }

    std::uint64_t ShaderService::ProgramGeneration( const std::string& name ) const
    {
        // A pass "<Shader>/<Pass>" is its file's program <Shader>.
        const std::string      program = name.substr( 0, name.find( '/' ) );
        const std::shared_lock read( m_Lock );
        std::uint64_t          generation = m_ClearGeneration;
        if ( const auto stamped = m_ProgramNameGenerations.find( program );
             stamped != m_ProgramNameGenerations.end() )
            generation = std::max( generation, stamped->second );
        if ( const auto handle = m_NameToHandleMap.find( program ); handle != m_NameToHandleMap.end() )
            if ( const auto file = m_FileGenerations.find( handle->second ); file != m_FileGenerations.end() )
                generation = std::max( generation, file->second );
        return generation;
    }

    std::vector<Core::CookedProgramVariant> ShaderService::ShippedByCookRequests() const
    {
        const std::shared_lock                  read( m_Lock );
        std::vector<Core::CookedProgramVariant> requests;
        requests.reserve( m_ShippedByCookRequests.size() );
        for ( const auto& [key, request] : m_ShippedByCookRequests )
            requests.push_back( request );
        return requests;
    }

    std::string ShaderService::MediumSourceOf( const Assets::AssetHandle& handle )
    {
        if ( handle.IsNull() )
            return {};

        {
            const std::shared_lock read( m_Lock );
            if ( const auto it = m_Media.find( handle ); it != m_Media.end() )
                return it->second.Source;
        }

        // A HANDLE THAT IS NOT A MEDIUM, and the two ways to get there are an unregistered shader and a
        // Medium slot pointed at an ordinary one. Both draw the shipped medium, which is a picture that
        // looks exactly like "I forgot to set it" — so it is said, once per handle, with the handle in
        // it. Once, because this is asked every frame.
        bool first = false;
        {
            const std::unique_lock write( m_Lock );
            // Registered since the shared look: a medium after all.
            if ( const auto it = m_Media.find( handle ); it != m_Media.end() )
                return it->second.Source;
            first = m_WarnedNotAMedium.insert( static_cast<uint64_t>( handle ) ).second;
        }
        if ( first )
        {
            LOG_ERROR( "[ShaderService] a cloud material's Medium slot names shader {}, which is not a "
                       "registered Volume medium (no `Medium {{ ... }}` block, or not loaded). The layer "
                       "draws the DEFAULT medium.",
                       static_cast<uint64_t>( handle ) );
        }
        return {};
    }

    std::string ShaderService::StampSourceOf( const std::string& name ) const
    {
        const std::shared_lock read( m_Lock );
        const auto             it = m_Stamps.find( name );
        return it != m_Stamps.end() ? it->second : std::string{};
    }

    std::vector<std::string> ShaderService::MediumSources() const
    {
        const std::shared_lock   read( m_Lock );
        std::vector<std::string> sources;
        sources.reserve( m_Media.size() );
        for ( const auto& [handle, medium] : m_Media )
            sources.push_back( medium.Source );
        return sources;
    }

    std::shared_ptr<const std::vector<Core::Formats::ShaderParam>>
    ShaderService::MediumSchemaOf( const Assets::AssetHandle& handle ) const
    {
        if ( handle.IsNull() )
            return nullptr;

        // NO WARNING HERE, and that is deliberate rather than an omission: MediumSourceOf is asked the same
        // question about the same handle in the same frame and latches the one message. Two refusals for
        // one mistake is how a log stops being read.
        const std::shared_lock read( m_Lock );
        const auto             it = m_Media.find( handle );
        if ( it == m_Media.end() )
            return nullptr;
        return it->second.Properties;
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
        //
        // A NEW schema object, never an overwrite of the old one: a render worker may hold that one
        // (MediumSchemaOf co-owns it), and it dies with its last holder.
        auto schema =
             std::make_shared<const std::vector<Core::Formats::ShaderParam>>( std::move( medium.Properties ) );
        const std::unique_lock write( m_Lock );
        m_Media[handle] = MediumEntry{ std::move( medium.Body ), std::move( schema ) };
        m_WarnedNotAMedium.erase( static_cast<uint64_t>( handle ) );
        return true;
    }

    int ShaderService::ReloadVariantsOf( const Assets::AssetHandle& handle )
    {
        // The live variants of @p handle by strong reference, the expired entries pruned - under the lock; the
        // recompiles (device objects rebuilt, a log line per failure) after it.
        std::vector<std::pair<std::string, std::shared_ptr<Graphic::Shader>>> live;
        {
            const std::unique_lock write( m_Lock );
            for ( auto it = m_Variants.begin(); it != m_Variants.end(); )
            {
                auto program = it->second.Handle == handle ? it->second.Program.lock() : nullptr;
                if ( it->second.Program.expired() )
                {
                    it = m_Variants.erase( it );
                    continue;
                }
                if ( program )
                    live.emplace_back( it->first, std::move( program ) );
                ++it;
            }
        }
        int reloaded = 0;
        for ( const auto& [key, program] : live )
        {
            const auto res = program->Reload();
            if ( !res )
            {
                LOG_ERROR( "[ShaderService] Variant {} failed to recompile: {}", key, res.GetError() );
            }
            else
            {
                ++reloaded;
            }
        }
        return reloaded;
    }

    Common::ResultStr<int> ShaderService::Recompile( const Assets::AssetHandle& handle )
    {
        ForgetRefusalsOf( handle );
        const auto shader = Get( handle );
        if ( !shader )
            return Common::MakeError<int>( std::format( "Recompile: shader {} is not a registered program",
                                                        static_cast<uint64_t>( handle ) ) );
        Graphic::PipelineBuilds::Get().WaitIdle();
        if ( const auto res = shader->Reload(); !res )
            return Common::MakeError<int>( res.GetError() );
        return Common::MakeSuccess( ReloadVariantsOf( handle ) );
    }

    void ShaderService::Clear()
    {
        // Was an empty body. Shaders own VkShaderModules and the pipeline layouts built from them: they leave
        // the maps under the lock and are released after it (declared before it, THREADS).
        decltype( m_Shaders )     shaders;
        decltype( m_PassShaders ) passShaders;
        {
            const std::unique_lock write( m_Lock );
            shaders.swap( m_Shaders );
            passShaders.swap( m_PassShaders );
            m_NameToHandleMap.clear();
            m_ShaderAssets.clear();
            m_Media.clear();
            m_Stamps.clear();
            m_WarnedNotAMedium.clear();
            m_Refusals.clear();
            m_FileGenerations.clear();
            m_ProgramNameGenerations.clear();
            m_ClearGeneration = ++m_RecompileGeneration;
            // Only the weak bookkeeping — a variant's modules belong to whoever still holds it, and freeing
            // them from here would leave that holder with a program made of destroyed modules.
            m_Variants.clear();
        }
    }

    std::vector<std::string> ShaderService::GetAllNames() const
    {
        const std::shared_lock   read( m_Lock );
        std::vector<std::string> names;
        names.reserve( m_NameToHandleMap.size() );
        for ( const auto& [name, handle] : m_NameToHandleMap )
            names.push_back( name );
        return names;
    }

} // namespace Desert::Runtime