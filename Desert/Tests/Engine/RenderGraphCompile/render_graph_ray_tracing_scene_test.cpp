// RT-SCENE: the view's TLAS pass as the graph sees it. Device-free: a fake RayTracingSceneTarget answers the sizes
// and stands in for the persistent TLAS storage, and the graph is only compiled.
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneRenderer.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace
{
    using namespace Desert::Graphic;
    using namespace Desert::Graphic::RDG;
    using namespace Desert::Graphic::RayTracing;
    namespace Common = Desert::Common;

    class Estimate final : public IMemoryRequirementsProvider
    {
    public:
        [[nodiscard]] Common::ResultStr<MemoryRequirements> GetTextureRequirements( const TextureDesc&,
                                                                                    uint32_t ) const override
        {
            return Common::MakeSuccess( MemoryRequirements{ 64ull * 1024ull, 64ull * 1024ull, ~0u } );
        }
        [[nodiscard]] Common::ResultStr<MemoryRequirements> GetBufferRequirements( const BufferDesc& desc,
                                                                                   uint32_t ) const override
        {
            return Common::MakeSuccess(
                 MemoryRequirements{ ( desc.Bytes + 255ull ) / 256ull * 256ull, 256ull, ~0u } );
        }
    };

    class FakeTarget final : public RayTracingSceneTarget
    {
    public:
        FakeTarget() : m_Storage( BufferDesc{ 4096 }, Access::None )
        {
        }
        Common::ResultStr<TlasBuildSizes> Prepare( uint32_t instanceCount ) override
        {
            PreparedFor = instanceCount;
            return Common::MakeSuccess( TlasBuildSizes{ 1024 } );
        }
        ExternalBuffer& GetStorage() override
        {
            return m_Storage;
        }
        Common::BoolResultStr RecordBuild( const PassContext&, const BufferBinding&, const BufferBinding&,
                                           uint32_t ) override
        {
            return BOOLSUCCESS;
        }
        uint32_t PreparedFor = ~0u;

    private:
        ExternalBuffer m_Storage;
    };

    class FakeGeometry final : public RayTracingGeometryResource
    {
    public:
        [[nodiscard]] uint64_t GetDeviceAddress() const override
        {
            return 0xABCD0000ull;
        }
    };

    RayTracingInstanceList OneInstance( const FakeGeometry& geometry )
    {
        RayTracingPrimitive primitive;
        primitive.Geometry    = &geometry;
        primitive.GpuSceneRow = 7;
        return BuildRayTracingInstances( std::span<const RayTracingPrimitive>( &primitive, 1 ) );
    }

    bool Declared( const CompileResult& result, std::string_view name )
    {
        const bool executed = std::any_of( result.Passes.begin(), result.Passes.end(),
                                           [&]( const CompiledPass& pass ) { return pass.Name == name; } );
        const bool culled   = std::find( result.CulledPassNames.begin(), result.CulledPassNames.end(),
                                         std::string( name ) ) != result.CulledPassNames.end();
        return executed || culled;
    }
} // namespace

// Red if the gate is ignored: a frame whose methods trace no ray must declare no TLAS pass and touch no target.
TEST( RayTracingScenePass, GateOffDeclaresNothing )
{
    const FakeGeometry geometry;
    FakeTarget         target;
    Builder            graph( "RayTracingScene.Off" );
    const auto         frame = AddRayTracingScenePass( graph, false, OneInstance( geometry ), target );
    ASSERT_TRUE( frame.IsSuccess() ) << frame.GetError();
    EXPECT_FALSE( frame.GetValue().Built );
    EXPECT_EQ( target.PreparedFor, ~0u );
    const auto result = graph.Compile( Estimate{} );
    ASSERT_TRUE( result.IsSuccess() ) << result.GetError();
    EXPECT_FALSE( Declared( result.GetValue(), kTlasPassName ) );
}

// Red if the pass is not declared with the gate on, does not write the TLAS storage as an acceleration-structure
// build, or is culled while a ray reader consumes the TLAS.
TEST( RayTracingScenePass, GateOnWritesTheTlasAndLivesWithAReader )
{
    const FakeGeometry geometry;
    FakeTarget         target;
    Builder            graph( "RayTracingScene.On" );
    const auto         frame = AddRayTracingScenePass( graph, true, OneInstance( geometry ), target );
    ASSERT_TRUE( frame.IsSuccess() ) << frame.GetError();
    ASSERT_TRUE( frame.GetValue().Built );
    EXPECT_EQ( target.PreparedFor, 1u );
    EXPECT_EQ( frame.GetValue().InstanceCount, 1u );

    const BufferRef tlas = frame.GetValue().Tlas;
    graph.AddPass(
         "RayTracingScene.Reader", PassFlags::Compute | PassFlags::NeverCull,
         [&]( PassBuilder& pass ) { pass.Read( tlas, Access::AccelStructRead ); },
         []( PassContext& ) -> Common::BoolResultStr { return BOOLSUCCESS; } );

    const auto result = graph.Compile( Estimate{} );
    ASSERT_TRUE( result.IsSuccess() ) << result.GetError();
    const CompileResult& compiled = result.GetValue();
    EXPECT_TRUE( compiled.Faults.empty() );
    EXPECT_TRUE( std::any_of( compiled.Passes.begin(), compiled.Passes.end(),
                              [&]( const CompiledPass& pass ) { return pass.Name == kTlasPassName; } ) );
    EXPECT_TRUE( std::find( compiled.CulledPassNames.begin(), compiled.CulledPassNames.end(),
                            std::string( kTlasPassName ) ) == compiled.CulledPassNames.end() );
    const uint32_t buildWrite = 1u << static_cast<uint32_t>( Access::AccelStructBuildWrite );
    EXPECT_TRUE(
         std::any_of( compiled.Usages.begin(), compiled.Usages.end(), [&]( const DerivedUsage& usage )
                      { return usage.Resource == tlas.Index && ( usage.AccessMask & buildWrite ) != 0; } ) );
}

// Red if a row drifts from VkAccelerationStructureInstanceKHR: transform, customIndex:24 | mask:8, the BLAS
// address.
TEST( RayTracingScenePass, InstanceRowsAreVulkanInstances )
{
    const FakeGeometry geometry;
    const auto         list = OneInstance( geometry );
    const auto         rows = EncodeInstanceRows( list.Instances );
    ASSERT_EQ( rows.size(), kInstanceRowBytes );
    float transform[12]{};
    std::memcpy( transform, rows.data(), sizeof( transform ) );
    EXPECT_EQ( transform[0], 1.0f );
    EXPECT_EQ( transform[5], 1.0f );
    EXPECT_EQ( transform[10], 1.0f );
    uint32_t indexAndMask = 0;
    std::memcpy( &indexAndMask, rows.data() + 48, sizeof( indexAndMask ) );
    EXPECT_EQ( indexAndMask & 0xFFFFFFu, 7u );
    EXPECT_EQ( indexAndMask >> 24u, static_cast<uint32_t>( kMaskOpaque | kMaskShadowCaster ) );
    uint64_t address = 0;
    std::memcpy( &address, rows.data() + 56, sizeof( address ) );
    EXPECT_EQ( address, 0xABCD0000ull );
    EXPECT_EQ( EncodeInstanceRows( {} ).size(), kInstanceRowBytes ); // an empty TLAS still has a buffer
}
