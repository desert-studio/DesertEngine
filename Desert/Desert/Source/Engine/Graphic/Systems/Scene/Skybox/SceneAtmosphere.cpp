#include "SceneAtmosphere.hpp"

#include <Common/Core/Logger.hpp>
#include <Common/Core/Profiler.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RenderConfig.hpp> // GlobalTextureFilterSampler
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <utility>

namespace Desert::Graphic
{
    namespace
    {
        constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;

        std::shared_ptr<ComputePipeline> MakeLutProgram( const char* name )
        {
            const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( name );
            if ( !shader )
            {
                LOG_ERROR( "[SkyAtmosphere] Compute shader '{}' is not registered -- expected "
                           "Editor/Resources/Shaders/Programs/Sky/{}.shader. The physical atmosphere's LUTs will "
                           "not be built for this scene.",
                           name, name );
                return nullptr;
            }
            // Create() hands back a BUILT pipeline or the reason it refused.
            const auto built = ComputePipeline::Create( { .Shader = shader, .DebugName = name } );
            if ( !built )
            {
                LOG_ERROR( "[SkyAtmosphere] {} The physical atmosphere's LUTs will not be built for this scene.",
                           built.GetError() );
                return nullptr;
            }
            return built.GetValue();
        }
    } // namespace

    uint64_t SceneAtmosphere::MixBytes( uint64_t hash, const void* data, const size_t size )
    {
        const auto* bytes = static_cast<const unsigned char*>( data );
        for ( size_t i = 0; i < size; ++i )
        {
            hash ^= static_cast<uint64_t>( bytes[i] );
            hash *= 1099511628211ull;
        }
        return hash;
    }

    uint64_t SceneAtmosphere::SkyBlockLutInputsOf( const SkyGpuPayload& payload )
    {
        // The view's luminance ceiling is the one block value no LUT reads (the on-screen sky pass clamps by it).
        SkyGpuPayload fillInput     = payload;
        fillInput.AtmosphereLight.w = 0.0f;
        return MixBytes( kFnvOffsetBasis, &fillInput, sizeof( fillInput ) );
    }

    uint64_t SceneAtmosphere::SkyLutFillInputsOf( const uint64_t blockInputs, const uint64_t pairGeneration,
                                                  const glm::vec4& push )
    {
        uint64_t hash = MixBytes( kFnvOffsetBasis, &blockInputs, sizeof( blockInputs ) );
        hash          = MixBytes( hash, &pairGeneration, sizeof( pairGeneration ) );
        return MixBytes( hash, &push, sizeof( push ) );
    }

    SceneAtmosphere::AtmosphereLutFingerprint SceneAtmosphere::LutFingerprintOf( const SkySettings& sky )
    {
        return AtmosphereLutFingerprint{ .RayleighScattering        = sky.RayleighScattering,
                                         .RayleighExpDistributionKm = sky.RayleighExpDistributionKm,
                                         .MieScattering             = sky.MieScattering,
                                         .MieAbsorption             = sky.MieAbsorption,
                                         .MieExpDistributionKm      = sky.MieExpDistributionKm,
                                         .OzoneAbsorption           = sky.OzoneAbsorption,
                                         .OzoneTipAltitudeKm        = sky.OzoneTipAltitudeKm,
                                         .OzoneTipValue             = sky.OzoneTipValue,
                                         .OzoneTentWidthKm          = sky.OzoneTentWidthKm,
                                         .GroundAlbedo              = sky.GroundAlbedo,
                                         .AtmosphereHeightKm        = sky.AtmosphereHeightKm,
                                         .MultiScatteringFactor     = sky.MultiScatteringFactor,
                                         .PlanetRadius              = sky.PlanetRadius };
    }

