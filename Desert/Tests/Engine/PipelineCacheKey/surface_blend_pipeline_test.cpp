// VFX-08e. ApplySurfaceBlendMode is THE one place a surface template's BlendMode becomes pipeline blend state -
// the translucent meshes (MeshRendererForward TranslucentDrawFor) and the particle sprites (ParticleRenderer
// SpriteDrawFor) both build through it. Device-free: it only writes a GraphicsPipelineSpecification.
#include <Engine/Graphic/Materials/SurfaceBlendPipeline.hpp>

#include <gtest/gtest.h>

using namespace Desert::Graphic;
using Desert::Core::Formats::SurfaceBlendMode;

namespace
{
    GraphicsPipelineSpecification Applied( const SurfaceBlendMode blend )
    {
        GraphicsPipelineSpecification spec;
        spec.DepthWriteEnabled = true;
        ApplySurfaceBlendMode( spec, blend );
        return spec;
    }
} // namespace

TEST( SurfaceBlendPipeline, AdditiveAddsOpacityWeightedColourAndWritesNoDepth )
{
    const GraphicsPipelineSpecification spec = Applied( SurfaceBlendMode::Additive );
    EXPECT_TRUE( spec.BlendEnable );
    // The pass headers write (colour, opacity) unpremultiplied: SrcAlpha / One is UE's One / One over
    // premultiplied.
    EXPECT_EQ( spec.SrcColorBlendFactor, BlendFactor::SrcAlpha );
    EXPECT_EQ( spec.DstColorBlendFactor, BlendFactor::One );
    EXPECT_FALSE( spec.DepthWriteEnabled );
}

TEST( SurfaceBlendPipeline, TranslucentBlendsOverAndWritesNoDepth )
{
    const GraphicsPipelineSpecification spec = Applied( SurfaceBlendMode::Translucent );
    EXPECT_TRUE( spec.BlendEnable );
    EXPECT_EQ( spec.SrcColorBlendFactor, BlendFactor::SrcAlpha );
    EXPECT_EQ( spec.DstColorBlendFactor, BlendFactor::OneMinusSrcAlpha );
    EXPECT_FALSE( spec.DepthWriteEnabled );
}

TEST( SurfaceBlendPipeline, OpaqueAndMaskedDoNotBlendAndKeepTheirDepthWrite )
{
    for ( const SurfaceBlendMode blend : { SurfaceBlendMode::Opaque, SurfaceBlendMode::Masked } )
    {
        const GraphicsPipelineSpecification spec = Applied( blend );
        EXPECT_FALSE( spec.BlendEnable );
        EXPECT_TRUE( spec.DepthWriteEnabled );
    }
}
