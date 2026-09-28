// The relation this suite pins: two GraphicsPipelineSpecifications share a cached pipeline if and only if
// they would produce an equivalent GPU object.
//
// It exists because the key silently fell behind the specification. `UseLoadRenderPass` selects a DIFFERENT
// VkRenderPass (VulkanPipeline.cpp:387) and was not in the key at all, so `MeshRenderer::DrawGenericMeshes`
// — which sets that field from a variable, false on the forward path and true on the deferred one — produced
// a byte-identical key for two pipelines that must differ. Whichever built first served both. The vertex
// layout, the back stencil face and both stencil masks were missing the same way.
//
// So the tests below are deliberately EXHAUSTIVE over the specification rather than over the bug: one
// assertion per field, including the fields that were already in the key. A single-field test would have
// passed on the broken code for every field except the four. What catches the NEXT field to be added is the
// census, and `EveryFieldOfTheSpecificationIsAccountedFor` states the count out loud so adding one to the
// struct without deciding about the key fails here.

#include <gtest/gtest.h>

#include <Engine/Graphic/PipelineCache.hpp>

using namespace Desert::Graphic;

namespace
{
    // Distinct non-null pointers without constructing the abstract types. Only pointer identity reaches the
    // key, so a no-op deleter over a dummy address is exactly as good as a real object and needs no device.
    template <typename T>
    std::shared_ptr<T> FakeHandle( int& storage )
    {
        return std::shared_ptr<T>( reinterpret_cast<T*>( &storage ), []( T* ) {} );
    }

    int g_shaderA, g_shaderB, g_fbA, g_fbB, g_rpA, g_rpB;

    // A fully populated baseline: stencil and blending are ENABLED so that the fields guarded by those flags
    // are actually reachable. A baseline with them off would make five of the assertions below vacuous.
    GraphicsPipelineSpecification Baseline()
    {
        GraphicsPipelineSpecification s;
        s.Shader      = FakeHandle<Shader>( g_shaderA );
        s.Framebuffer = FakeHandle<Framebuffer>( g_fbA );
        s.Renderpass  = FakeHandle<RenderPass>( g_rpA );
        s.Layout      = VertexBufferLayout{ { ShaderDataType::Float3, "a_Position" },
                                            { ShaderDataType::Float2, "a_TextureCoord" } };

        s.DepthTestEnabled   = true;
        s.DepthWriteEnabled  = true;
        s.DepthCompareOp     = DepthCompare::Closer;
        s.CullMode           = CullMode::Back;
        s.Topology           = PrimitiveTopology::Triangles;
        s.PolygonMode        = PrimitivePolygonMode::Solid;
        s.PatchControlPoints = 0;

        s.BlendEnable         = true;
        s.SrcColorBlendFactor = BlendFactor::SrcAlpha;
        s.DstColorBlendFactor = BlendFactor::OneMinusSrcAlpha;

        s.StencilTestEnabled     = true;
        s.StencilFront.CompareOp = CompareOp::Equal;
        s.StencilFront.FailOp    = StencilOp::Keep;
        s.StencilFront.PassOp    = StencilOp::Replace;
        s.StencilFront.Reference = 1;
        s.StencilBack            = s.StencilFront;

        s.UseLoadRenderPass = false;
        s.DebugName         = "Baseline";
        return s;
    }

    // The relation, named once. `SharesPipeline` is symmetric by construction; asserting both directions
    // would test std::equal_to, not the key.
    void MustDiffer( const GraphicsPipelineSpecification& b, const char* what )
    {
        EXPECT_FALSE( PipelineCache::SharesPipeline( Baseline(), b ) )
             << what << " does not reach the key, so two different pipelines would share one cache entry";
    }

    void MustShare( const GraphicsPipelineSpecification& b, const char* what )
    {
        EXPECT_TRUE( PipelineCache::SharesPipeline( Baseline(), b ) )
             << what << " is over-hashed: it forks a cache entry without changing the pipeline";
    }
} // namespace

