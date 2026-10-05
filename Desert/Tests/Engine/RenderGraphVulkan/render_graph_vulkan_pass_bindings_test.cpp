#include <Engine/Graphic/API/Vulkan/VulkanRdgPassBindings.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

// RDG-A2 - the consumer side of RDG::PassBindings: what Renderer::DispatchCompute / DrawFullscreen refuse before
// anything is recorded. The resolution is pure over reflection data, so these run with no device; the graph
// executes on a backend that records nothing and only provides the PassContext the block is built from.

using namespace Desert::Graphic;
using namespace Desert::Graphic::RDG;
using Desert::Core::Formats::ImageFormat;
using Desert::Graphic::API::Vulkan::RdgOtherRoute;
using Desert::Graphic::API::Vulkan::RdgResolvedEntry;
using Desert::Graphic::API::Vulkan::RdgSlotKey;
using Desert::Graphic::API::Vulkan::ResolveRdgPassBindings;
namespace ShaderResource = Desert::Graphic::API::Vulkan::ShaderResource;
namespace Layout         = Desert::ShaderResources::ShaderLayout;

namespace
{
    class FixedEstimate final : public IMemoryRequirementsProvider
    {
    public:
        Common::ResultStr<MemoryRequirements> GetTextureRequirements( const TextureDesc& desc,
                                                                      uint32_t ) const override
        {
            const uint64_t bytes = uint64_t( desc.Size.Width ) * desc.Size.Height * 8u * desc.Layers;
            return Common::MakeSuccess(
                 MemoryRequirements{ ( bytes + 0xFFFFu ) & ~uint64_t( 0xFFFFu ), 0x10000u, ~0u } );
        }
        Common::ResultStr<MemoryRequirements> GetBufferRequirements( const BufferDesc& desc,
                                                                     uint32_t ) const override
        {
            return Common::MakeSuccess(
                 MemoryRequirements{ ( desc.Bytes + 255u ) & ~uint64_t( 255u ), 256u, ~0u } );
        }
    };

    class NoPlacementAllocator final : public ITransientAllocator
    {
    public:
        void BeginFrameSlot( uint32_t ) override
        {
        }
        Common::BoolResultStr ReserveHeaps( std::string_view, std::span<const TransientHeapDesc> ) override
        {
            return Common::MakeSuccess( true );
        }
        Common::ResultStr<std::shared_ptr<IPhysicalTexture>> PlaceTexture( const Allocation&, const TextureDesc&,
                                                                           uint32_t, std::string_view ) override
        {
            return Common::MakeError<std::shared_ptr<IPhysicalTexture>>( "the mock places nothing" );
        }
        Common::ResultStr<std::shared_ptr<IPhysicalBuffer>> PlaceBuffer( const Allocation&, const BufferDesc&,
                                                                         uint32_t, std::string_view ) override
        {
            return Common::MakeError<std::shared_ptr<IPhysicalBuffer>>( "the mock places nothing" );
        }
        void EndGraph( std::string_view ) override
        {
        }
        TransientAllocatorStats GetStats() const override
        {
            return {};
        }
    };

