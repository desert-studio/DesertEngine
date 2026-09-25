// Per-view descriptor sets of one material (RT2e): the device-free half of VulkanMaterialBackend's sets.
//
// The Vulkan backend supplies three callbacks — make the sets from the view's pool chain, write fallbacks,
// write one image seed — and everything else (which view owns which set, when a set is made, what a new
// set starts with, when a pool is full) is decided here. The doubles below stand in for VkDescriptorSet as
// a binding -> handle map, which is exactly the part of a set the rules are about.

#include <Engine/Graphic/ViewDescriptorSets.hpp>

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>

using namespace Desert::Graphic;

namespace
{
    constexpr uint64_t kFallback = 1;

    struct FakeSets final : IViewDescriptorSetCopy
    {
        std::map<uint32_t, uint64_t> Descriptors; // binding -> what the set points at
    };

    // One material: two buffer bindings (0, 1) and one image binding (2).
    struct FakeMaterial
    {
        ViewDescriptorSets Sets;
        int                Made = 0;

        FakeSets* Resolve( uint32_t frame )
        {
            IViewDescriptorSetCopy* out      = nullptr;
            auto                    resolved = Sets.Resolve(
                 frame,
                 [this]( std::string_view, uint32_t, std::unique_ptr<IViewDescriptorSetCopy>& made )
                 {
                     ++Made;
                     made = std::make_unique<FakeSets>();
                     return Common::MakeSuccess( true );
                 },
                 [this]( uint32_t f )
                 {
                     for ( uint32_t binding = 0; binding < 3; ++binding )
                         Active( f )->Descriptors[binding] = kFallback;
                 },
                 out );
            if ( !resolved.IsSuccess() )
            {
                ADD_FAILURE() << resolved.GetError();
                return nullptr;
            }
            return static_cast<FakeSets*>( out ); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast): the
                                                  // maker above makes only FakeSets
        }

        [[nodiscard]] FakeSets* Active( uint32_t frame ) const
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the maker above makes only FakeSets
            return static_cast<FakeSets*>( Sets.FindActive( frame ) );
        }

        // An image write, as ApplyTexture2D does it: into the active view's set, and recorded as a seed.
        void WriteImage( uint32_t frame, uint32_t binding, uint64_t image )
        {
            FakeSets* sets = Resolve( frame );
            if ( sets == nullptr )
                return;
            sets->Descriptors[binding] = image;
            Sets.Seed( binding,
                       [this, binding, image]( uint32_t f ) { Active( f )->Descriptors[binding] = image; } );
        }
    };
} // namespace

// The isolation the whole layer exists for: a write into one view's set is invisible in another view's.
TEST( ViewDescriptorSets, TwoViewsHaveTheirOwnSetsAndAWriteInOneIsNotSeenByTheOther )
{
    FakeMaterial  material;
    ViewResources viewport( "Viewport" );
    ViewResources preview( "Preview" );

    FakeSets* inViewport = nullptr;
    {
        const ActiveViewScope active( viewport );
        inViewport                 = material.Resolve( 0 );
        inViewport->Descriptors[0] = 11; // the viewport's uniform-buffer copy
    }
    FakeSets* inPreview = nullptr;
    {
        const ActiveViewScope active( preview );
        inPreview = material.Resolve( 0 );
        ASSERT_NE( inPreview, nullptr );
        EXPECT_NE( inPreview, inViewport ) << "two views were handed the same descriptor sets";
        EXPECT_EQ( inPreview->Descriptors[0], kFallback ) << "the preview's set shows the viewport's buffer write";
        inPreview->Descriptors[0] = 22;
    }
    EXPECT_EQ( inViewport->Descriptors[0], 11u ) << "the preview's write reached the viewport's set";
    EXPECT_EQ( material.Made, 2 );
}

// A set is made on first use in a view, once per frame in flight, and never again while it lives.
TEST( ViewDescriptorSets, SetsAreMadeLazilyOncePerViewAndFrame )
{
    FakeMaterial          material;
    ViewResources         view( "Viewport" );
    const ActiveViewScope active( view );
    EXPECT_EQ( material.Active( 0 ), nullptr ) << "a set existed before the view bound the material";
    (void)material.Resolve( 0 );
    (void)material.Resolve( 0 );
    (void)material.Resolve( 1 );
    EXPECT_EQ( material.Made, 2 );
    EXPECT_EQ( view.CopyCount(), 2u );
}