// ---------------------------------------------------------------------------------------------------------
// Fields the backend reads. Differing in any one of them must NOT share a pipeline.
// ---------------------------------------------------------------------------------------------------------

TEST( PipelineCacheKey, ShaderSeparates )
{
    auto s   = Baseline();
    s.Shader = FakeHandle<Shader>( g_shaderB );
    MustDiffer( s, "Shader" );
}

TEST( PipelineCacheKey, FramebufferSeparates )
{
    auto s        = Baseline();
    s.Framebuffer = FakeHandle<Framebuffer>( g_fbB );
    MustDiffer( s, "Framebuffer" );
}

TEST( PipelineCacheKey, TargetLayoutSeparates )
{
    // Each change on its own: presence, a colour format, the colour count, the depth format, the samples.
    const RenderTargetLayout target{ { Desert::Core::Formats::ImageFormat::RGBA16F }, std::nullopt, 1 };
    auto                     s = Baseline();
    s.TargetLayout             = target;
    MustDiffer( s, "TargetLayout presence" );

    const auto differsFrom = []( const RenderTargetLayout& a, const RenderTargetLayout& b, const char* what )
    {
        auto x         = Baseline();
        auto y         = Baseline();
        x.TargetLayout = a;
        y.TargetLayout = b;
        EXPECT_FALSE( PipelineCache::SharesPipeline( x, y ) )
             << what << " does not reach the key, so two different pipelines would share one cache entry";
    };
    RenderTargetLayout other = target;
    other.ColorFormats[0]    = Desert::Core::Formats::ImageFormat::RGBA8F;
    differsFrom( target, other, "TargetLayout colour format" );
    other = target;
    other.ColorFormats.push_back( Desert::Core::Formats::ImageFormat::RGBA16F );
    differsFrom( target, other, "TargetLayout colour count" );
    other             = target;
    other.DepthFormat = Desert::Core::Formats::ImageFormat::DEPTH32F;
    differsFrom( target, other, "TargetLayout depth format" );
    other         = target;
    other.Samples = 4;
    differsFrom( target, other, "TargetLayout samples" );
}

TEST( PipelineCacheKey, RenderpassSeparates )
{
    auto s       = Baseline();
    s.Renderpass = FakeHandle<RenderPass>( g_rpB );
    MustDiffer( s, "Renderpass" );
}

TEST( PipelineCacheKey, DepthTestSeparates )
{
    auto s             = Baseline();
    s.DepthTestEnabled = false;
    MustDiffer( s, "DepthTestEnabled" );
}

TEST( PipelineCacheKey, DepthWriteSeparates )
{
    auto s              = Baseline();
    s.DepthWriteEnabled = false;
    MustDiffer( s, "DepthWriteEnabled" );
}

TEST( PipelineCacheKey, DepthCompareSeparates )
{
    auto s           = Baseline();
    s.DepthCompareOp = CompareOp::Less;
    MustDiffer( s, "DepthCompareOp" );
}

TEST( PipelineCacheKey, CullModeSeparates )
{
    auto s     = Baseline();
    s.CullMode = CullMode::Front;
    MustDiffer( s, "CullMode" );
}

TEST( PipelineCacheKey, TopologySeparates )
{
    auto s     = Baseline();
    s.Topology = PrimitiveTopology::Lines;
    MustDiffer( s, "Topology" );
}

TEST( PipelineCacheKey, PolygonModeSeparates )
{
    auto s        = Baseline();
    s.PolygonMode = PrimitivePolygonMode::Wireframe;
    MustDiffer( s, "PolygonMode" );
}

TEST( PipelineCacheKey, PatchControlPointsSeparates )
{
    auto s               = Baseline();
    s.PatchControlPoints = 4;
    MustDiffer( s, "PatchControlPoints" );
}

TEST( PipelineCacheKey, BlendEnableSeparates )
{
    auto s        = Baseline();
    s.BlendEnable = false;
    MustDiffer( s, "BlendEnable" );
}