    // Every call succeeds and records nothing: these tests look only at what the exec lambda sees.
    class NoOpBackend final : public IBackend
    {
    public:
        BackendKind GetKind() const override
        {
            return BackendKind::Recording;
        }
        const IMemoryRequirementsProvider& GetMemoryRequirements() const override
        {
            return m_Memory;
        }
        ITransientAllocator& GetTransientAllocator() override
        {
            return m_Allocator;
        }
        PipeCapabilities GetPipeCapabilities() const override
        {
            return {};
        }
        AsyncComputeFallbackLog& GetAsyncComputeFallbackLog() override
        {
            return m_FallbackLog;
        }
        Common::BoolResultStr BeginPipeSegment( const PipeSegment& ) override
        {
            return Common::MakeSuccess( true );
        }
        Common::BoolResultStr EndPipeSegment( const PipeSegment& ) override
        {
            return Common::MakeSuccess( true );
        }
        void RecordEpilogueBarriers( std::span<const Barrier> ) override
        {
        }
        Common::BoolResultStr BeginGraph( const GraphView& ) override
        {
            return Common::MakeSuccess( true );
        }
        void BeginPass( const CompiledPass& ) override
        {
        }
        void RecordBarriers( std::span<const Barrier> ) override
        {
        }
        Common::BoolResultStr BeginRenderPass( const CompiledPass& ) override
        {
            return Common::MakeSuccess( true );
        }
        void EndRenderPass() override
        {
        }
        void EndPass( const CompiledPass& ) override
        {
        }
        Common::BoolResultStr EndGraph( std::span<const Barrier> ) override
        {
            return Common::MakeSuccess( true );
        }
        void AbandonGraph() override
        {
        }
        std::shared_ptr<IPhysicalTexture> GetPhysicalTexture( uint32_t ) const override
        {
            return nullptr;
        }
        std::shared_ptr<IPhysicalBuffer> GetPhysicalBuffer( uint32_t ) const override
        {
            return nullptr;
        }

    private:
        FixedEstimate           m_Memory;
        NoPlacementAllocator    m_Allocator;
        AsyncComputeFallbackLog m_FallbackLog{ []( std::string_view ) {} };
    };

         // A bloom-upsample-like compute shader: u_Source (sampled 2D, binding 0), u_Output (storage image,
         // binding 1), u_Params (uniform buffer, binding 2, filled by the material), 12 bytes of push constants.
         ShaderResource::ReflectionData
         UpsampleReflection()
    {
        ShaderResource::ReflectionData       reflection;
        ShaderResource::ShaderDescriptorSet& set = reflection.ShaderDescriptorSets[0];
        Layout::Image2DSampler               source;
        source.BindingPoint = 0;
        source.Name         = "u_Source";
        set.Image2DSamplers.emplace( 0, source );
        Layout::Image2DSampler output;
        output.BindingPoint = 1;
        output.Name         = "u_Output";
        set.StorageImage2DSamplers.emplace( 1, output );
        Layout::UniformBuffer params;
        params.BindingPoint = 2;
        params.Name         = "u_Params";
        params.Size         = 16;
        set.UniformBuffers.emplace( 2, params );
        Layout::PushConstantRange push;
        push.Size                     = 12;
        push.Name                     = "Push";
        reflection.PushConstantRanges = push;
        return reflection;
    }

    RdgOtherRoute MaterialFillsParams()
    {
        RdgOtherRoute other;
        other.Filled.push_back( RdgSlotKey{ 0, 2 } );
        return other;
    }

    TextureDesc Chain()
    {
        TextureDesc desc;
        desc.Size   = { 64, 64, 1 };
        desc.Format = ImageFormat::RGBA16F;
        desc.Mips   = 2;
        return desc;
    }

    using Verdict = Common::ResultStr<std::vector<RdgResolvedEntry>>;

    // Runs one compute pass that samples mip 1 of a chain and writes mip 0, and resolves the block @p fill
    // builds against @p reflection; returns the consumer's verdict.
    template <class Fill>
    Verdict ResolveInPass( const ShaderResource::ReflectionData& reflection, const RdgOtherRoute& other,
                           Fill&& fill )
    {
        ExternalTexture  chainImage( Chain(), Access::None );
        Builder          graph( "pass-bindings-consumer" );
        const TextureRef chain   = graph.RegisterExternal( chainImage, "Bloom" );
        Verdict          verdict = Common::MakeError<std::vector<RdgResolvedEntry>>( "the pass did not run" );
        graph.AddPass(
             "PostFX: BloomUpsample1", PassFlags::Compute,
             [&]( PassBuilder& pass )
             {
                 pass.Read( chain, Access::SampledCompute, SubresourceRange::Mip( 1 ) );
                 pass.Write( chain, Access::StorageWrite, SubresourceRange::Mip( 0 ) );
             },
             [&]( PassContext& context ) -> Common::BoolResultStr
             {
                 PassBindings bindings( context );
                 fill( bindings, chain );
                 verdict = ResolveRdgPassBindings( reflection, "BloomUpsample", bindings, other );
                 return Common::MakeSuccess( true );
             } );
        NoOpBackend                 backend;
        const Common::BoolResultStr executed = graph.Execute( backend );
        EXPECT_TRUE( executed.IsSuccess() ) << executed.GetError();
        return verdict;
    }

