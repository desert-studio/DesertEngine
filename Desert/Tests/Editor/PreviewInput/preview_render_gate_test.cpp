// WHEN A PREVIEW RENDERS (Editor/Widgets/PreviewRenderGate.hpp) and WHO OWNS THE WHEEL over it
// (Editor/Widgets/PreviewInput.hpp).

#include <Editor/Widgets/PreviewInput.hpp>
#include <Editor/Widgets/PreviewRenderGate.hpp>

#include <gtest/gtest.h>

#include <functional>
#include <vector>

namespace
{
    using namespace Desert::Editor;
    using namespace Desert::Editor::PreviewRenderGate;

    Inputs Base()
    {
        Inputs in;
        in.Subject = 11u;
        in.Width   = 640u;
        in.Height  = 480u;
        in.Zoom    = 1.0f;
        return in;
    }

    // Frames rendered from now until the gate goes idle on unchanged inputs.
    uint32_t RendersUntilIdle( State& s, const Inputs& in )
    {
        uint32_t n = 0;
        for ( int i = 0; i < 1000 && ShouldRender( s, in, false, false ); ++i )
            ++n;
        return n;
    }

    TEST( PreviewRenderGate, UnchangedInputsStopRenderingAfterSettling )
    {
        State s;
        EXPECT_EQ( RendersUntilIdle( s, Base() ), kSettleFrames + 1 );
        for ( int i = 0; i < 10; ++i )
            EXPECT_FALSE( ShouldRender( s, Base(), false, false ) );
    }

    TEST( PreviewRenderGate, EachInputChangeRendersAgain )
    {
        const std::vector<std::function<void( Inputs& )>> changes = {
             []( Inputs& in ) { in.Subject = 12u; },  []( Inputs& in ) { in.Parameters = 5u; },
             []( Inputs& in ) { in.Setup = 9u; },     []( Inputs& in ) { in.Yaw = 0.1f; },
             []( Inputs& in ) { in.Pitch = 0.2f; },   []( Inputs& in ) { in.Zoom = 1.5f; },
             []( Inputs& in ) { in.Focus.y = 3.0f; }, []( Inputs& in ) { in.Width = 700u; },
             []( Inputs& in ) { in.Height = 500u; },
        };
        for ( const auto& change : changes )
        {
            State s;
            RendersUntilIdle( s, Base() );
            Inputs next = Base();
            change( next );
            EXPECT_TRUE( ShouldRender( s, next, false, false ) );
            EXPECT_EQ( RendersUntilIdle( s, next ), kSettleFrames );
        }
    }

    TEST( PreviewRenderGate, RealtimeAndTimeDependentContentRenderEveryFrame )
    {
        State s;
        RendersUntilIdle( s, Base() );
        for ( int i = 0; i < 5; ++i )
        {
            EXPECT_TRUE( ShouldRender( s, Base(), false, true ) );
            EXPECT_TRUE( ShouldRender( s, Base(), true, false ) );
        }
        EXPECT_FALSE( ShouldRender( s, Base(), false, false ) );
    }

    TEST( PreviewRenderGate, FingerprintSeesAParameterChange )
    {
        const float a[] = { 1.0f, 0.02f };
        const float b[] = { 1.0f, 0.03f };
        EXPECT_NE( Fingerprint( a, sizeof( a ) ), Fingerprint( b, sizeof( b ) ) );
        EXPECT_EQ( Fingerprint( a, sizeof( a ) ), Fingerprint( a, sizeof( a ) ) );
    }

    // A material instance previewed while its PARENT is edited: the parent's values are part of what the
    // instance's pane draws, so they must move the instance's key (ME1f). The old key hashed the subject alone.
    struct ChainNode
    {
        float            Value  = 0.0f;
        const ChainNode* Parent = nullptr;
    };
    uint64_t KeyOf( const ChainNode& subject )
    {
        return ChainFingerprint(
             subject, 7u,
             []( const ChainNode& n, uint64_t seed ) { return Fingerprint( &n.Value, sizeof( n.Value ), seed ); },
             []( const ChainNode& n ) { return n.Parent; } );
    }

    TEST( PreviewRenderGate, ChildKeyMovesWhenAnyAncestorChanges )
    {
        ChainNode       base{ 0.5f };
        ChainNode       parent{ 1.0f, &base };
        const ChainNode child{ 2.0f, &parent };
        const uint64_t  before = KeyOf( child );
        EXPECT_EQ( KeyOf( child ), before );

        parent.Value               = 1.5f;
        const uint64_t afterParent = KeyOf( child );
        EXPECT_NE( afterParent, before );

        base.Value = 0.75f;
        EXPECT_NE( KeyOf( child ), afterParent );
    }

    TEST( PreviewRenderGate, CyclicChainStopsAtTheServiceDepth )
    {
        ChainNode       a{ 1.0f };
        const ChainNode b{ 2.0f, &a };
        a.Parent   = &b;
        int visits = 0;
        (void)ChainFingerprint(
             a, 0u,
             [&visits]( const ChainNode&, uint64_t seed )
             {
                 ++visits;
                 return seed;
             },
             []( const ChainNode& n ) { return n.Parent; } );
        EXPECT_EQ( visits, kMaxMaterialChain );
    }

    TEST( PreviewWheel, InteractiveHoveredClaimsTheWheelToZoom )
    {
        // Claimed on every hovered frame: the wheel's value is not an input (ImGui reads the claim next frame).
        EXPECT_EQ( WheelOwner( PreviewInteraction::Interactive, true, true ), PreviewWheelOwner::Zoom );
        EXPECT_EQ( WheelOwner( PreviewInteraction::Interactive, true, false ), PreviewWheelOwner::PassThrough );
    }

    TEST( PreviewWheel, StaticPreviewLetsTheDetailsPanelScroll )
    {
        EXPECT_EQ( WheelOwner( PreviewInteraction::Static, true, true ), PreviewWheelOwner::PassThrough );
        EXPECT_EQ( WheelOwner( PreviewInteraction::Static, true, false ), PreviewWheelOwner::PassThrough );
    }
} // namespace