TEST( PipelineCacheKey, BlendFactorsSeparate )
{
    auto src                = Baseline();
    src.SrcColorBlendFactor = BlendFactor::One;
    MustDiffer( src, "SrcColorBlendFactor" );

    auto dst                = Baseline();
    dst.DstColorBlendFactor = BlendFactor::Zero;
    MustDiffer( dst, "DstColorBlendFactor" );
}

TEST( PipelineCacheKey, StencilTestSeparates )
{
    auto s               = Baseline();
    s.StencilTestEnabled = false;
    MustDiffer( s, "StencilTestEnabled" );
}

// Every member of StencilOpState is baked into the pipeline (VulkanPipeline.cpp:455-464), and the old key
// packed four of the seven, for the front face only.
TEST( PipelineCacheKey, EveryStencilFrontMemberSeparates )
{
    {
        auto s                   = Baseline();
        s.StencilFront.CompareOp = CompareOp::Never;
        MustDiffer( s, "StencilFront.CompareOp" );
    }
    {
        auto s                = Baseline();
        s.StencilFront.FailOp = StencilOp::Zero;
        MustDiffer( s, "StencilFront.FailOp" );
    }
    {
        auto s                = Baseline();
        s.StencilFront.PassOp = StencilOp::Invert;
        MustDiffer( s, "StencilFront.PassOp" );
    }
    {
        auto s                     = Baseline();
        s.StencilFront.DepthFailOp = StencilOp::IncrementAndWrap;
        MustDiffer( s, "StencilFront.DepthFailOp" );
    }
    {
        auto s                     = Baseline();
        s.StencilFront.CompareMask = 0x0F;
        MustDiffer( s, "StencilFront.CompareMask" );
    }
    {
        auto s                   = Baseline();
        s.StencilFront.WriteMask = 0x0F;
        MustDiffer( s, "StencilFront.WriteMask" );
    }
    {
        auto s                   = Baseline();
        s.StencilFront.Reference = 7;
        MustDiffer( s, "StencilFront.Reference" );
    }
}

TEST( PipelineCacheKey, EveryStencilBackMemberSeparates )
{
    {
        auto s                  = Baseline();
        s.StencilBack.CompareOp = CompareOp::Never;
        MustDiffer( s, "StencilBack.CompareOp" );
    }
    {
        auto s               = Baseline();
        s.StencilBack.FailOp = StencilOp::Zero;
        MustDiffer( s, "StencilBack.FailOp" );
    }
    {
        auto s               = Baseline();
        s.StencilBack.PassOp = StencilOp::Invert;
        MustDiffer( s, "StencilBack.PassOp" );
    }
    {
        auto s                    = Baseline();
        s.StencilBack.DepthFailOp = StencilOp::IncrementAndWrap;
        MustDiffer( s, "StencilBack.DepthFailOp" );
    }
    {
        auto s                    = Baseline();
        s.StencilBack.CompareMask = 0x0F;
        MustDiffer( s, "StencilBack.CompareMask" );
    }
    {
        auto s                  = Baseline();
        s.StencilBack.WriteMask = 0x0F;
        MustDiffer( s, "StencilBack.WriteMask" );
    }
    {
        auto s                  = Baseline();
        s.StencilBack.Reference = 7;
        MustDiffer( s, "StencilBack.Reference" );
    }
}

// The defect that started this. One shader, one framebuffer, one layout, drawn once forward and once over
// the deferred composite: the ONLY difference is which render pass the pipeline is built against.
TEST( PipelineCacheKey, LoadRenderPassSeparates_TheOriginalCollision )
{
    auto s              = Baseline();
    s.UseLoadRenderPass = true;
    MustDiffer( s, "UseLoadRenderPass" );
}