    bool SceneAtmosphere::EnsurePrograms()
    {
        if ( m_ProgramsMade )
            return true;
        if ( m_ProgramsFailed )
            return false;

        // The block before the programs: without it no node can bind the sky.
        m_SkyBlock = ShaderResources::StorageBuffer::Create( "SkyAtmosphereBlock", kSkyPayloadBytes,
                                                             kSkyPayloadBinding, /*persistent=*/false );
        if ( !m_SkyBlock )
        {
            LOG_ERROR(
                 "[SkyAtmosphere] the scene's atmosphere block could not be created; this scene's procedural "
                 "sky gives no atmosphere LUTs and no sky light." );
            m_ProgramsFailed = true;
            return false;
        }
        m_TransmittanceLutPipeline  = MakeLutProgram( "SkyTransmittanceLut" );
        m_MultiScatterLutPipeline   = MakeLutProgram( "SkyMultiScatterLut" );
        m_SkyViewLutPipeline        = MakeLutProgram( "SkyViewLut" );
        m_AerialPerspectivePipeline = MakeLutProgram( "SkyAerialPerspectiveLut" );
        m_DistantLightPipeline      = MakeLutProgram( "SkyDistantLight" );
        // Each missing program was said above; the physical model asks CanBakeLuts, the gradient needs none.
        m_ProgramsMade = true;
        return true;
    }

    bool SceneAtmosphere::ClaimFrame( const void* view )
    {
        const uint64_t frame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        if ( frame == m_ClaimFrame )
            return m_Claimant == view;
        m_ClaimFrame = frame;
        m_Claimant   = view;
        m_UploadedBlockInputs.reset();
        return true;
    }

    bool SceneAtmosphere::Claims( const void* view ) const
    {
        return m_Claimant == view && m_ClaimFrame == Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
    }

    void SceneAtmosphere::UploadSkyBlock( const void* view, const SkyGpuPayload& payload )
    {
        // EVERY VIEW WRITES ITS OWN COPY (the block is per renderer slot): the view that records the capture need
        // not be the one that claimed the LUTs, and each binds the copy in its own slot. Same bytes for all.
        if ( !m_SkyBlock )
            return;
        const bool claims = Claims( view );
        // The capture's own view: no exposure. The ceiling is the view's (SkyBakeFingerprint), and neither the
        // LUTs nor the capture read it.
        SkyGpuPayload block     = payload;
        block.AtmosphereLight.w = 0.0f;
        if ( claims )
            m_UploadedBlockInputs.reset();
        if ( const auto uploaded = m_SkyBlock->SetData( &block, kSkyPayloadBytes ); !uploaded.IsSuccess() )
        {
            LOG_ERROR(
                 "[SkyAtmosphere] the scene's atmosphere block was not uploaded; its LUTs and sky light keep "
                 "the previous sky until the next frame: {}",
                 uploaded.GetError() );
            return;
        }
        // What the claiming view's LUT nodes read (the distant light's fill inputs).
        if ( claims )
            m_UploadedBlockInputs = SkyBlockLutInputsOf( block );
    }

