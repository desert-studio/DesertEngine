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
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/DebugViewState.hpp>

#include "../../TestSupport/scratch_dir.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <regex>
#include <string>
#include <vector>
#include <set>

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

// GBUF1d: Vulkan forbids blending into an integer colour attachment (VUID-...-renderPass-06041). The per-slot
// blend state is decided by the attachment's FORMAT: a blended material drawn into the G-buffer (slot 2 =
// R32_UINT shading word) blends its float slots and never the integer one. Mutation: ColourAttachmentBlends
// returns pipelineBlend alone (blend left on for the integer slot) -> red.
TEST( PipelineBlendState, AnIntegerAttachmentNeverBlendsWhateverTheMaterialAsks )
{
    using Desert::Core::Formats::ImageFormat;
    EXPECT_TRUE( Desert::Core::Formats::IsIntegerFormat( ImageFormat::R32_UINT ) );
    EXPECT_FALSE( Desert::Core::Formats::IsIntegerFormat( ImageFormat::R32F ) );

    GraphicsPipelineSpecification spec = Baseline();
    spec.Framebuffer.reset();
    spec.TargetLayout = RenderTargetLayout{ .ColorFormats = { ImageFormat::RGBA16F, ImageFormat::RGBA16F,
                                                              ImageFormat::R32_UINT, ImageFormat::RGBA16F } };
    spec.BlendEnable  = true;

    const std::vector<std::optional<ImageFormat>> formats = ColourAttachmentFormats( spec );
    ASSERT_EQ( formats.size(), 4u );
    EXPECT_EQ( ColourAttachmentBlendEnables( formats, spec.BlendEnable ),
               ( std::vector<bool>{ true, true, false, true } ) );
    EXPECT_EQ( ColourAttachmentBlendEnables( formats, false ), ( std::vector<bool>( 4, false ) ) );
}

// GBUF1g: an UNUSED colour slot (the RSM's slot 2: RenderTargetLayout std::nullopt / FramebufferAttachment::
// UnusedColourSlot) keeps the slots after it at their locations, still gets a blend entry (Vulkan wants one per
// colour reference) that never blends, and separates the pipeline key from a layout with an image there.
// Mutations: the unused entry dropped from ColourAttachmentBlendEnables (3 entries, slot 3 shifted) -> red; the
// key mixing an unused slot as format 0 (no +1 in PipelineCache::MakeKey) -> red.
TEST( PipelineBlendState, AnUnusedColourSlotKeepsItsPlaceAndNeverBlends )
{
    using Desert::Core::Formats::ImageFormat;
    GraphicsPipelineSpecification spec = Baseline();
    spec.Framebuffer.reset();
    spec.TargetLayout = RenderTargetLayout{
         .ColorFormats = { ImageFormat::RGBA8F, ImageFormat::RGBA16F, std::nullopt, ImageFormat::RGBA16F } };
    spec.BlendEnable  = true;
    const std::vector<std::optional<ImageFormat>> formats = ColourAttachmentFormats( spec );
    ASSERT_EQ( formats.size(), 4u );
    EXPECT_FALSE( formats[2].has_value() );
    EXPECT_EQ( ColourAttachmentBlendEnables( formats, true ), ( std::vector<bool>{ true, true, false, true } ) );

    GraphicsPipelineSpecification filled = spec;
    // RGBA8F is enumerator 0: the key must not mix an unused slot as the first format.
    filled.TargetLayout->ColorFormats[2] = ImageFormat::RGBA8F;
    EXPECT_FALSE( PipelineCache::SharesPipeline( spec, filled ) )
         << "an unused colour slot and an image in that slot produced one pipeline key.";

    // The framebuffer route reports the same: the slot is in the list, without a format.
    FramebufferAttachment unused = FramebufferAttachment::UnusedColourSlot();
    EXPECT_TRUE( unused.Unused );
    EXPECT_FALSE( unused.ColourSlotFormat().has_value() );
    EXPECT_EQ( FramebufferAttachment( ImageFormat::RGBA8F ).ColourSlotFormat(), ImageFormat::RGBA8F );
}

