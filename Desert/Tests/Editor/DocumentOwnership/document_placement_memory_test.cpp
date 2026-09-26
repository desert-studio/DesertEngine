// WHERE A DOCUMENT KIND REOPENS: first time beside the level viewport; afterwards where the person last put
// it; a remembered node the layout no longer has falls back beside the level viewport WITH a report.

#include <Editor/Core/DocumentPlacement.hpp>

#include <gtest/gtest.h>

namespace
{
    using namespace Desert::Editor::DocumentPlacement;

    const glm::vec2 kWorkPos( 0.0f, 20.0f );
    const glm::vec2 kWorkSize( 1600.0f, 900.0f );

    TEST( DocumentPlacementMemory, FirstOpeningGoesBesideTheScene )
    {
        const Resolution r = Resolve( "Material Editor", nullptr, 7u, true, false, kWorkPos, kWorkSize );
        EXPECT_EQ( r.Place.DockId, 7u );
        EXPECT_TRUE( r.Report.empty() );
    }

    TEST( DocumentPlacementMemory, RememberedLiveNodeIsUsed )
    {
        const Remembered mem = Observe( 42u, 7u, glm::vec2( 0.0f ), glm::vec2( 0.0f ) );
        ASSERT_EQ( mem.At, Remembered::Where::DockNode );
        const Resolution r = Resolve( "Material Editor", &mem, 7u, true, true, kWorkPos, kWorkSize );
        EXPECT_EQ( r.Place.DockId, 42u );
        EXPECT_TRUE( r.Report.empty() );
    }

    TEST( DocumentPlacementMemory, DeadRememberedNodeFallsBackBesideTheSceneAndReports )
    {
        const Remembered mem{ Remembered::Where::DockNode, 42u, glm::vec2( 0.0f ), glm::vec2( 0.0f ) };
        const Resolution r = Resolve( "Material Editor", &mem, 7u, true, false, kWorkPos, kWorkSize );
        EXPECT_EQ( r.Place.DockId, 7u );
        EXPECT_NE( r.Report.find( "Material Editor" ), std::string::npos );
        EXPECT_NE( r.Report.find( "42" ), std::string::npos );
    }

    TEST( DocumentPlacementMemory, FloatingIsRememberedWithItsRect )
    {
        const Remembered mem = Observe( 0u, 7u, glm::vec2( 100.0f, 50.0f ), glm::vec2( 800.0f, 600.0f ) );
        const Resolution r   = Resolve( "Mesh Viewer", &mem, 7u, true, false, kWorkPos, kWorkSize );
        EXPECT_FALSE( r.Place.Docked() );
        EXPECT_EQ( r.Place.Pos, glm::vec2( 100.0f, 50.0f ) );
        EXPECT_EQ( r.Place.Size, glm::vec2( 800.0f, 600.0f ) );
        EXPECT_TRUE( r.Report.empty() );
    }

    TEST( DocumentPlacementMemory, LeftInTheSceneNodeIsRememberedAsBesideTheScene )
    {
        // A layout rebuilt under a new id must still read as "beside the level", not as a dead node.
        const Remembered mem = Observe( 7u, 7u, glm::vec2( 0.0f ), glm::vec2( 0.0f ) );
        EXPECT_EQ( mem.At, Remembered::Where::NextToScene );
        const Resolution r = Resolve( "Material Editor", &mem, 99u, true, false, kWorkPos, kWorkSize );
        EXPECT_EQ( r.Place.DockId, 99u );
        EXPECT_TRUE( r.Report.empty() );
    }

    TEST( DocumentPlacementMemory, IniValueRoundTripsAndRejectsGarbage )
    {
        const Remembered cases[] = {
             {},
             { Remembered::Where::DockNode, 3735928559u, glm::vec2( 0.0f ), glm::vec2( 0.0f ) },
             { Remembered::Where::Floating, 0u, glm::vec2( -40.0f, 12.0f ), glm::vec2( 640.0f, 480.0f ) },
        };
        for ( const Remembered& c : cases )
        {
            const auto back = Parse( Format( c ) );
            ASSERT_TRUE( back.has_value() ) << Format( c );
            EXPECT_EQ( *back, c ) << Format( c );
        }
        EXPECT_FALSE( Parse( "dock 0" ).has_value() );
        EXPECT_FALSE( Parse( "somewhere" ).has_value() );
    }
} // namespace