TEST( PipelineCacheKey, VertexLayoutSeparates )
{
    { // a different attribute type changes the Vulkan format
        auto s   = Baseline();
        s.Layout = VertexBufferLayout{ { ShaderDataType::Float4, "a_Position" },
                                       { ShaderDataType::Float2, "a_TextureCoord" } };
        MustDiffer( s, "Layout element type" );
    }
    { // one more attribute changes stride and the attribute count
        auto s   = Baseline();
        s.Layout = VertexBufferLayout{ { ShaderDataType::Float3, "a_Position" },
                                       { ShaderDataType::Float3, "a_Normal" },
                                       { ShaderDataType::Float2, "a_TextureCoord" } };
        MustDiffer( s, "Layout element count" );
    }
    { // same types, swapped order: identical stride and element count, different offsets
        auto s   = Baseline();
        s.Layout = VertexBufferLayout{ { ShaderDataType::Float2, "a_TextureCoord" },
                                       { ShaderDataType::Float3, "a_Position" } };
        MustDiffer( s, "Layout element order" );
    }
    { // no layout at all is an empty vertex input, which is not the baseline's
        auto s   = Baseline();
        s.Layout = std::nullopt;
        MustDiffer( s, "absent Layout" );
    }
}

TEST( PipelineCacheKey, VertexPullingEngagementSeparates )
{
    auto                s = Baseline();
    VertexPullingConfig cfg;
    cfg.VertexStride = 32;
    s.PullingConfig  = cfg;
    MustDiffer( s, "PullingConfig engagement" );
}

// ---------------------------------------------------------------------------------------------------------
// Fields the backend does NOT read. Differing in one of them MUST share, or the cache forks for nothing.
// ---------------------------------------------------------------------------------------------------------

TEST( PipelineCacheKey, DebugNameDoesNotSeparate )
{
    auto s      = Baseline();
    s.DebugName = "A completely different label";
    MustShare( s, "DebugName" );
}

// The backend builds attributes from Type and Offset only, so a renamed attribute is the same pipeline.
TEST( PipelineCacheKey, AttributeNamesAndNormalizationDoNotSeparate )
{
    auto s   = Baseline();
    s.Layout = VertexBufferLayout{ { ShaderDataType::Float3, "position_renamed" },
                                   { ShaderDataType::Float2, "uv_renamed" } };
    MustShare( s, "Layout element names" );
}

// LineWidth is dynamic: VK_DYNAMIC_STATE_LINE_WIDTH is declared and vkCmdSetLineWidth is issued per draw, so
// the baked value is ignored by Vulkan. This assertion is the record of that decision -- if line width ever
// stops being dynamic, this test fails and says why, which is the outcome we want.
TEST( PipelineCacheKey, LineWidthDoesNotSeparate_BecauseItIsDynamicState )
{
    auto s      = Baseline();
    s.LineWidth = 8.0F;
    MustShare( s, "LineWidth" );
}

// With pulling engaged the backend never looks at Layout, so two pulling specs with different layouts are
// genuinely one pipeline. Folding the layout in unconditionally would have been the easy wrong answer.
TEST( PipelineCacheKey, UnderVertexPullingTheLayoutIsIgnored )
{
    VertexPullingConfig cfg;
    cfg.VertexStride = 32;

    auto a          = Baseline();
    a.PullingConfig = cfg;
    a.Layout        = VertexBufferLayout{ { ShaderDataType::Float3, "a_Position" } };

    auto b          = Baseline();
    b.PullingConfig = cfg;
    b.Layout =
         VertexBufferLayout{ { ShaderDataType::Float4, "a_Anything" }, { ShaderDataType::Float4, "a_Else" } };

    EXPECT_TRUE( PipelineCache::SharesPipeline( a, b ) )
         << "vertex pulling makes the backend ignore Layout, so these are one pipeline";
}