// The "black material in a preview opened later" defect: the texture was written while the viewport alone
// was open, its dirty window closed, and a view opened afterwards must still get it — not the fallback.
TEST( ViewDescriptorSets, AViewOpenedAfterAnImageWriteStartsWithThatImage )
{
    FakeMaterial  material;
    ViewResources viewport( "Viewport" );
    {
        const ActiveViewScope active( viewport );
        material.WriteImage( 0, 2, 777 );
    }
    ViewResources         preview( "Preview opened later" );
    const ActiveViewScope active( preview );
    FakeSets*             sets = material.Resolve( 0 );
    ASSERT_NE( sets, nullptr );
    EXPECT_EQ( sets->Descriptors[2], 777u ) << "the later view binds the fallback instead of the material's image";
    EXPECT_EQ( sets->Descriptors[1], kFallback ) << "a binding with no seed must still get its fallback";
}

// Closing a view gives its sets back; destroying the material takes its sets from every live view.
TEST( ViewDescriptorSets, SetsLeaveWithTheViewAndWithTheMaterial )
{
    ViewResources viewport( "Viewport" );
    {
        auto material = std::make_unique<FakeMaterial>();
        {
            const ActiveViewScope active( viewport );
            (void)material->Resolve( 0 );
        }
        {
            ViewResources         preview( "Preview" );
            const ActiveViewScope active( preview );
            (void)material->Resolve( 0 );
            EXPECT_EQ( preview.CopyCount(), 1u );
        }
        EXPECT_EQ( viewport.CopyCount(), 1u );
    }
    EXPECT_EQ( viewport.CopyCount(), 0u ) << "a destroyed material left its sets in a live view";
}

namespace
{
    struct FakePool
    {
        uint32_t Ordinal  = 0;
        uint32_t Capacity = 0;
    };

    using Chain = DescriptorPoolChain<FakePool>;

    Chain::CreatePool Pools( uint32_t capacity )
    {
        return [capacity]( uint32_t ordinal, std::shared_ptr<FakePool>& out )
        {
            out = std::make_shared<FakePool>( FakePool{ ordinal, capacity } );
            return Common::MakeSuccess( true );
        };
    }

    PoolAllocation TakeOne( FakePool& pool, std::string& why )
    {
        if ( pool.Capacity == 0 )
        {
            why = "VK_ERROR_OUT_OF_POOL_MEMORY";
            return PoolAllocation::PoolExhausted;
        }
        --pool.Capacity;
        return PoolAllocation::Allocated;
    }
} // namespace

// A full pool opens the next one and the allocation succeeds from it.
TEST( ViewDescriptorPoolChain, AFullPoolChainsToANewOne )
{
    Chain                     chain;
    std::shared_ptr<FakePool> from;
    for ( uint32_t i = 0; i < 5; ++i )
    {
        auto allocated = chain.Allocate( Pools( 2 ), TakeOne, from );
        ASSERT_TRUE( allocated.IsSuccess() ) << allocated.GetError();
        EXPECT_EQ( from->Ordinal, i / 2 );
    }
    EXPECT_EQ( chain.PoolCount(), 3u );
}

// A request that does not fit even a fresh pool is refused with the reason, instead of opening pools forever.
TEST( ViewDescriptorPoolChain, ARequestThatNoPoolCanHoldIsRefusedNotLooped )
{
    Chain                     chain;
    std::shared_ptr<FakePool> from;
    auto                      allocated = chain.Allocate( Pools( 0 ), TakeOne, from );
    ASSERT_FALSE( allocated.IsSuccess() );
    EXPECT_NE( allocated.GetError().find( "VK_ERROR_OUT_OF_POOL_MEMORY" ), std::string::npos )
         << allocated.GetError();
    EXPECT_EQ( chain.PoolCount(), 1u );
}

// Any other failure stops at once: a new pool would fail the same way.
TEST( ViewDescriptorPoolChain, AHardFailureDoesNotOpenAnotherPool )
{
    Chain                     chain;
    std::shared_ptr<FakePool> from;
    auto                      allocated = chain.Allocate(
         Pools( 4 ),
         []( FakePool&, std::string& why )
         {
             why = "VK_ERROR_OUT_OF_DEVICE_MEMORY";
             return PoolAllocation::Failed;
         },
         from );
    ASSERT_FALSE( allocated.IsSuccess() );
    EXPECT_EQ( chain.PoolCount(), 1u );
}
