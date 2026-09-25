// Per-view descriptor sets of one material (RT2e): the device-free half of VulkanMaterialBackend's sets.
//
// The Vulkan backend supplies three callbacks — make the sets from the view's pool chain, write fallbacks,
// write one image seed — and everything else (which view owns which set, when a set is made, what a new
// set starts with, when a pool is full) is decided here. The doubles below stand in for VkDescriptorSet as
// a binding -> handle map, which is exactly the part of a set the rules are about.

#include <Engine/Graphic/ViewDescriptorSets.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

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

// THE LEAK QUESTION (RT1i): 300 frames of one material in one view must allocate sets once per frame in
// flight and never again. If sets were re-allocated per frame, a view's pool chain would grow without bound
// and VK_ERROR_OUT_OF_POOL_MEMORY would be a leak rather than a turnover.
TEST( ViewDescriptorSets, ManyFramesOfOneMaterialAllocateOncePerFrameInFlight )
{
    constexpr int         kFramesInFlight = 3;
    constexpr int         kFrames         = 300;
    FakeMaterial          material;
    ViewResources         view( "Viewport" );
    const ActiveViewScope active( view );
    for ( int frame = 0; frame < kFrames; ++frame )
        (void)material.Resolve( static_cast<uint32_t>( frame % kFramesInFlight ) );
    EXPECT_EQ( material.Made, kFramesInFlight ) << "sets are being re-allocated per frame -- a leak";
    EXPECT_EQ( view.CopyCount(), static_cast<uint32_t>( kFramesInFlight ) );
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
    // One descriptor type is enough to state every capacity rule; 7 stands in for
    // VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, whose value this header must not need to know.
    constexpr uint32_t kUniform = 7;

    struct FakePool
    {
        uint32_t         Ordinal = 0;
        DescriptorBudget Budget;
        // How many times the DRIVER was asked for an allocation out of this pool. The whole point of the
        // reservation is that this never counts a failed attempt.
        int Attempts = 0;
    };

    using Chain = DescriptorPoolChain<FakePool>;

    // `capacity` sets and one uniform descriptor per set, so "the sets ran out" and "the descriptors ran out"
    // are the same moment and each test states only one number.
    Chain::CreatePool Pools( uint32_t capacity )
    {
        return [capacity]( uint32_t ordinal, std::shared_ptr<FakePool>& out )
        {
            out          = std::make_shared<FakePool>();
            out->Ordinal = ordinal;
            out->Budget  = DescriptorBudget( capacity, { { kUniform, capacity } } );
            return Common::MakeSuccess( true );
        };
    }

    // One set with one uniform descriptor: the smallest request a material can make.
    const DescriptorRequest kOneSet{ 1, { { kUniform, 1 } } };

    Chain::Reserve ReserveOne()
    {
        return []( FakePool& pool ) { return pool.Budget.Reserve( kOneSet ); };
    }

    // The driver, which now only ever sees a pool that has already reserved room, so it always succeeds.
    PoolAllocation TakeOne( FakePool& pool, std::string& why )
    {
        (void)why;
        ++pool.Attempts;
        return PoolAllocation::Allocated;
    }
} // namespace

// ---- THE BLOCK'S OWN ACCOUNTING (RT1i) ----
//
// Under RT2e a full block was found by ALLOCATING FROM IT AND FAILING. That failed vkAllocateDescriptorSets
// is reported by the validation layer as an error, and the debug callback breaks into the debugger -- so a
// routine turnover stopped the editor on Windows (VK_ERROR_OUT_OF_POOL_MEMORY in AllocateViewSets). These
// pin the arithmetic that replaces the failed call.

TEST( DescriptorBudget, ARequestIsTakenWholeOrNotAtAll )
{
    DescriptorBudget budget( 2, { { kUniform, 3 } } );

    EXPECT_TRUE( budget.Reserve( kOneSet ) );
    EXPECT_EQ( budget.RemainingSets(), 1U );
    EXPECT_EQ( budget.Remaining( kUniform ), 2U );

    // Wants more descriptors than are left: refused, and the budget is UNCHANGED. A partial take would leave
    // the block short of the set it had just promised.
    const DescriptorRequest greedy{ 1, { { kUniform, 9 } } };
    EXPECT_FALSE( budget.Reserve( greedy ) );
    EXPECT_EQ( budget.RemainingSets(), 1U ) << "a refused request still consumed a set";
    EXPECT_EQ( budget.Remaining( kUniform ), 2U ) << "a refused request still consumed descriptors";
}

TEST( DescriptorBudget, TheSetCOUNTRunsOutIndependentlyOfTheDescriptors )
{
    DescriptorBudget budget( 1, { { kUniform, 100 } } );
    EXPECT_TRUE( budget.Reserve( kOneSet ) );
    EXPECT_FALSE( budget.CanHold( kOneSet ) ) << "maxSets is a limit of its own, descriptors to spare or not";
    EXPECT_EQ( budget.Remaining( kUniform ), 99U );
}