// The RSM's colour slots are one list (ViewTargetFormats::kRSMColourSlots) read by the framebuffer and by the
// RSM pipeline's target layout; slot 2 is unused (no shading-word image: the DESERT_GBUFFER_RSM permutation
// writes none) and the others keep the G-buffer's locations. Mutation: the RSM framebuffer or pipeline spelled
// from its own list (or kRSMShadingWord back in slot 2) -> red.
TEST( PipelineBlendState, TheRSMSlotsAreOneListWithSlotTwoUnused )
{
    namespace F = Desert::Graphic::ViewTargetFormats;
    ASSERT_EQ( F::kRSMColourSlots.size(), 4u );
    EXPECT_EQ( F::kRSMColourSlots[0], F::kGBufferA );
    EXPECT_EQ( F::kRSMColourSlots[1], F::kGBufferB );
    EXPECT_FALSE( F::kRSMColourSlots[2].has_value() );
    EXPECT_EQ( F::kRSMColourSlots[3], F::kGBufferEmissive );

    const auto read = []( const char* relative )
    {
        std::ifstream in( Desert::TestSupport::RepositoryRoot() / relative, std::ios::binary );
        std::string   text( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
        std::erase_if( text, []( unsigned char c ) { return std::isspace( c ) != 0; } );
        return text;
    };
    const std::string scene = read( "Desert/Desert/Source/Engine/Graphic/SceneRenderer.cpp" );
    EXPECT_NE( scene.find( "for(conststd::optional<Core::Formats::ImageFormat>&slot:ViewTargetFormats::kRSMColourSlots)"
                           "rsmSpec.Attachments.Attachments.push_back(slot?FramebufferAttachment(*slot):"
                           "FramebufferAttachment::UnusedColourSlot());" ),
               std::string::npos )
         << "the RSM framebuffer is not built from kRSMColourSlots.";
    const std::string mesh = read( "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRendererDeferred.cpp" );
    EXPECT_NE( mesh.find( "rsmSpec.Framebuffer.reset();rsmSpec.TargetLayout=RenderTargetLayout{.ColorFormats="
                          "std::vector<std::optional<Core::Formats::ImageFormat>>(ViewTargetFormats::kRSMColourSlots"
                          ".begin(),ViewTargetFormats::kRSMColourSlots.end()),.DepthFormat=ViewTargetFormats::"
                          "kRSMDepth};" ),
               std::string::npos )
         << "the RSM pipeline is not built against kRSMColourSlots.";
}

// An unused colour slot (FramebufferAttachment::UnusedColourSlot, the RSM's slot 2) has no image:
// GetColorAttachmentImage / GetMultisampleColorAttachmentImage return null and its graph ref is invalid. Every code
// path that walks a framebuffer's colour slots by index is named here with the guard that skips the slot; a new walker
// (a file outside the list calling GetColorAttachmentCount / GetMultisampleColorAttachmentImage) is red until it is
// named with its guard. Readers of ONE slot (GetColorAttachmentImage(0) of the scene target, tonemap, SMAA, FXAA,
// SSR/GI accumulators, UI / movie targets, the cascades) name framebuffers that have no unused slot.
// Mutation: drop any one guard below (e.g. Colors()'s `if(!image){refs.emplace_back();continue;}`) -> red.
TEST( PipelineBlendState, EveryColourSlotWalkerSkipsAnUnusedSlot )
{
    const auto read = []( const std::filesystem::path& path )
    {
        std::ifstream in( path, std::ios::binary );
        std::string   text( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
        std::erase_if( text, []( unsigned char c ) { return std::isspace( c ) != 0; } );
        return text;
    };
    const std::filesystem::path root  = Desert::TestSupport::RepositoryRoot();
    const std::string           frame = read( root / "Desert/Desert/Source/Engine/Graphic/SceneRendererFrame.hpp" );
    ASSERT_FALSE( frame.empty() );
    // FrameTextures::Colors: an invalid ref at the slot.
    EXPECT_NE( frame.find( "conststd::shared_ptr<Image2D>&image=framebuffer->GetColorAttachmentImage(i);if(!image)"
                           "{refs.emplace_back();continue;}" ),
               std::string::npos )
         << "FrameTextures::Colors imports an unused slot's null image.";
    // FrameTextures::MultisampleColors: the same, for the multisampled images.
    EXPECT_NE( frame.find( "conststd::shared_ptr<Image2D>&image=framebuffer->GetMultisampleColorAttachmentImage(i);"
                           "if(!image){refs.emplace_back();continue;}" ),
               std::string::npos )
         << "FrameTextures::MultisampleColors drops an unused slot and shifts the slots after it.";
    // FrameTextures::ImportFramebuffer.
    EXPECT_NE( frame.find( "imported.Colors.push_back(image?Import(image,std::format(\"{}.Color{}\",name,i)):"
                           "RDG::TextureRef{});" ),
               std::string::npos )
         << "FrameTextures::ImportFramebuffer imports an unused slot's null image.";

    // AddRaster (SceneRendererFrameMesh.cpp) and LoadTarget (DeferredFrameNodes.hpp): no target for an invalid slot.
    const std::string mesh = read( root / "Desert/Desert/Source/Engine/Graphic/SceneRendererFrameMesh.cpp" );
    EXPECT_NE( mesh.find( "if(targets.Colors[slot].IsValid())pass.ColorTarget(slot,targets.Colors[slot],color);" ),
               std::string::npos )
         << "AddRaster declares a target for an unused colour slot.";
    EXPECT_NE( mesh.find( "if(targets.Resolves[slot].IsValid())pass.ResolveTarget(slot,targets.Resolves[slot]);" ),
               std::string::npos )
         << "AddRaster declares a resolve for an unused colour slot.";
    const std::string nodes = read( root / "Desert/Desert/Source/Engine/Graphic/DeferredFrameNodes.hpp" );
    EXPECT_NE( nodes.find( "if(target.Colors[i].IsValid())pass.ColorTarget(i,target.Colors[i],RDG::LoadOp::Load());" ),
               std::string::npos )
         << "LoadTarget declares a target for an unused colour slot.";
    EXPECT_NE( nodes.find( "if(target.Resolves[i].IsValid())pass.ResolveTarget(i,target.Resolves[i]);" ),
               std::string::npos )
         << "LoadTarget declares a resolve for an unused colour slot.";

    // VulkanFramebuffer::RT_Invalidate (create and Resize): the colour reference, the resolve reference, the
    // multisampled image and the single-sample image each skip an unused slot.
    const std::string vk = read( root / "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanFramebuffer.cpp" );
    std::size_t       guards = 0;
    for ( auto at = vk.find( "if(attachment.Unused)" ); at != std::string::npos;
          at      = vk.find( "if(attachment.Unused)", at + 1 ) )
        ++guards;
    EXPECT_EQ( guards, 4u ) << "VulkanFramebuffer::RT_Invalidate: a walk over the attachments lost its unused guard.";

    // No walker outside the named ones.
    const std::set<std::string> walkers = { "SceneRendererFrame.hpp", "Pipeline.hpp", "Framebuffer.hpp",
                                            "VulkanFramebuffer.hpp", "VulkanFramebuffer.cpp" };
    std::vector<std::string>    unnamed;
    for ( const char* dir : { "Desert/Desert/Source", "Editor/Source", "Runtime/Source" } )
    {
        if ( !std::filesystem::exists( root / dir ) )
            continue;
        for ( const auto& entry : std::filesystem::recursive_directory_iterator( root / dir ) )
        {
            const auto ext = entry.path().extension();
            if ( !entry.is_regular_file() || ( ext != ".cpp" && ext != ".hpp" && ext != ".h" ) )
                continue;
            const std::string text = read( entry.path() );
            if ( ( text.find( "GetColorAttachmentCount(" ) != std::string::npos ||
                   text.find( "GetMultisampleColorAttachmentImage(" ) != std::string::npos ) &&
                 !walkers.contains( entry.path().filename().string() ) )
                unnamed.push_back( entry.path().filename().string() );
        }
    }
    EXPECT_TRUE( unnamed.empty() ) << "walks a framebuffer's colour slots without being named here: "
                                   << ::testing::PrintToString( unnamed );
}

// The function above is only the rule if pipeline creation obeys it: VulkanPipeline::CreateColorBlendState
// takes every blendEnable from ColourAttachmentBlendEnables and never reads the requested blend itself.
// Mutation: the loop sets `.blendEnable = m_Specification.BlendEnable` again -> red.
TEST( PipelineBlendState, PipelineCreationTakesEveryBlendSwitchFromTheRule )
{
    std::ifstream file( Desert::TestSupport::RepositoryRoot() /
                        "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanPipeline.cpp" );
    ASSERT_TRUE( file );
    std::string text( ( std::istreambuf_iterator<char>( file ) ), std::istreambuf_iterator<char>() );
    text.erase( std::remove_if( text.begin(), text.end(), []( unsigned char c ) { return std::isspace( c ) != 0; } ),
                text.end() );
    const size_t begin = text.find( "voidVulkanPipeline::CreateColorBlendState()" );
    ASSERT_NE( begin, std::string::npos );
    const size_t      end  = text.find( "voidVulkanPipeline::", begin + 1 );
    const std::string body = text.substr( begin, end - begin );
    EXPECT_NE( body.find( "ColourAttachmentBlendEnables(ColourAttachmentFormats(m_Specification),"
                          "m_Specification.BlendEnable)" ),
               std::string::npos );
    EXPECT_NE( body.find( ".blendEnable=blends[slot]?VK_TRUE:VK_FALSE" ), std::string::npos );
    // The requested blend reaches the attachments only through the rule: named once, as its argument.
    size_t uses = 0;
    for ( size_t at = body.find( "m_Specification.BlendEnable" ); at != std::string::npos;
          at = body.find( "m_Specification.BlendEnable", at + 1 ) )
        ++uses;
    EXPECT_EQ( uses, 1u );
}

// G-buffer slot 2 is the R32_UINT shading word. A float sampler over it reinterprets the bits, so: no shader declares a
// float sampler for a *ShadingWord, none reads one through texture()/textureLod (texelFetch only), the one C++ binding is
// DeferredLighting's u_GBufferShadingWord fed from gbuffer[2], and the editor's buffer views of the word's three fields
// (Material Complexity = TEXTURES, Shading Model = INDEX, Sun Shadow Receive = NO_SUN_SHADOWS) decode the one integer
// fetch. Mutations: `usampler2D u_GBufferShadingWord` -> `sampler2D`, or a debug branch reading texture(u_GBufferShadingWord,
// ...), or a second .Sampled("u_GBufferShadingWord", ...) anywhere -> red.
TEST( GBufferShadingWord, NoFloatSamplerBindsTheShadingWord )
{
    const auto read = []( const std::filesystem::path& path )
    {
        std::ifstream in( path, std::ios::binary );
        return std::string( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
    };
    const auto strip = []( std::string text )
    {
        std::erase_if( text, []( unsigned char c ) { return std::isspace( c ) != 0; } );
        return text;
    };
    const std::filesystem::path root = Desert::TestSupport::RepositoryRoot();

    const std::regex floatSampler( R"((^|[^u])sampler2D(Array)?\s+\w*ShadingWord)" );
    const std::regex floatRead( R"(texture(Lod|Grad|Offset)?\s*\(\s*\w*ShadingWord)" );
    std::vector<std::string> offenders;
    std::size_t              shaders = 0;
    for ( const auto& entry : std::filesystem::recursive_directory_iterator( root / "Editor/Resources/Shaders" ) )
    {
        const auto ext = entry.path().extension();
        if ( !entry.is_regular_file() || ( ext != ".shader" && ext != ".glslh" && ext != ".glsl" ) )
            continue;
        ++shaders;
        const std::string text = read( entry.path() );
        if ( std::regex_search( text, floatSampler ) || std::regex_search( text, floatRead ) )
            offenders.push_back( entry.path().filename().string() );
    }
    EXPECT_GT( shaders, 50u );
    EXPECT_TRUE( offenders.empty() ) << "reads the R32_UINT shading word through a float sampler: "
                                     << ::testing::PrintToString( offenders );

    const std::string lighting =
         strip( read( root / "Editor/Resources/Shaders/Programs/Deferred/DeferredLighting.shader" ) );
    ASSERT_FALSE( lighting.empty() );
    EXPECT_NE( lighting.find( "Uniform(1)usampler2Du_GBufferShadingWord;" ), std::string::npos );
    const auto fetch = lighting.find( "constuintword=texelFetch(u_GBufferShadingWord,ivec2(gl_FragCoord.xy),0).r;" );
    ASSERT_NE( fetch, std::string::npos );
    for ( const char* branch : { "if(dbg==9){oColor=vec4(HeatColor(DesertSampledTextureCount(word)",
                                 "if(dbg==10){constfloathue=fract(float(shadingModel)",
                                 "if(dbg==11){oColor=DesertReceivesSunShadows(word)" } )
    {
        const auto at = lighting.find( branch );
        EXPECT_NE( at, std::string::npos ) << branch;
        EXPECT_GT( at, fetch ) << branch << " does not decode the integer fetch.";
    }
    EXPECT_NE( lighting.find( "constintshadingModel=DesertShadingModelIndex(word);" ), std::string::npos );
    EXPECT_EQ( static_cast<int>( Desert::Graphic::DeferredDebugMode::MaterialComplexity ), 9 );
    EXPECT_EQ( static_cast<int>( Desert::Graphic::DeferredDebugMode::ShadingModel ), 10 );
    EXPECT_EQ( static_cast<int>( Desert::Graphic::DeferredDebugMode::SunShadowReceive ), 11 );

    // C++: the only binding of the word by name, fed from G-buffer slot 2.
    std::vector<std::string> binders;
    for ( const auto& entry : std::filesystem::recursive_directory_iterator( root / "Desert/Desert/Source" ) )
    {
        const auto ext = entry.path().extension();
        if ( !entry.is_regular_file() || ( ext != ".cpp" && ext != ".hpp" ) )
            continue;
        if ( read( entry.path() ).find( "\"u_GBufferShadingWord\"" ) != std::string::npos )
            binders.push_back( entry.path().filename().string() );
    }
    EXPECT_EQ( binders, std::vector<std::string>{ "DeferredLightingRenderer.hpp" } );
    const std::string renderer =
         strip( read( root / "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Deferred/DeferredLightingRenderer.hpp" ) );
    EXPECT_NE( renderer.find( ".Sampled(\"u_GBufferShadingWord\",inputs.GBufferShadingWord,RDG::Access::SampledGraphics,"
                              "RDG::SubresourceRange::All(),RDG::SamplerDesc::PointClamp())" ),
               std::string::npos );
    const std::string deferred = strip( read( root / "Desert/Desert/Source/Engine/Graphic/SceneRendererFrameDeferred.cpp" ) );
    EXPECT_NE( deferred.find( "inputs.GBufferShadingWord=gbuffer[2];" ), std::string::npos );
}