    const float kPush[3] = { 1.0f / 32.0f, 1.0f / 32.0f, 1.0f };

    void FillComplete( PassBindings& bindings, TextureRef chain )
    {
        bindings.Sampled( "u_Source", chain, Access::SampledCompute, SubresourceRange::Mip( 1 ), SamplerDesc::LinearClamp())
             .Storage( "u_Output", chain, Access::StorageWrite, 0 )
             .PushConstants( kPush, sizeof( kPush ) );
    }
} // namespace

// The complete block resolves each name to its reflected (set, binding); the material's slot is left to it.
TEST( RenderGraphVulkanPassBindings, ACompleteBlockResolvesEveryNameToItsReflectedSlot )
{
    const Verdict resolved = ResolveInPass( UpsampleReflection(), MaterialFillsParams(), FillComplete );
    ASSERT_TRUE( resolved.IsSuccess() ) << resolved.GetError();
    const std::vector<RdgResolvedEntry>& entries = resolved.GetValue();
    ASSERT_EQ( entries.size(), 2u );
    EXPECT_EQ( entries[0].Slot, ( RdgSlotKey{ 0, 0 } ) );
    EXPECT_EQ( entries[0].Kind, ShaderResourceKind::SampledTexture );
    EXPECT_EQ( entries[1].Slot, ( RdgSlotKey{ 0, 1 } ) );
    EXPECT_EQ( entries[1].Kind, ShaderResourceKind::StorageTexture );
}

// A name the shader does not declare is refused, naming the pass, the name and the shader.
TEST( RenderGraphVulkanPassBindings, ANameTheShaderDoesNotDeclareIsRefused )
{
    const Verdict resolved = ResolveInPass( UpsampleReflection(), MaterialFillsParams(),
                                            []( PassBindings& bindings, TextureRef chain )
                                            {
                                                FillComplete( bindings, chain );
                                                bindings.Sampled( "u_Sorce", chain, Access::SampledCompute,
                                                                  SubresourceRange::Mip( 1 ), SamplerDesc::LinearClamp());
                                            } );
    ASSERT_FALSE( resolved.IsSuccess() );
    const std::string error = resolved.GetError();
    EXPECT_NE( error.find( "PostFX: BloomUpsample1" ), std::string::npos ) << error;
    EXPECT_NE( error.find( "'u_Sorce' is not a resource of shader 'BloomUpsample'" ), std::string::npos ) << error;
}

// A sampled entry into a storage-image slot (and the reverse) is refused: the kind is checked against reflection.
TEST( RenderGraphVulkanPassBindings, AKindOtherThanTheReflectedOneIsRefused )
{
    const Verdict resolved = ResolveInPass(
         UpsampleReflection(), MaterialFillsParams(),
         []( PassBindings& bindings, TextureRef chain )
         {
             bindings.Sampled( "u_Output", chain, Access::SampledCompute, SubresourceRange::Mip( 1 ), SamplerDesc::LinearClamp())
                  .Storage( "u_Source", chain, Access::StorageWrite, 0 )
                  .PushConstants( kPush, sizeof( kPush ) );
         } );
    ASSERT_FALSE( resolved.IsSuccess() );
    const std::string error = resolved.GetError();
    EXPECT_NE( error.find( "'u_Output' is bound as a Sampled texture" ), std::string::npos ) << error;
    EXPECT_NE( error.find( "declares it a storage image" ), std::string::npos ) << error;
}