// Stencil members are only read when the test is enabled, so they must not fork a key when it is off.
TEST( PipelineCacheKey, StencilMembersAreInertWhileTheTestIsOff )
{
    auto a                  = Baseline();
    a.StencilTestEnabled    = false;
    auto b                  = a;
    b.StencilFront.PassOp   = StencilOp::Invert;
    b.StencilBack.WriteMask = 0x0F;

    EXPECT_TRUE( PipelineCache::SharesPipeline( a, b ) )
         << "stencil state is not read while StencilTestEnabled is false";
}

// ---------------------------------------------------------------------------------------------------------
// The hash must agree with equality, and the census must be maintained.
// ---------------------------------------------------------------------------------------------------------

// A field added to `operator==` but forgotten in KeyHash makes equal keys hash differently, which breaks
// unordered_map outright -- the entry becomes unfindable and every GetOrCreate builds a new pipeline.
TEST( PipelineCacheKey, EqualKeysHashEqually )
{
    EXPECT_EQ( PipelineCache::HashOf( Baseline() ), PipelineCache::HashOf( Baseline() ) );

    auto renamed      = Baseline();
    renamed.DebugName = "different label, same pipeline";
    ASSERT_TRUE( PipelineCache::SharesPipeline( Baseline(), renamed ) );
    EXPECT_EQ( PipelineCache::HashOf( Baseline() ), PipelineCache::HashOf( renamed ) )
         << "these two specs share a cache entry, so their hashes must agree or the entry is unreachable";
}

// Distinct specs are ALLOWED to collide in a hash -- that is what buckets are for -- but if the four fields
// this task restored collided in practice the cache would still be slow and confusing. Cheap to assert.
TEST( PipelineCacheKey, TheRestoredFieldsAlsoChangeTheHash )
{
    const size_t base = PipelineCache::HashOf( Baseline() );

    auto load              = Baseline();
    load.UseLoadRenderPass = true;
    EXPECT_NE( base, PipelineCache::HashOf( load ) );

    auto layout   = Baseline();
    layout.Layout = VertexBufferLayout{ { ShaderDataType::Float4, "a_Position" } };
    EXPECT_NE( base, PipelineCache::HashOf( layout ) );

    auto back                  = Baseline();
    back.StencilBack.WriteMask = 0x0F;
    EXPECT_NE( base, PipelineCache::HashOf( back ) );

    auto                pulling = Baseline();
    VertexPullingConfig cfg;
    cfg.VertexStride      = 32;
    pulling.PullingConfig = cfg;
    EXPECT_NE( base, PipelineCache::HashOf( pulling ) );
}

// The census. GraphicsPipelineSpecification has 20 members; this suite decides about every one of them, and
// the number is written here so that adding a member without deciding shows up as a failure rather than as a
// pipeline that quietly shares someone else's. If you added a field: add its assertion above, then this
// count. If you are here because the count is wrong, the answer is never to edit the number alone.
TEST( PipelineCacheKey, EveryFieldOfTheSpecificationIsAccountedFor )
{
    // This is a COMPILE-TIME census, not a comparison of two numbers I wrote down -- a test that asserts
    // 19 + 2 == 21 passes forever and checks nothing. Structured bindings must name every member of the
    // aggregate exactly, so adding or removing one in Pipeline.hpp fails to build right here.
    //
    // Separating (20): the binding names them in declaration order.
    // Deliberately inert (2): lineWidth is dynamic state, debugName is a label.
    GraphicsPipelineSpecification spec                                      = Baseline();
    auto& [shader, framebuffer, targetLayout, renderpass, layout, pullingConfig, depthTest, depthCompare,
           stencilTest, stencilFront, stencilBack, cullMode, depthWrite, blendEnable, srcBlend, dstBlend,
           useLoadRenderPass, lineWidth, topology, polygonMode, patchControlPoints, debugName] = spec;

    // Touch the two inert ones so the census also states WHICH members were excused, rather than leaving a
    // reader to infer it from an unused-variable warning.
    EXPECT_FLOAT_EQ( 1.0F, lineWidth );
    EXPECT_EQ( "Baseline", debugName );
    EXPECT_FALSE( useLoadRenderPass );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
