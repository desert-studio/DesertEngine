// Every view owns its resources, and every view that closes gives them back.
//
// A view (one SceneRenderer) keeps its per-frame GPU state in its own ViewResources (Engine/Graphic/
// ViewResources.hpp, Docs/RENDERER_FRAME_STATE.md). Before RT2j a view instead leased one of six renderer
// slots, and the seventh recorded into slot 0 — the main viewport's — and traded camera, lights and shadow
// cascades with it, a picture defect with no error message. That path is gone: there is no count ceiling
// and no shared fallback, only the byte budget the editor checks before creating a view
// (Engine/Core/ViewBudget.hpp, suite ViewBudget).
//
// So the properties under test are RELATIONS between opening and closing: the live count is exactly the
// number of views alive after any sequence of opens and closes, a view past the old six still gets copies
// of its own, and two live views never see each other's copies. Nothing here touches Vulkan: the register
// is Engine/Graphic/ViewResources.cpp, compiled into this suite.

#include <Engine/Graphic/ViewResources.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using Desert::Graphic::ActiveViewScope;
using Desert::Graphic::IViewResourceCopy;
using Desert::Graphic::IViewResourceCopyFactory;
using Desert::Graphic::ViewResourceKey;
using Desert::Graphic::ViewResourceRegistry;
using Desert::Graphic::ViewResources;

namespace
{
    // The view count the retired slot pool allowed; the tests open more than this on purpose.
    constexpr uint32_t kOldSlotCeiling = 6;

    using View = std::unique_ptr<ViewResources>;

    View Open( const std::string& name )
    {
        return std::make_unique<ViewResources>( name );
    }

    // A copy that remembers which view it was made for, so "whose copy is this" is an observable.
    class NamedCopy final : public IViewResourceCopy
    {
    public:
        explicit NamedCopy( std::string owner ) : Owner( std::move( owner ) )
        {
        }

        [[nodiscard]] uint64_t HeldBytes() const noexcept override
        {
            return 256;
        }

        std::string Owner;
    };

    class NamedCopyFactory final : public IViewResourceCopyFactory
    {
    public:
        std::unique_ptr<IViewResourceCopy> CreateViewCopy( std::string_view viewName, uint32_t ) override
        {
            ++Created;
            return std::make_unique<NamedCopy>( std::string( viewName ) );
        }

        uint32_t Created = 0;
    };

    class ViewLifetime : public testing::Test
    {
    protected:
        void SetUp() override
        {
            // The register is process-wide; every test starts and must end with no view alive.
            ASSERT_EQ( ViewResourceRegistry::LiveCount(), 0u ) << "an earlier test leaked a view";
        }
    };
} // namespace

TEST_F( ViewLifetime, OccupancyIsExactlyTheNumberOfLiveViews )
{
    std::vector<View> views;
    for ( uint32_t i = 0; i < 4; ++i )
    {
        views.push_back( Open( "view " + std::to_string( i ) ) );
        EXPECT_EQ( ViewResourceRegistry::LiveCount(), i + 1 );
    }
    while ( !views.empty() )
    {
        views.pop_back();
        EXPECT_EQ( ViewResourceRegistry::LiveCount(), views.size() );
    }
}

TEST_F( ViewLifetime, AViewPastTheOldSlotCeilingGetsCopiesOfItsOwn )
{
    // THE PATH RT2j REMOVED. The seventh view used to record into slot 0 and share the main viewport's
    // per-frame state; now it must hold a copy made for IT, distinct from the main viewport's.
    const ViewResourceKey key = ViewResourceKey::Allocate();
    NamedCopyFactory      factory;

    std::vector<View> views;
    for ( uint32_t i = 0; i <= kOldSlotCeiling; ++i )
        views.push_back( Open( "view " + std::to_string( i ) ) );
    ASSERT_EQ( ViewResourceRegistry::LiveCount(), kOldSlotCeiling + 1 );

    auto& mainCopy    = static_cast<NamedCopy&>( views.front()->Acquire( key, 0, factory ) );
    auto& seventhCopy = static_cast<NamedCopy&>( views.back()->Acquire( key, 0, factory ) );

    EXPECT_NE( &mainCopy, &seventhCopy ) << "the seventh view shares the main viewport's copy";
    EXPECT_EQ( mainCopy.Owner, "view 0" );
    EXPECT_EQ( seventhCopy.Owner, "view 6" ) << "the seventh view's copy was made for somebody else";
    EXPECT_EQ( factory.Created, 2u );
}