// A resource slot neither route fills is refused - there is no fallback image to hide it - and so is a declared
// push-constant block nothing gives.
TEST( RenderGraphVulkanPassBindings, AnUnfilledSlotIsRefused )
{
    const Verdict noOutput = ResolveInPass(
         UpsampleReflection(), MaterialFillsParams(),
         []( PassBindings& bindings, TextureRef chain )
         {
             bindings.Sampled( "u_Source", chain, Access::SampledCompute, SubresourceRange::Mip( 1 ), SamplerDesc::LinearClamp())
                  .PushConstants( kPush, sizeof( kPush ) );
         } );
    ASSERT_FALSE( noOutput.IsSuccess() );
    EXPECT_NE( noOutput.GetError().find( "'u_Output' (set 0 binding 1)" ), std::string::npos )
         << noOutput.GetError();

    const Verdict noMaterial = ResolveInPass( UpsampleReflection(), RdgOtherRoute{}, FillComplete );
    ASSERT_FALSE( noMaterial.IsSuccess() );
    EXPECT_NE( noMaterial.GetError().find( "'u_Params'" ), std::string::npos ) << noMaterial.GetError();

    const Verdict noPush = ResolveInPass(
         UpsampleReflection(), MaterialFillsParams(),
         []( PassBindings& bindings, TextureRef chain )
         {
             bindings.Sampled( "u_Source", chain, Access::SampledCompute, SubresourceRange::Mip( 1 ), SamplerDesc::LinearClamp())
                  .Storage( "u_Output", chain, Access::StorageWrite, 0 );
         } );
    ASSERT_FALSE( noPush.IsSuccess() );
    EXPECT_NE( noPush.GetError().find( "push-constant block 'Push'" ), std::string::npos ) << noPush.GetError();
}

// A slot filled by both the block and the material (or a pipeline setter) is refused: one route per slot. The
// same holds for the push constants.
TEST( RenderGraphVulkanPassBindings, ASlotFilledByBothRoutesIsRefused )
{
    RdgOtherRoute other = MaterialFillsParams();
    other.Filled.push_back( RdgSlotKey{ 0, 0 } );
    const Verdict both = ResolveInPass( UpsampleReflection(), other, FillComplete );
    ASSERT_FALSE( both.IsSuccess() );
    EXPECT_NE( both.GetError().find( "'u_Source' of shader 'BloomUpsample' is filled both" ), std::string::npos )
         << both.GetError();

    RdgOtherRoute pushToo = MaterialFillsParams();
    pushToo.PushConstants = true;
    const Verdict push    = ResolveInPass( UpsampleReflection(), pushToo, FillComplete );
    ASSERT_FALSE( push.IsSuccess() );
    EXPECT_NE( push.GetError().find( "push constants of shader 'BloomUpsample' are given both" ),
               std::string::npos )
         << push.GetError();
}

// A block with a failed entry is refused with that entry's error, before anything is resolved.
TEST( RenderGraphVulkanPassBindings, ABlockWithAFailedEntryIsRefusedWithThatEntry )
{
    const Verdict resolved = ResolveInPass(
         UpsampleReflection(), MaterialFillsParams(),
         []( PassBindings& bindings, TextureRef chain )
         {
             bindings.Sampled( "u_Source", chain, Access::SampledCompute, SubresourceRange::Mip( 1 ), SamplerDesc::LinearClamp())
                  .Sampled( "u_Source", chain, Access::SampledCompute, SubresourceRange::Mip( 1 ), SamplerDesc::LinearClamp());
         } );
    ASSERT_FALSE( resolved.IsSuccess() );
    EXPECT_NE( resolved.GetError().find( "already bound" ), std::string::npos ) << resolved.GetError();
}
