// Every view owns its per-frame copies of shared GPU resources; the key is in the resource, the register in
// the view (Engine/Graphic/ViewResources.hpp). The properties pinned here are the ones whose failure is
// silent on screen: a reused key binds a dead resource's copy, a Forget that misses a view leaks until it
// closes, a destroyed view that keeps a copy leaks for the session, and a scope that does not restore on an
// exception leaves the UI writing into a view's copies — the implicit coupling the frame context replaces.

#include <Engine/Graphic/ViewResources.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

using Desert::Graphic::ActiveViewScope;
using Desert::Graphic::IViewResourceCopy;
using Desert::Graphic::IViewResourceCopyFactory;
using Desert::Graphic::ViewResourceKey;
using Desert::Graphic::ViewResourceRegistry;
using Desert::Graphic::ViewResources;

namespace
{
    // Counts live copies, standing in for the GPU objects a real copy hands to the deferred-deletion queue.
    int g_LiveCopies = 0;

    struct CountedCopy final : IViewResourceCopy
    {
        explicit CountedCopy( const uint64_t bytes ) : Bytes( bytes )
        {
            ++g_LiveCopies;
        }
        ~CountedCopy() override
        {
            --g_LiveCopies;
        }

        [[nodiscard]] uint64_t HeldBytes() const noexcept override
        {
            return Bytes;
        }

        uint64_t Bytes = 0;
    };

    struct CountingFactory final : IViewResourceCopyFactory
    {
        int      Created   = 0;
        uint64_t CopyBytes = 0; // what each copy made from here says it holds

        std::unique_ptr<IViewResourceCopy> CreateViewCopy( std::string_view, uint32_t ) override
        {
            ++Created;
            return std::make_unique<CountedCopy>( CopyBytes );
        }
    };

    struct NullFactory final : IViewResourceCopyFactory
    {
        std::unique_ptr<IViewResourceCopy> CreateViewCopy( std::string_view, uint32_t ) override
        {
            return nullptr;
        }
    };

    class ViewResourcesTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            ViewResourceRegistry::FrameContext().Clear();
            g_LiveCopies = 0;
        }
        void TearDown() override
        {
            ViewResourceRegistry::FrameContext().Clear();
        }
    };
} // namespace

TEST_F( ViewResourcesTest, KeysAreNeverRepeatedAndNeverZero )
{
    std::unordered_set<uint64_t> seen;
    for ( int i = 0; i < 10000; ++i )
    {
        const ViewResourceKey key = ViewResourceKey::Allocate();
        ASSERT_TRUE( key.IsValid() );
        ASSERT_TRUE( seen.insert( key.Value() ).second ) << "key " << key.Value() << " handed out twice";
    }
    EXPECT_FALSE( ViewResourceKey{}.IsValid() );
}

TEST_F( ViewResourcesTest, CopiesAreLazyPerViewAndPerFrame )
{
    const ViewResourceKey key = ViewResourceKey::Allocate();
    CountingFactory       factory;
    ViewResources         a( "a" );
    ViewResources         b( "b" );

    EXPECT_EQ( a.Find( key, 0 ), nullptr );
    const IViewResourceCopy& a0 = a.Acquire( key, 0, factory );
    EXPECT_EQ( &a.Acquire( key, 0, factory ), &a0 ) << "a second bind in the same frame made a second copy";
    EXPECT_NE( &a.Acquire( key, 1, factory ), &a0 ) << "two frames in flight shared one copy";
    EXPECT_NE( &b.Acquire( key, 0, factory ), &a0 ) << "two views shared one copy";
    EXPECT_EQ( factory.Created, 3 );
    EXPECT_EQ( a.CopyCount(), 2u );
    EXPECT_EQ( b.CopyCount(), 1u );
}

TEST_F( ViewResourcesTest, ForgetReachesEveryLiveViewAndTheFrameContext )
{
    const ViewResourceKey key   = ViewResourceKey::Allocate();
    const ViewResourceKey other = ViewResourceKey::Allocate();
    CountingFactory       factory;
    ViewResources         a( "a" );
    ViewResources         b( "b" );
    ViewResources&        frame = ViewResourceRegistry::FrameContext();

    (void)a.Acquire( key, 0, factory );
    (void)a.Acquire( key, 2, factory );
    (void)b.Acquire( key, 1, factory );
    (void)frame.Acquire( key, 0, factory );
    (void)b.Acquire( other, 0, factory );

    EXPECT_EQ( ViewResourceRegistry::Forget( key ), 4u );
    EXPECT_EQ( a.CopyCount(), 0u );
    EXPECT_EQ( b.CopyCount(), 1u ) << "Forget took a copy of a different resource";
    EXPECT_EQ( frame.CopyCount(), 0u );
    EXPECT_EQ( g_LiveCopies, 1 );
}

