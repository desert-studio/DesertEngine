// ParticleSortKey (VFX-08): the key the GPU sort orders a translucent sprite emitter by. Ascending keys must be
// back to front (far first), behind-the-camera depths last, and the padding past the alive count after all.

#include <Engine/Graphic/Systems/Scene/Particles/ParticleSortKey.hpp>

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace PS = Desert::Graphic::System;

TEST( ParticleSortKey, OrderedBitsKeepFloatOrder )
{
    const std::vector<float> ascending{ -1.0e6f, -250.5f, -1.0f, -0.0f, 0.0f, 0.25f, 1.0f, 99.0f, 1.0e6f };
    for ( std::size_t i = 1; i < ascending.size(); ++i )
        EXPECT_LE( PS::OrderedFloatBits( ascending[i - 1] ), PS::OrderedFloatBits( ascending[i] ) )
             << ascending[i - 1] << " vs " << ascending[i];
    EXPECT_LT( PS::OrderedFloatBits( -1.0f ), PS::OrderedFloatBits( 1.0f ) );
}

TEST( ParticleSortKey, AscendingKeysDrawFarBeforeNear )
{
    // view depths in cm; sorted ascending by key the order must be 5000, 1200, 300, 10, then -50 (behind).
    std::vector<float> depths{ 300.0f, -50.0f, 5000.0f, 10.0f, 1200.0f };
    std::sort( depths.begin(), depths.end(), []( const float a, const float b )
               { return PS::ParticleSortKey( a ) < PS::ParticleSortKey( b ); } );
    const std::vector<float> expected{ 5000.0f, 1200.0f, 300.0f, 10.0f, -50.0f };
    EXPECT_EQ( depths, expected );
}

TEST( ParticleSortKey, PaddingSortsAfterEveryParticle )
{
    for ( const float depth : { -1.0e30f, -1.0f, 0.0f, 1.0f, 1.0e30f } )
        EXPECT_LT( PS::ParticleSortKey( depth ), PS::kParticleSortPadKey ) << depth;
}

TEST( ParticleSortKey, SortLengthIsThePowerOfTwoCoveringTheRange )
{
    EXPECT_EQ( PS::ParticleSortLength( 0 ), 2u );
    EXPECT_EQ( PS::ParticleSortLength( 1 ), 2u );
    EXPECT_EQ( PS::ParticleSortLength( 2 ), 2u );
    EXPECT_EQ( PS::ParticleSortLength( 3 ), 4u );
    EXPECT_EQ( PS::ParticleSortLength( 2000 ), 2048u );
    EXPECT_EQ( PS::ParticleSortLength( 2048 ), 2048u );
    EXPECT_EQ( PS::ParticleSortLength( 100000 ), 131072u );
}

TEST( ParticleSortKey, StagesCoverTheWholeBitonicNetwork )
{
    using Kind = PS::ParticleSortStageKind;
    // N <= one block: keys, one shared-memory sort, write.
    const auto small = PS::ParticleSortStages( 256 );
    ASSERT_EQ( small.size(), 3u );
    EXPECT_EQ( small[0].Kind, Kind::Keys );
    EXPECT_EQ( small[1].Kind, Kind::Local );
    EXPECT_EQ( small[1].Groups, 1u );
    EXPECT_EQ( small[2].Kind, Kind::Write );

    // N = 4096: k = 2048 -> global j 1024, merge; k = 4096 -> global j 2048, 1024, merge.
    const auto big = PS::ParticleSortStages( 4096 );
    ASSERT_EQ( big.size(), 8u );
    EXPECT_EQ( big[1].Kind, Kind::Local );
    EXPECT_EQ( big[1].Groups, 4u );
    EXPECT_EQ( big[2].Kind, Kind::Global );
    EXPECT_EQ( big[2].K, 2048u );
    EXPECT_EQ( big[2].J, 1024u );
    EXPECT_EQ( big[2].Groups, 4u ); // 2048 pairs / 512 threads
    EXPECT_EQ( big[3].Kind, Kind::Merge );
    EXPECT_EQ( big[3].K, 2048u );
    EXPECT_EQ( big[4].J, 2048u );
    EXPECT_EQ( big[5].J, 1024u );
    EXPECT_EQ( big[6].Kind, Kind::Merge );
    EXPECT_EQ( big[6].K, 4096u );
    EXPECT_EQ( big[7].Kind, Kind::Write );

    // Every (k, j) of the network is run exactly once: shared-memory stages count their own.
    std::uint32_t exchanges = 0;
    for ( const auto& stage : PS::ParticleSortStages( 131072 ) )
    {
        if ( stage.Kind == Kind::Global )
            ++exchanges;
        else if ( stage.Kind == Kind::Local )
            exchanges += 55; // log2(1024) * (log2(1024) + 1) / 2
        else if ( stage.Kind == Kind::Merge )
            exchanges += 10; // j = 512 .. 1
    }
    EXPECT_EQ( exchanges, 17u * 18u / 2u );
}

// THE SORT DIRECTION under a real view (VFX-08f lead check "flip the sign if near draws over far"): a camera at a
// non-origin position looking along +X (glm::lookAt, the engine's -Z-forward view basis), two particles in front
// of it at 150 cm and 2400 cm and one 300 cm behind. The view the sort pushes (ParticleSortViewOf of InvView) must
// give those depths, and ascending keys must draw the far one first, the near one second and the one behind last.
// A flipped forward makes the depths negative and the order near-first: red here, not on screen.
TEST( ParticleSortKey, AFartherParticleSortsFirstUnderARealViewMatrix )
{
    const glm::vec3            eye( 1000.0f, 250.0f, -400.0f );
    const glm::vec3            look( 1.0f, 0.0f, 0.0f );
    const glm::mat4            viewMatrix = glm::lookAt( eye, eye + look, glm::vec3( 0.0f, 1.0f, 0.0f ) );
    const PS::ParticleSortView view       = PS::ParticleSortViewOf( glm::inverse( viewMatrix ) );

    EXPECT_NEAR( glm::dot( view.Forward, look ), 1.0f, 1.0e-5f )
         << "the sort's forward is the direction the camera looks";
    EXPECT_NEAR( glm::length( view.Origin - eye ), 0.0f, 1.0e-2f );

    const glm::vec3 nearParticle   = eye + look * 150.0f + glm::vec3( 0.0f, 40.0f, 0.0f );
    const glm::vec3 farParticle    = eye + look * 2400.0f - glm::vec3( 0.0f, 0.0f, 90.0f );
    const glm::vec3 behindParticle = eye - look * 300.0f;
    const float     nearDepth      = PS::ParticleSortDepth( view, nearParticle );
    const float     farDepth       = PS::ParticleSortDepth( view, farParticle );
    const float     behindDepth    = PS::ParticleSortDepth( view, behindParticle );
    EXPECT_NEAR( nearDepth, 150.0f, 1.0e-2f );
    EXPECT_NEAR( farDepth, 2400.0f, 1.0e-1f );
    EXPECT_NEAR( behindDepth, -300.0f, 1.0e-2f );

    EXPECT_LT( PS::ParticleSortKey( farDepth ), PS::ParticleSortKey( nearDepth ) ) << "far draws before near";
    EXPECT_LT( PS::ParticleSortKey( nearDepth ), PS::ParticleSortKey( behindDepth ) ) << "behind the camera last";
}