    bool SceneAtmosphere::EnsureLutResources()
    {
        if ( m_LutResourcesFailed )
            return false;
        if ( m_TransmittanceLut && m_MultiScatterLut )
            return true;

        m_TransmittanceLut = Image2D::Create( Core::Formats::Image2DSpecification{
             .Tag        = "SkyTransmittanceLut",
             .Width      = kTransmittanceLutWidth,
             .Height     = kTransmittanceLutHeight,
             .Format     = Core::Formats::ImageFormat::RGBA16F,
             .Mips       = 1u,
             .Usage      = Core::Formats::Image2DUsage::Image2D,
             .Properties = Core::Formats::Storage | Core::Formats::Sample,
        } );
        m_MultiScatterLut  = Image2D::Create( Core::Formats::Image2DSpecification{
              .Tag        = "SkyMultiScatterLut",
              .Width      = kMultiScatterLutSize,
              .Height     = kMultiScatterLutSize,
              .Format     = Core::Formats::ImageFormat::RGBA16F,
              .Mips       = 1u,
              .Usage      = Core::Formats::Image2DUsage::Image2D,
              .Properties = Core::Formats::Storage | Core::Formats::Sample,
        } );
        if ( !m_TransmittanceLut || !m_MultiScatterLut )
        {
            LOG_ERROR( "[SkyAtmosphere] The atmosphere LUTs could not be created (transmittance {}x{}: {}, "
                       "multi-scattering {}x{}: {}); the physical atmosphere will not be built for this scene.",
                       kTransmittanceLutWidth, kTransmittanceLutHeight, m_TransmittanceLut ? "ok" : "FAILED",
                       kMultiScatterLutSize, kMultiScatterLutSize, m_MultiScatterLut ? "ok" : "FAILED" );
            m_TransmittanceLut.reset();
            m_MultiScatterLut.reset();
            m_LutResourcesFailed = true;
            return false;
        }
        LOG_INFO(
             "[SkyAtmosphere] Transmittance LUT {}x{} + multi-scattering LUT {}x{} RGBA16F ({:.2f} MiB) "
             "allocated for the scene's physical atmosphere.",
             kTransmittanceLutWidth, kTransmittanceLutHeight, kMultiScatterLutSize, kMultiScatterLutSize,
             LutBytesToMiB( Core::Formats::CalculateImageSize( kTransmittanceLutWidth, kTransmittanceLutHeight,
                                                               Core::Formats::ImageFormat::RGBA16F ) +
                            Core::Formats::CalculateImageSize( kMultiScatterLutSize, kMultiScatterLutSize,
                                                               Core::Formats::ImageFormat::RGBA16F ) ) );
        return true;
    }

    bool SceneAtmosphere::EnsureDistantLightResources()
    {
        if ( m_DistantLightResourcesFailed )
            return false;
        if ( m_DistantLight )
            return true;

        // RGBA32F rather than the RGBA16F every other LUT uses: at 16 bytes the exact format is free, and this
        // value is added to a fog colour at night radiances where a half would quantise into visible steps.
        m_DistantLight = Image2D::Create( Core::Formats::Image2DSpecification{
             .Tag        = "SkyDistantLight",
             .Width      = kDistantLightWidth,
             .Height     = 1,
             .Format     = Core::Formats::ImageFormat::RGBA32F,
             .Mips       = 1u,
             .Usage      = Core::Formats::Image2DUsage::Image2D,
             .Properties = Core::Formats::Storage | Core::Formats::Sample,
        } );
        if ( !m_DistantLight )
        {
            LOG_ERROR( "[SkyAtmosphere] The distant sky light ({}x1 RGBA32F) could not be created; the "
                       "atmospheric fog of this scene falls back to its authored colour with no sky ambient.",
                       kDistantLightWidth );
            m_DistantLightResourcesFailed = true;
            return false;
        }
        LOG_INFO(
             "[SkyAtmosphere] Distant sky light {}x1 RGBA32F allocated for the scene -- {} directions marched "
             "at {} km, reduced to the full-sphere mean the fog reads.",
             kDistantLightWidth, 64, 6 );
        return true;
    }

    RenderPassDeclaration::BlockDeclaration SceneAtmosphere::DeclareLutBlock( RenderPassDeclaration&    declared,
                                                                              ComputePipeline*          pipeline,
                                                                              ShaderBindingLayoutCache& layout )
    {
        // The pipeline route carries only the atmosphere's block, set here in setup every frame; every LUT is a
        // block entry naming this scene's own image (imported by the graph, which orders and barriers it).
        pipeline->SetStorageBuffer( kSkyPayloadBinding, m_SkyBlock.get() );
        auto block = declared.Bindings( layout.Get( pipeline->GetSpecification().Shader ),
                                        Renderer::GetPipelineRouteFill( *pipeline ) );
        block.PushConstantBytes( 0 );
        return block;
    }

