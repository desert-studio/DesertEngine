#include "RayTracedTranslucency.hpp"

#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RayTracingPipeline.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Runtime/Services/Shader/ShaderLookup.hpp>

#include <Common/Settings/Scalability.hpp>

#include <format>
#include <utility>

namespace Desert::Graphic::System
{
    // THE ORDER CHECK (contract RayTracedTranslucency.hpp): the row's value names are indexed by the enum.
    static_assert( Common::Scalability::kTranslucencyRefractionMethodNames.size() == 2 );
    static_assert( Common::Scalability::kTranslucencyRefractionMethodNames[static_cast<size_t>(
                        TranslucencyRefractionMethod::ScreenSpace )] == std::string_view( "ScreenSpace" ) );
    static_assert( Common::Scalability::kTranslucencyRefractionMethodNames[static_cast<size_t>(
                        TranslucencyRefractionMethod::HardwareTraced )] == std::string_view( "HardwareTraced" ) );

    struct RayTracedTranslucencyRenderer::Impl
    {
        std::shared_ptr<Shader>             Program;
        ShaderBindingLayoutCache            Layout;
        std::shared_ptr<RayTracingPipeline> Pipeline;
        std::vector<std::string>            Templates;
        uint64_t                            VariantHash = 0;
    };

    RayTracedTranslucencyRenderer::RayTracedTranslucencyRenderer() : m_Impl( std::make_unique<Impl>() )
    {
    }

    RayTracedTranslucencyRenderer::~RayTracedTranslucencyRenderer() = default;

    Core::ShaderVariant RayTracedTranslucencyRenderer::Variant( const bool reorder )
    {
        Core::ShaderVariant variant = GlobalIllumination::TracerVariant( kRayTracedTranslucencyTracer );
        if ( reorder )
            variant.Defines.emplace_back( kRayTracingReorderDefine );
        return variant;
    }

    std::vector<Core::CookedProgramVariant> RayTracedTranslucencyRenderer::ProgramVariants()
    {
        std::vector<Core::CookedProgramVariant> variants;
        for ( const bool reorder : { false, true } )
            variants.push_back( { std::string( kRayTracedTranslucencyProgram ), Variant( reorder ) } );
        return variants;
    }

    Common::BoolResultStr
    RayTracedTranslucencyRenderer::EnsurePipeline( const std::vector<std::string>& templates )
    {
        const bool                reorder = RayTracingReorderOffered();
        const Core::ShaderVariant variant = Variant( reorder );
        const uint64_t            hash    = variant.Hash();
        Impl&                     self    = *m_Impl;
        if ( self.Pipeline && templates == self.Templates && self.VariantHash == hash )
            return BOOLSUCCESS;
        auto acquired = Runtime::AcquireShaderVariant( std::string( kRayTracedTranslucencyProgram ), variant );
        if ( !acquired )
            return Common::MakeFormattedError<bool>(
                 "Ray traced refraction: the ray generation ('{}'{}) was refused: {}",
                 kRayTracedTranslucencyProgram, reorder ? " with DESERT_SER=1, Shader Execution Reordering" : "",
                 acquired.GetError() );
        auto                            program = acquired.ExtractValue();
        RayTracingPipelineSpecification spec{
             .Program = program, .HitGroups = {}, .DebugName = "RayTracedTranslucency" };
        for ( const std::string& name : templates )
        {
            const std::string cell = std::format( "{}/RayTracing.Hit", name );
            auto              hit  = Runtime::FindShader( cell );
            if ( !hit || !hit->IsCompiled() )
                return Common::MakeFormattedError<bool>( "Ray traced refraction: the hit group table names "
                                                         "surface template '{}', whose hit group '{}' "
                                                         "is not compiled",
                                                         name, cell );
            spec.HitGroups.push_back( std::move( hit ) );
        }
        auto pipeline = RayTracingPipeline::Create( spec );
        if ( !pipeline )
            return Common::MakeError( pipeline.GetError() );
        self.Program = std::move( program );
        // Published whole; the replaced one outlives the frames that may still trace it (a pass of an earlier
        // frame traces the one it captured in AddPass).
        std::shared_ptr<RayTracingPipeline> outgoing = std::exchange( self.Pipeline, pipeline.GetValue() );
        RayTracingPipeline::Retire( std::move( outgoing ) );
        self.Templates   = templates;
        self.VariantHash = hash;
        return BOOLSUCCESS;
    }