TEST_F( ViewLifetime, EveryLiveViewHoldsItsOwnCopyAndNoneSeesAnother )
{
    const ViewResourceKey key = ViewResourceKey::Allocate();
    NamedCopyFactory      factory;

    std::vector<View> views;
    for ( uint32_t i = 0; i < 2 * kOldSlotCeiling; ++i )
        views.push_back( Open( "view " + std::to_string( i ) ) );

    for ( const View& view : views )
        (void)view->Acquire( key, 0, factory );

    for ( const View& view : views )
    {
        const auto* copy = static_cast<const NamedCopy*>( view->Find( key, 0 ) );
        ASSERT_NE( copy, nullptr );
        EXPECT_EQ( copy->Owner, view->GetName() ) << "a view reads another view's copy";
    }
    EXPECT_EQ( factory.Created, 2 * kOldSlotCeiling );
}

TEST_F( ViewLifetime, TheActiveViewIsTheOneThatRecordsEvenPastTheOldCeiling )
{
    // What binding a renderer slot used to say, now said by the scope: inside a view's phase, per-frame
    // writes go to THAT view; after it, to the frame context — never to the main viewport by accident.
    std::vector<View> views;
    for ( uint32_t i = 0; i <= kOldSlotCeiling; ++i )
        views.push_back( Open( "view " + std::to_string( i ) ) );

    {
        const ActiveViewScope scope( *views.back() );
        EXPECT_EQ( &ViewResourceRegistry::Active(), views.back().get() );
    }
    EXPECT_EQ( &ViewResourceRegistry::Active(), &ViewResourceRegistry::FrameContext() );
}

TEST_F( ViewLifetime, EveryPreviewSurfaceOpenedAndClosedReturnsToTheBaseline )
{
    // The measurement a leak is judged by: open a transient surface, close it, and require the count to be
    // back at the baseline every time. A surface that keeps its view shows up on its FIRST cycle, not when
    // the budget runs out some minutes later.
    const View mainViewport = Open( "main" ); // never closed while the editor runs
    ASSERT_EQ( ViewResourceRegistry::LiveCount(), 1u );

    for ( int cycle = 0; cycle < 20; ++cycle )
    {
        {
            const View opened = Open( "preview " + std::to_string( cycle ) );
            EXPECT_EQ( ViewResourceRegistry::LiveCount(), 2u ) << "cycle " << cycle;
        }
        EXPECT_EQ( ViewResourceRegistry::LiveCount(), 1u )
             << "cycle " << cycle << ": the count did not return to the baseline after closing the surface";
    }
}

TEST_F( ViewLifetime, ClosingOutOfOrderStillReturnsEverything )
{
    // Panels are closed in whatever order the user closes them, not the order they were opened.
    std::vector<View> views;
    for ( uint32_t i = 0; i < kOldSlotCeiling; ++i )
        views.push_back( Open( "view " + std::to_string( i ) ) );

    views.erase( views.begin() + 2 );
    EXPECT_EQ( ViewResourceRegistry::LiveCount(), kOldSlotCeiling - 1 );
    views.erase( views.begin() );
    EXPECT_EQ( ViewResourceRegistry::LiveCount(), kOldSlotCeiling - 2 );

    views.push_back( Open( "reopened a" ) );
    views.push_back( Open( "reopened b" ) );
    views.push_back( Open( "past the old ceiling" ) );
    EXPECT_EQ( ViewResourceRegistry::LiveCount(), kOldSlotCeiling + 1 );

    views.clear();
    EXPECT_EQ( ViewResourceRegistry::LiveCount(), 0u );
}

TEST_F( ViewLifetime, AClosedViewTakesItsCopiesAndHeldBytesWithIt )
{
    const ViewResourceKey key = ViewResourceKey::Allocate();
    NamedCopyFactory      factory;

    const View main    = Open( "main" );
    View       preview = Open( "preview" );
    (void)main->Acquire( key, 0, factory );
    (void)preview->Acquire( key, 0, factory );
    (void)preview->Acquire( key, 1, factory );
    EXPECT_EQ( preview->HeldBytes(), 512u );

    preview.reset();
    EXPECT_EQ( ViewResourceRegistry::LiveCount(), 1u );
    EXPECT_EQ( main->CopyCount(), 1u ) << "closing the preview took the main viewport's copy with it";
    EXPECT_EQ( main->HeldBytes(), 256u );
}

// Only gtest is linked, not gtest_main — every suite in this tree brings its own entry point.
int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