// A pool is created with a fixed list of types; a shader declaring one that is not on the list can never be
// satisfied, and saying so by name is what stops it becoming a per-draw failure inside the driver.
TEST( DescriptorBudget, ATypeThePoolWasNotCreatedWithHasNoRoom )
{
    DescriptorBudget        budget( 8, { { kUniform, 8 } } );
    const DescriptorRequest storageImage{ 1, { { kUniform + 1, 1 } } };
    EXPECT_FALSE( budget.CanHold( storageImage ) );
    EXPECT_EQ( budget.Remaining( kUniform + 1 ), 0U );
}

// THE REGRESSION ITSELF: a turnover must not cost a failed allocation. Two sets fit per block, so the third
// request turns the block over -- and the driver must be asked exactly three times in total, never a fourth
// time on a block that had no room.
TEST( ViewDescriptorPoolChain, ATurnoverAsksTheDriverNothingItWillRefuse )
{
    Chain                                  chain;
    std::shared_ptr<FakePool>              from;
    std::vector<std::shared_ptr<FakePool>> used;
    for ( uint32_t i = 0; i < 5; ++i )
    {
        auto allocated = chain.Allocate( Pools( 2 ), ReserveOne(), TakeOne, from );
        ASSERT_TRUE( allocated.IsSuccess() ) << allocated.GetError();
        ASSERT_NE( from, nullptr );
        EXPECT_EQ( from->Ordinal, i / 2 ) << "allocation " << i << " came from the wrong block";
        if ( std::find( used.begin(), used.end(), from ) == used.end() )
            used.push_back( from );
    }
    EXPECT_EQ( chain.PoolCount(), 3U );
    // Two, two and one: every attempt landed, so no block was ever allocated from while full.
    ASSERT_EQ( used.size(), 3U );
    EXPECT_EQ( used[0]->Attempts, 2 );
    EXPECT_EQ( used[1]->Attempts, 2 );
    EXPECT_EQ( used[2]->Attempts, 1 );
    for ( const auto& pool : used )
        EXPECT_EQ( pool->Budget.RemainingSets(), pool->Ordinal < 2 ? 0U : 1U );
}

// A full pool opens the next one and the allocation succeeds from it.
TEST( ViewDescriptorPoolChain, AFullPoolChainsToANewOne )
{
    Chain                     chain;
    std::shared_ptr<FakePool> from;
    for ( uint32_t i = 0; i < 5; ++i )
    {
        auto allocated = chain.Allocate( Pools( 2 ), ReserveOne(), TakeOne, from );
        ASSERT_TRUE( allocated.IsSuccess() ) << allocated.GetError();
        EXPECT_EQ( from->Ordinal, i / 2 );
    }
    EXPECT_EQ( chain.PoolCount(), 3U );
}

// A request that does not fit even a fresh pool is refused with the reason, instead of opening pools forever.
// It is now refused by the ACCOUNTING, so the driver is never asked at all.
TEST( ViewDescriptorPoolChain, ARequestThatNoPoolCanHoldIsRefusedNotLooped )
{
    Chain                     chain;
    std::shared_ptr<FakePool> from;
    auto                      allocated = chain.Allocate( Pools( 0 ), ReserveOne(), TakeOne, from );
    ASSERT_FALSE( allocated.IsSuccess() );
    EXPECT_NE( allocated.GetError().find( "no room left" ), std::string::npos ) << allocated.GetError();
    EXPECT_EQ( chain.PoolCount(), 1U );
}

// Any other failure stops at once: a new pool would fail the same way.
TEST( ViewDescriptorPoolChain, AHardFailureDoesNotOpenAnotherPool )
{
    Chain                     chain;
    std::shared_ptr<FakePool> from;
    auto                      allocated = chain.Allocate(
         Pools( 4 ), ReserveOne(),
         []( FakePool&, std::string& why )
         {
             why = "VK_ERROR_OUT_OF_DEVICE_MEMORY";
             return PoolAllocation::Failed;
         },
         from );
    ASSERT_FALSE( allocated.IsSuccess() );
    EXPECT_NE( allocated.GetError().find( "VK_ERROR_OUT_OF_DEVICE_MEMORY" ), std::string::npos )
         << allocated.GetError();
    EXPECT_EQ( chain.PoolCount(), 1U );
}

// A driver that refuses what our accounting reserved is a DEFECT in the block sizes, not a turnover, and the
// refusal has to say which of the two disagreed.
TEST( ViewDescriptorPoolChain, ADriverRefusingAReservedRequestIsNamedAsADisagreement )
{
    Chain                     chain;
    std::shared_ptr<FakePool> from;
    auto                      allocated = chain.Allocate(
         Pools( 4 ), ReserveOne(),
         []( FakePool&, std::string& why )
         {
             why = "VK_ERROR_OUT_OF_POOL_MEMORY";
             return PoolAllocation::PoolExhausted;
         },
         from );
    ASSERT_FALSE( allocated.IsSuccess() );
    EXPECT_NE( allocated.GetError().find( "refused it anyway" ), std::string::npos ) << allocated.GetError();
}