    Common::BoolResultStr SceneAtmosphere::DispatchTransmittanceLut( const RDG::PassContext& context )
    {
        // Transmittance strictly first: the multi-scattering march samples it per step (the graph orders the
        // two nodes by their declared write and read).
        const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        return Renderer::GetInstance().DispatchCompute( bindings, *m_TransmittanceLutPipeline,
                                                        LutGroupCount( kTransmittanceLutWidth ),
                                                        LutGroupCount( kTransmittanceLutHeight ), 1 );
    }

    Common::BoolResultStr SceneAtmosphere::DispatchMultiScatterLut( const RDG::PassContext& context )
    {
        const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        return Renderer::GetInstance().DispatchCompute( bindings, *m_MultiScatterLutPipeline,
                                                        LutGroupCount( kMultiScatterLutSize ),
                                                        LutGroupCount( kMultiScatterLutSize ), 1 );
    }

    Common::BoolResultStr SceneAtmosphere::DispatchDistantLight( const RDG::PassContext& context )
    {
        // ONE WORKGROUP, and it must stay one: the 64 directions are reduced in groupshared memory, which no
        // second group can see. The shader's LocalSize is 64 for the same reason.
        const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        return Renderer::GetInstance().DispatchCompute( bindings, *m_DistantLightPipeline, 1, 1, 1 );
    }

    ComputeNodeList SceneAtmosphere::DeclareCachedLutNodes( const void* view, Common::Memory::FrameArena& arena,
                                                            const SkySettings& sky )
    {
        // One Compute node per dispatch, each declaring the LUT it writes (StorageWrite) and the LUTs it samples
        // (SampledCompute): the graph orders transmittance -> multi-scattering -> distant light and places every
        // barrier. Only the claiming view records them; the others read what its graph left.
        ComputeNodeList nodes = MakeComputeNodeList( arena );
        if ( !Claims( view ) )
            return nodes;
        m_LutBakePending.reset();
        m_DistantLightInputsPending.reset();
        m_DistantLightPending = false;
        if ( !m_SkyBlock || !CanBakeLuts() || !EnsureLutResources() )
            return nodes;

        const auto sampledLuts = [this]( RenderPassDeclaration::BlockDeclaration block )
        {
            block.Texture( SkyboxLutReadShaderParameters::TransmittanceLutTexture, m_TransmittanceLut,
                           RDG::Access::SampledCompute, "Sky.TransmittanceLut" )
                 .Sampler( SkyboxLutReadShaderParameters::GlobalFilterSampler, GlobalTextureFilterSampler() )
                 .Texture( SkyboxLutReadShaderParameters::MultiScatterLutTexture, m_MultiScatterLut,
                           RDG::Access::SampledCompute, "Sky.MultiScatterLut" );
            return block;
        };

        // The cached pair only when the atmosphere's parameters changed.
        const AtmosphereLutFingerprint wanted = LutFingerprintOf( sky );
        if ( !m_LutsValid || wanted != m_LutBaked )
        {
            ComputeNodeDeclaration transmittance( arena );
            transmittance.Name = "Sky: TransmittanceLut";
            DeclareLutBlock( transmittance.Access, m_TransmittanceLutPipeline.get(), m_TransmittanceLutLayout )
                 .Storage( SkyboxLutWriteShaderParameters::TransmittanceLutTexture, m_TransmittanceLut,
                           RDG::Access::StorageWrite, "Sky.TransmittanceLut" );
            transmittance.Record = [this]( RDG::PassContext& context,
                                           const FrameGraphRefs& ) -> Common::BoolResultStr
            {
                DESERT_PROFILE_PASS( "Sky: AtmosphereLuts" );
                return DispatchTransmittanceLut( context );
            };
            nodes.push_back( std::move( transmittance ) );

            ComputeNodeDeclaration multiScatter( arena );
            multiScatter.Name = "Sky: MultiScatterLut";
            DeclareLutBlock( multiScatter.Access, m_MultiScatterLutPipeline.get(), m_MultiScatterLutLayout )
                 .Texture( SkyboxLutReadShaderParameters::TransmittanceLutTexture, m_TransmittanceLut,
                           RDG::Access::SampledCompute, "Sky.TransmittanceLut" )
                 .Sampler( SkyboxLutReadShaderParameters::GlobalFilterSampler, GlobalTextureFilterSampler() )
                 .Storage( SkyboxLutWriteShaderParameters::MultiScatterLutTexture, m_MultiScatterLut,
                           RDG::Access::StorageWrite, "Sky.MultiScatterLut" );
            multiScatter.Record = [this]( RDG::PassContext& context,
                                          const FrameGraphRefs& ) -> Common::BoolResultStr
            { return DispatchMultiScatterLut( context ); };
            nodes.push_back( std::move( multiScatter ) );

            // BUILD TIME: the pair counts as baked once SettleCachedLutNodes hears the graph accepted them.
            m_LutBakePending = wanted;
        }

        if ( !m_DistantLightPipeline || !EnsureDistantLightResources() )
            return nodes;
        // RE-MARCHED ONLY WHEN WHAT IT READS MOVED (CLOUDS-MARCH-LATENCY): the block and the pair, no push. A pair
        // baked by this very graph is one generation ahead once it settles, so its dependant re-fills now.
        uint64_t pairGeneration = m_LutBakeGeneration;
        if ( m_LutBakePending )
            ++pairGeneration;
        std::optional<uint64_t> wantedInputs;
        if ( m_UploadedBlockInputs )
            wantedInputs = SkyLutFillInputsOf( *m_UploadedBlockInputs, pairGeneration, glm::vec4( 0.0f ) );
        if ( wantedInputs && m_DistantLightWritten && m_DistantLightInputs == wantedInputs )
            return nodes;
        m_DistantLightInputsPending = wantedInputs;

        ComputeNodeDeclaration distant( arena );
        distant.Name = "Sky: DistantSkyLight";
        sampledLuts( DeclareLutBlock( distant.Access, m_DistantLightPipeline.get(), m_DistantLightLayout ) )
             .Storage( SkyboxLutReadShaderParameters::DistantSkyLightTexture, m_DistantLight,
                       RDG::Access::StorageWrite, "Sky.DistantLight" );
        distant.Record = [this]( RDG::PassContext& context, const FrameGraphRefs& ) -> Common::BoolResultStr
        {
            DESERT_PROFILE_PASS( "Sky: DistantSkyLight" );
            return DispatchDistantLight( context );
        };
        nodes.push_back( std::move( distant ) );
        m_DistantLightPending = true;
        return nodes;
    }