    Common::ResultStr<RefractionOutputs>
    RayTracedTranslucencyRenderer::AddPass( RDG::Builder& graph, const RayTracedTranslucencyInputs& in,
                                            const ShaderPush::RayTracedTranslucencyPush& push )
    {
        Impl& self = *m_Impl;
        if ( !self.Pipeline || !self.Program )
            return Common::MakeError<RefractionOutputs>(
                 "Ray traced refraction: EnsurePipeline did not succeed for this frame's hit group table" );
        if ( in.HitLighting.HitGroups == nullptr )
            return Common::MakeError<RefractionOutputs>( "Ray traced refraction: no hit group table this frame" );
        if ( in.HitLighting.HitGroups->Templates != self.Templates )
            return Common::MakeError<RefractionOutputs>( "Ray traced refraction: the hit group table's templates "
                                                         "are not the ones the pipeline was built for" );
        if ( !in.HitLighting.SceneReads.IsValid() )
            return Common::MakeError<RefractionOutputs>( "Ray traced refraction: the hit groups' scene reads "
                                                         "(world parameters, rain occlusion) are missing" );
        if ( !in.FrontLayer.Normal.IsValid() || !in.FrontLayer.Depth.IsValid() )
            return Common::MakeError<RefractionOutputs>(
                 "Ray traced refraction: the translucent front layer G-buffer (normal, depth) was not rendered" );
        if ( !in.Camera.IsValid() )
            return Common::MakeError<RefractionOutputs>( "Ray traced refraction: no CameraUB" );
        if ( !in.OpaqueDepth.IsValid() )
            return Common::MakeError<RefractionOutputs>( "Ray traced refraction: no opaque G-buffer depth" );
        if ( in.Width == 0 || in.Height == 0 )
            return Common::MakeError<RefractionOutputs>( "Ray traced refraction: the render resolution is empty" );
        // The permutation statement, not the record's fields (GITracing.hpp: a consumer hands the frame on).
        if ( GlobalIllumination::TracerVariant( in.HitLighting.Tracer.Pipeline ).Hash() !=
             GlobalIllumination::TracerVariant( kRayTracedTranslucencyTracer ).Hash() )
            return Common::MakeError<RefractionOutputs>(
                 "Ray traced refraction: the hit lighting reads were made for another tracer than "
                 "kRayTracedTranslucencyTracer (hardware, hits lit in place)" );
        if ( const std::string_view missing = GlobalIllumination::TracerFrameMissing( in.HitLighting.Tracer );
             !missing.empty() )
            return Common::MakeFormattedError<RefractionOutputs>( "Ray traced refraction: {}", missing );

        const RDG::TextureRef radiance =
             graph.CreateTexture( RDG::TextureDesc{ .Size   = { .Width = in.Width, .Height = in.Height },
                                                    .Format = ViewTargetFormats::kSceneColor },
                                  "Translucency.RayTracedRefraction" );

        graph.AddPass(
             kRayTracedTranslucencyPass, RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 auto block = pass.Bindings( self.Layout.Get( self.Program ), RDG::NoOtherRouteFill() );
                 block.Storage( RayTracedTranslucencyShaderParameters::RefractionOutputTexture, radiance,
                                RDG::Access::StorageWrite )
                      .Uniform( RayTracedTranslucencyShaderParameters::CameraUB, in.Camera )
                      .Texture( RayTracedTranslucencyShaderParameters::GBufferDepthTexture, in.OpaqueDepth,
                                RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                      .PushConstants<ShaderPush::RayTracedTranslucencyPush>();
                 RayTracing::BindRayTracingSceneReads<RayTracedTranslucencyShaderParameters>(
                      block, in.HitLighting.SceneReads );
                 SSRRenderer::DeclareFrontLayerReads( block, &in.FrontLayer );
                 SSRRenderer::DeclareTracedReads( block, in.HitLighting );
             },
             [pipeline = self.Pipeline, hits = in.HitLighting.HitGroups, push, width = in.Width,
              height = in.Height]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 DESERT_PROFILE_PASS_DYNAMIC( kRayTracedTranslucencyPass.data() ); // a literal: NUL-terminated
                 RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 bindings.PushConstants( push );
                 return Renderer::TraceRays( bindings, *pipeline, *hits, width, height );
             } );
        return Common::MakeSuccess( RefractionOutputs{ .Radiance = radiance } );
    }
} // namespace Desert::Graphic::System