TEST_F( ViewResourcesTest, DestroyingAViewReleasesEveryCopyAndLeavesTheLiveList )
{
    CountingFactory factory;
    const uint32_t  before = ViewResourceRegistry::LiveCount();
    {
        ViewResources view( "preview" );
        EXPECT_EQ( ViewResourceRegistry::LiveCount(), before + 1 );
        for ( uint32_t frame = 0; frame < 3; ++frame )
            for ( int i = 0; i < 5; ++i )
                (void)view.Acquire( ViewResourceKey::Allocate(), frame, factory );
        EXPECT_EQ( g_LiveCopies, 15 );
    }
    EXPECT_EQ( g_LiveCopies, 0 );
    EXPECT_EQ( ViewResourceRegistry::LiveCount(), before );
}

TEST_F( ViewResourcesTest, TheFrameContextIsActiveOutsideEveryScope )
{
    ViewResources view( "main" );
    EXPECT_EQ( &ViewResourceRegistry::Active(), &ViewResourceRegistry::FrameContext() );
    {
        const ActiveViewScope scope( view );
        EXPECT_EQ( &ViewResourceRegistry::Active(), &view );
    }
    EXPECT_EQ( &ViewResourceRegistry::Active(), &ViewResourceRegistry::FrameContext() )
         << "the last view stayed active after its phase returned; the UI would write into its copies";
}

TEST_F( ViewResourcesTest, ScopeRestoresTheFrameContextOnException )
{
    ViewResources view( "main" );
    bool          unwound = false;
    try
    {
        const ActiveViewScope scope( view );
        throw std::runtime_error( "phase failed" );
    }
    catch ( const std::runtime_error& )
    {
        unwound = true;
    }
    ASSERT_TRUE( unwound );
    EXPECT_EQ( &ViewResourceRegistry::Active(), &ViewResourceRegistry::FrameContext() );
}

TEST_F( ViewResourcesTest, NestedScopesRestoreTheOuterView )
{
    ViewResources         outer( "outer" );
    ViewResources         inner( "inner" );
    const ActiveViewScope outerScope( outer );
    {
        const ActiveViewScope innerScope( inner );
        EXPECT_EQ( &ViewResourceRegistry::Active(), &inner );
    }
    EXPECT_EQ( &ViewResourceRegistry::Active(), &outer );
}

TEST_F( ViewResourcesTest, AViewDestroyedWhileActiveHandsBackToTheFrameContext )
{
    auto view = std::make_unique<ViewResources>( "closing" );
    {
        const ActiveViewScope scope( *view );
        view.reset();
        EXPECT_EQ( &ViewResourceRegistry::Active(), &ViewResourceRegistry::FrameContext() );
    }
    EXPECT_EQ( &ViewResourceRegistry::Active(), &ViewResourceRegistry::FrameContext() );
}

TEST_F( ViewResourcesTest, AcquireRefusesAnUnassignedKeyAndANullCopy )
{
    ViewResources   view( "main" );
    CountingFactory counting;
    NullFactory     none;
    EXPECT_THROW( (void)view.Acquire( ViewResourceKey{}, 0, counting ), std::invalid_argument );
    const ViewResourceKey key = ViewResourceKey::Allocate();
    EXPECT_THROW( (void)view.Acquire( key, 0, none ), std::logic_error );
    EXPECT_EQ( view.Find( key, 0 ), nullptr ) << "a refused copy was left behind as an empty entry";
    EXPECT_EQ( view.CopyCount(), 0u );
}

// The view budget counts a view's holding as its targets PLUS these copies (SceneRenderer::HeldBytes); a
// per-view uniform buffer a view never gives back must show up in the number the refusal prints.
TEST_F( ViewResourcesTest, HeldBytesSumsEveryCopyOverEveryFrameAndDropsWithIt )
{
    ViewResources         view( "scene" );
    ViewResources         other( "other" );
    const ViewResourceKey uniforms = ViewResourceKey::Allocate();
    const ViewResourceKey storage  = ViewResourceKey::Allocate();
    CountingFactory       small;
    small.CopyBytes = 256;
    CountingFactory large;
    large.CopyBytes = 4096;

    EXPECT_EQ( view.HeldBytes(), 0u );
    (void)view.Acquire( uniforms, 0, small );
    (void)view.Acquire( uniforms, 1, small );
    (void)view.Acquire( storage, 1, large );
    (void)other.Acquire( uniforms, 0, large );
    EXPECT_EQ( view.HeldBytes(), 256u + 256u + 4096u ) << "a frame in flight or a second resource went uncounted";
    EXPECT_EQ( other.HeldBytes(), 4096u ) << "one view's copies were counted as another's";

    EXPECT_EQ( view.Forget( storage ), 1u );
    EXPECT_EQ( view.HeldBytes(), 512u ) << "a dropped copy is still counted as held";
    view.Clear();
    EXPECT_EQ( view.HeldBytes(), 0u );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