    void SceneAtmosphere::SettleCachedLutNodes( const void* view, const bool accepted )
    {
        if ( !Claims( view ) )
            return;
        const std::optional<AtmosphereLutFingerprint> pending = std::exchange( m_LutBakePending, std::nullopt );
        // The distant light's node, when this graph declared one: accepted, the image now holds what it marched
        // from (an empty pending -- a refused block upload -- leaves it unknown, so the next frame re-fills);
        // refused, it wrote nothing and keeps the earlier frame's content and inputs.
        const std::optional<uint64_t> distantPending = std::exchange( m_DistantLightInputsPending, std::nullopt );
        if ( accepted && m_DistantLightPending )
        {
            m_DistantLightInputs  = distantPending;
            m_DistantLightWritten = true;
        }
        m_DistantLightPending = false;
        if ( !accepted )
        {
            // Nothing was recorded: a pair that was about to be baked is not baked, so the next frame bakes it.
            if ( pending )
                m_LutsValid = false;
            return;
        }
        if ( !pending )
            return;
        m_LutBaked  = *pending;
        m_LutsValid = true;
        ++m_LutBakeGeneration;
        LOG_INFO( "[SkyAtmosphere] Atmosphere LUTs dispatched once for the scene (transmittance {}x{}, "
                  "multi-scattering {}x{}) -- the atmosphere parameter fingerprint changed.",
                  kTransmittanceLutWidth, kTransmittanceLutHeight, kMultiScatterLutSize, kMultiScatterLutSize );
    }
} // namespace Desert::Graphic
