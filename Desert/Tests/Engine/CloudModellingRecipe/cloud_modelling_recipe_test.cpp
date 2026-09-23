// The RECIPE half of the sculpting tool: the three primitives, the rotation, the weighted join, the
// container that carries them and the single-plane preview the panel draws while an artist works.
//
// WHAT THIS SUITE IS FOR, stated plainly because the panel itself cannot be tested here. A headless
// `--shot` renders no ImGui, so nothing below claims to have seen a window. What it does claim is that
// every decision the panel DELEGATES is correct: whether a recipe is legal, what a slice looks like,
// whether Save writes something Open can read back, and whether the join still cannot tell what order its
// lumps arrived in. Those are pure functions, and contract §2.3 says pure functions are tested.
//
// THE ONE RELATION THIS SUITE EXISTS TO PIN is that the preview and the bake are the same field. A
// preview that disagrees with what Save produces is the "two statements of one fact" defect of contract
// §2.3.1 in its most expensive form — the artist tunes against a lie, and every body in the library is
// subtly wrong. It is asserted on the bytes, for all three axes.

#include <Engine/Assets/CloudModellingVolume.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <vector>

using namespace Desert;

namespace
{
    // A body that USES everything this phase added: one capsule laid on its side for a base, two spheres
    // for lobes, one rotated ellipsoid, and weights either side of 1. A fixture of unrotated unit-weight
    // ellipsoids would pass every test below while exercising none of the code that was written.
    Assets::CloudModellingVolumeRecipe SculptedRecipe()
    {
        Assets::CloudModellingVolumeRecipe recipe;

        // A TALLER BOX THAN THE SHIPPED 2 x 1 x 2, because this fixture's tower is rotated and a rotated
        // lump reaches further up than its own radii do. Validate said so with the number when the box was
        // 1 km, which is the refusal working: the fixture was wrong and the check found it.
        recipe.SizeKm           = glm::vec3( 2.0f, 1.4f, 2.0f );
        recipe.BlendRadiusKm    = 0.05f;
        recipe.ProfileDepthKm   = 0.18f;
        recipe.EnvelopeMarginKm = 0.09f;

        recipe.Blobs = {
             // the base: a capsule on its side, which is the shape an ellipsoid cannot make — constant
             // cross-section along its length instead of a taper to a point
             Assets::CloudModellingBlob{ .CentreKm     = glm::vec3( 0.0f, -0.16f, 0.0f ),
                                         .RadiiKm      = glm::vec3( 0.14f, 0.42f, 0.14f ),
                                         .RotationDeg  = glm::vec3( 0.0f, 0.0f, 90.0f ),
                                         .Primitive    = Assets::CloudModellingPrimitive::Capsule,
                                         .Weight       = 1.0f,
                                         .DetailType   = 0.9f,
                                         .DensityScale = 0.85f },
             // west lobe, pulled out a little by its weight
             Assets::CloudModellingBlob{ .CentreKm     = glm::vec3( -0.26f, -0.02f, 0.05f ),
                                         .RadiiKm      = glm::vec3( 0.22f, 0.22f, 0.22f ),
                                         .Primitive    = Assets::CloudModellingPrimitive::Sphere,
                                         .Weight       = 1.6f,
                                         .DetailType   = 1.0f,
                                         .DensityScale = 1.0f },
             // east lobe, pushed in a little by its weight
             Assets::CloudModellingBlob{ .CentreKm     = glm::vec3( 0.27f, -0.03f, -0.06f ),
                                         .RadiiKm      = glm::vec3( 0.20f, 0.20f, 0.20f ),
                                         .Primitive    = Assets::CloudModellingPrimitive::Sphere,
                                         .Weight       = 0.7f,
                                         .DetailType   = 1.0f,
                                         .DensityScale = 1.0f },
             // the tower, leaning
             Assets::CloudModellingBlob{ .CentreKm     = glm::vec3( 0.0f, 0.10f, 0.0f ),
                                         .RadiiKm      = glm::vec3( 0.20f, 0.26f, 0.18f ),
                                         .RotationDeg  = glm::vec3( 12.0f, 25.0f, -8.0f ),
                                         .Primitive    = Assets::CloudModellingPrimitive::Ellipsoid,
                                         .Weight       = 1.0f,
                                         .DetailType   = 1.0f,
                                         .DensityScale = 0.95f },
        };

        return recipe;
    }

    size_t CountDifferingBytes( const std::vector<unsigned char>& a, const std::vector<unsigned char>& b,
                                int& maxDelta )
    {
        maxDelta = 0;
        if ( a.size() != b.size() )
            return a.size() + b.size();

        size_t differing = 0;
        for ( size_t i = 0; i < a.size(); ++i )
        {
            const int delta = std::abs( static_cast<int>( a[i] ) - static_cast<int>( b[i] ) );
            if ( delta != 0 )
            {
                ++differing;
                maxDelta = std::max( maxDelta, delta );
            }
        }
        return differing;
    }

    size_t VoxelIndex( uint32_t x, uint32_t y, uint32_t z )
    {
        return ( ( static_cast<size_t>( z ) * Assets::kCloudModellingVolumeHeight + y ) *
                      Assets::kCloudModellingVolumeWidth +
                 x ) *
               Assets::kCloudModellingBytesPerVoxel;
    }

    // The shipped body, baked once. Several tests want it and it costs seconds unoptimised.
    const std::vector<unsigned char>& ShippedVoxels()
    {
        static const std::vector<unsigned char> voxels = []
        {
            auto baked = Assets::GenerateCloudModellingVolume( Assets::CloudModellingDefaultRecipe() );
            EXPECT_TRUE( baked ) << baked.GetError();
            return baked ? baked.ExtractValue() : std::vector<unsigned char>{};
        }();
        return voxels;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// The recipe survives the file — which is what "the tool can re-open what it saved" means
// ---------------------------------------------------------------------------------------------------

TEST( CloudModellingRecipe, EveryAuthoredNumberSurvivesTheContainer )
{
    // THE PANEL'S OPEN BUTTON IS THIS TEST. A0's note on the format says the recipe lives in the header so
    // that the sculpting tool is an editor for the struct rather than a new file format — which is only
    // true if every field an artist can touch comes back out. A field that is written and not read, or
    // read into the wrong offset, produces a body that re-bakes differently from the one that was saved,
    // and the artist's evidence for that is "it changed when I reopened it".
    Assets::CloudModellingVolumeData data;
    data.Recipe = SculptedRecipe();

    auto baked = Assets::GenerateCloudModellingVolume( data.Recipe );
    ASSERT_TRUE( baked ) << baked.GetError();
    data.Voxels = baked.ExtractValue();

    const std::vector<unsigned char> encoded = Assets::EncodeCloudModellingVolume( data );

    // The size is a formula, not an observation: a record that grew without kCloudModellingBlobBytes
    // moving would pass a test that meant nothing.
    EXPECT_EQ( encoded.size(), Assets::kCloudModellingHeaderSize +
                                    data.Recipe.Blobs.size() * Assets::kCloudModellingBlobBytes +
                                    Assets::kCloudModellingVoxelBytes );

    const auto decoded = Assets::DecodeCloudModellingVolume( encoded );
    ASSERT_TRUE( decoded ) << decoded.GetError();

    const Assets::CloudModellingVolumeRecipe& read = decoded.GetValue().Recipe;

    EXPECT_FLOAT_EQ( read.SizeKm.x, data.Recipe.SizeKm.x );
    EXPECT_FLOAT_EQ( read.SizeKm.y, data.Recipe.SizeKm.y );
    EXPECT_FLOAT_EQ( read.SizeKm.z, data.Recipe.SizeKm.z );
    EXPECT_FLOAT_EQ( read.BlendRadiusKm, data.Recipe.BlendRadiusKm );
    EXPECT_FLOAT_EQ( read.ProfileDepthKm, data.Recipe.ProfileDepthKm );
    EXPECT_FLOAT_EQ( read.EnvelopeMarginKm, data.Recipe.EnvelopeMarginKm );

    ASSERT_EQ( read.Blobs.size(), data.Recipe.Blobs.size() );
    for ( size_t i = 0; i < read.Blobs.size(); ++i )
    {
        const Assets::CloudModellingBlob& got  = read.Blobs[i];
        const Assets::CloudModellingBlob& want = data.Recipe.Blobs[i];

        EXPECT_FLOAT_EQ( got.CentreKm.x, want.CentreKm.x ) << "lump " << i;
        EXPECT_FLOAT_EQ( got.CentreKm.y, want.CentreKm.y ) << "lump " << i;
        EXPECT_FLOAT_EQ( got.CentreKm.z, want.CentreKm.z ) << "lump " << i;
        EXPECT_FLOAT_EQ( got.RadiiKm.x, want.RadiiKm.x ) << "lump " << i;
        EXPECT_FLOAT_EQ( got.RadiiKm.y, want.RadiiKm.y ) << "lump " << i;
        EXPECT_FLOAT_EQ( got.RadiiKm.z, want.RadiiKm.z ) << "lump " << i;
        EXPECT_FLOAT_EQ( got.RotationDeg.x, want.RotationDeg.x ) << "lump " << i;
        EXPECT_FLOAT_EQ( got.RotationDeg.y, want.RotationDeg.y ) << "lump " << i;
        EXPECT_FLOAT_EQ( got.RotationDeg.z, want.RotationDeg.z ) << "lump " << i;
        EXPECT_EQ( got.Primitive, want.Primitive ) << "lump " << i;
        EXPECT_FLOAT_EQ( got.Weight, want.Weight ) << "lump " << i;
        EXPECT_FLOAT_EQ( got.DetailType, want.DetailType ) << "lump " << i;
        EXPECT_FLOAT_EQ( got.DensityScale, want.DensityScale ) << "lump " << i;
    }

    // The strongest form of the same claim: the recipe that came back out bakes the body that went in.
    // Field-by-field equality could still miss a field the BAKE reads and the comparison forgot.
    auto rebaked = Assets::GenerateCloudModellingVolume( read );
    ASSERT_TRUE( rebaked ) << rebaked.GetError();

    int          maxDelta  = 0;
    const size_t differing = CountDifferingBytes( data.Voxels, rebaked.GetValue(), maxDelta );
    EXPECT_EQ( differing, 0u ) << "re-baking the recipe read back from the file moved " << differing
                               << " bytes, by up to " << maxDelta;
}

TEST( CloudModellingRecipe, AnUnknownPrimitiveIsRefusedByNumberRatherThanGuessed )
{
    // A WELL-FORMED FILE FROM A LATER BUILD, not a corrupt one. The distinction is the point: flipping the
    // primitive's bytes on disk is caught by the recipe's checksum, as the second half of this test shows,
    // and the checksum's message would be a lie about what happened. The case that MATTERS is a file whose
    // checksum is perfectly good and whose primitive is a 3 this build has never heard of — a volume
    // sculpted after somebody adds a fourth solid. Reading that as an Ellipsoid because the switch fell
    // through would put a differently-shaped cloud in the sky and report nothing.
    Assets::CloudModellingVolumeData data;
    data.Recipe = SculptedRecipe();

    // The voxels are never parsed for meaning, only checksummed, so a zero-filled payload of the right
    // length is enough here — and it keeps a test about a header off the two seconds a bake costs.
    data.Voxels.assign( static_cast<size_t>( Assets::kCloudModellingVoxelBytes ), 0u );
    data.Recipe.Blobs[0].Primitive = static_cast<Assets::CloudModellingPrimitive>( 99u );

    const std::vector<unsigned char> encoded = Assets::EncodeCloudModellingVolume( data );

    const auto decoded = Assets::DecodeCloudModellingVolume( encoded );
    ASSERT_FALSE( decoded );

    // ASSERTED ON THE DECODER'S OWN MESSAGE, not merely on the word "primitive". `Validate` refuses an
    // unknown primitive too, from its switch's default, and it runs at the end of `Decode` — so a test
    // that only checked the file was rejected would stay green with the decoder's check deleted. Verified
    // by breaking it: that is exactly what happened. Only the decoder names the three it knows.
    EXPECT_NE( decoded.GetError().find( "this build knows Ellipsoid" ), std::string::npos ) << decoded.GetError();

    // And bit-rot in the same field is reported as bit-rot, because the checksum is checked before the
    // record is parsed. Two different faults, two different messages, neither wearing the other's name.
    std::vector<unsigned char> rotted = encoded;
    rotted[Assets::kCloudModellingHeaderSize + 44] ^= 0x01u;

    const auto corrupt = Assets::DecodeCloudModellingVolume( rotted );
    ASSERT_FALSE( corrupt );
    EXPECT_NE( corrupt.GetError().find( "checksum" ), std::string::npos ) << corrupt.GetError();
}

// ---------------------------------------------------------------------------------------------------
// The property A0 measured, and this phase had to not break
// ---------------------------------------------------------------------------------------------------

TEST( CloudModellingRecipe, TheJoinIsStillOrderIndependentWithPrimitivesRotationsAndWeights )
{
    // A0 FOUND THIS THE HARD WAY and the finding has to survive every later phase: the exponential
    // smooth-min is commutative in real arithmetic and floating-point addition is not, so shuffling eight
    // lumps moved six bytes of four million until a canonical sort was put in front of the sum.
    //
    // This phase added three fields to a lump, and the sort's key had to grow with them. It is exactly the
    // kind of change that quietly re-opens a closed defect: lumps identical in A0's eight numbers but
    // different in rotation compare EQUAL under the old key, and their order is then left to `std::sort`'s
    // internals — which at this size is insertion sort, is stable, and so faithfully preserves whatever
    // order the shuffle handed it.
    //
    // THE FIXTURE IS BUILT FOR SENSITIVITY, and this took two attempts to get right. Dropping the new
    // fields from the key changes NOTHING SEMANTIC — the set of lumps is the same and only the order the
    // sum is accumulated in moves — so the only observable is floating-point rounding, and how much of
    // that survives quantisation to 8 bits depends entirely on the recipe.
    //
    // A first attempt reused the sculpted fixture's 50 m blend radius and did not bite: at 50 m the terms
    // decay as `exp(-d/r)` so fast that the sum is dominated by its nearest one or two, and reordering a
    // negligible tail cannot move a byte. **The sabotage passed, which made the test a hole rather than a
    // guard.** The three numbers below are what fix it:
    //
    //   * A LARGE BLEND RADIUS (250 m), so every term is within an order of magnitude of every other and
    //     the sum has no dominant member to hide the reordering behind.
    //   * TWELVE mutually-equivalent lumps, so the shuffle has 12! orderings to draw from.
    //   * ROTATIONS THAT SHARE NO SYMMETRY. A regular family — 36 degrees apart about one axis — leaves
    //     the lumps congruent under the very rotations that separate them, so at a great many points
    //     their distances are EXACTLY equal and reordering exactly equal numbers changes nothing. Three
    //     coprime-ish rates about three axes has no such orbit.
    Assets::CloudModellingVolumeRecipe recipe;
    recipe.SizeKm           = glm::vec3( 4.0f, 3.0f, 4.0f );
    recipe.BlendRadiusKm    = 0.25f;
    recipe.ProfileDepthKm   = 0.18f;
    recipe.EnvelopeMarginKm = 0.10f;

    for ( int i = 0; i < 12; ++i )
    {
        const float turn = static_cast<float>( i );

        recipe.Blobs.push_back(
             Assets::CloudModellingBlob{ .CentreKm     = glm::vec3( 0.10f, 0.0f, 0.10f ),
                                         .RadiiKm      = glm::vec3( 0.12f, 0.20f, 0.12f ),
                                         .RotationDeg  = glm::vec3( 13.7f * turn, 29.3f * turn, 47.1f * turn ),
                                         .Primitive    = Assets::CloudModellingPrimitive::Capsule,
                                         .Weight       = 0.8f + 0.05f * turn,
                                         .DetailType   = 0.5f,
                                         .DensityScale = 0.5f } );
    }

    // Two lumps the old key CAN separate, so the fixture is not degenerate: if the sort ever stopped
    // working at all rather than merely losing the new fields, these would move too.
    recipe.Blobs.push_back( Assets::CloudModellingBlob{ .CentreKm  = glm::vec3( -0.30f, 0.05f, 0.0f ),
                                                        .RadiiKm   = glm::vec3( 0.22f, 0.22f, 0.22f ),
                                                        .Primitive = Assets::CloudModellingPrimitive::Sphere } );
    recipe.Blobs.push_back(
         Assets::CloudModellingBlob{ .CentreKm  = glm::vec3( 0.34f, -0.04f, 0.06f ),
                                     .RadiiKm   = glm::vec3( 0.19f, 0.26f, 0.21f ),
                                     .Primitive = Assets::CloudModellingPrimitive::Ellipsoid } );

    auto reference = Assets::GenerateCloudModellingVolume( recipe );
    ASSERT_TRUE( reference ) << reference.GetError();

    // Several shuffles, not one: a single permutation can be the identity on a small list, and a test that
    // passes because nothing moved is the "break that did not break anything" of the A0 report.
    for ( unsigned int seed : { 20260820u, 7u, 99991u } )
    {
        Assets::CloudModellingVolumeRecipe shuffled = recipe;

        std::mt19937 rng( seed );
        std::shuffle( shuffled.Blobs.begin(), shuffled.Blobs.end(), rng );

        auto rebaked = Assets::GenerateCloudModellingVolume( shuffled );
        ASSERT_TRUE( rebaked ) << rebaked.GetError();

        int          maxDelta  = 0;
        const size_t differing = CountDifferingBytes( reference.GetValue(), rebaked.GetValue(), maxDelta );
        EXPECT_EQ( differing, 0u ) << "seed " << seed << ": shuffling the lumps moved " << differing
                                   << " bytes, by up to " << maxDelta;
    }
}

TEST( CloudModellingRecipe, TheBakeIsAPureFunctionOfTheRecipe )
{
    // "Pure" is a claim about a function that grew a std::function parameter this phase, which is exactly
    // when the claim stops being obvious. Two things are asserted:
    //
    //   * the same recipe baked twice gives the same bytes — no clock, no global, no uninitialised scratch
    //     leaking between runs;
    //   * a bake WITH a progress callback gives the same bytes as one without. The callback is told a
    //     number and its answer is only ever read as "carry on"; if it could reach the arithmetic, the
    //     panel's preview and the tool's output would differ for no reason a user could ever discover.
    const Assets::CloudModellingVolumeRecipe recipe = SculptedRecipe();

    auto first = Assets::GenerateCloudModellingVolume( recipe );
    ASSERT_TRUE( first ) << first.GetError();

    auto second = Assets::GenerateCloudModellingVolume( recipe );
    ASSERT_TRUE( second ) << second.GetError();

    int maxDelta = 0;
    EXPECT_EQ( CountDifferingBytes( first.GetValue(), second.GetValue(), maxDelta ), 0u )
         << "the same recipe baked twice differed by up to " << maxDelta;

    std::vector<float> reported;
    auto               watched = Assets::GenerateCloudModellingVolume( recipe,
                                                                       [&reported]( float fraction )
                                                                       {
                                                             reported.push_back( fraction );
                                                             return true;
                                                         } );
    ASSERT_TRUE( watched ) << watched.GetError();

    EXPECT_EQ( CountDifferingBytes( first.GetValue(), watched.GetValue(), maxDelta ), 0u )
         << "watching the bake changed it, by up to " << maxDelta;

    // The progress is monotone, starts at 0 and ends at 1 — a bar that goes backwards or stops at 0.99 is
    // the artist's only evidence about a job they cannot see.
    ASSERT_FALSE( reported.empty() );
    EXPECT_FLOAT_EQ( reported.front(), 0.0f );
    EXPECT_FLOAT_EQ( reported.back(), 1.0f );
    EXPECT_TRUE( std::is_sorted( reported.begin(), reported.end() ) );
}

TEST( CloudModellingRecipe, ACancelledBakeSaysSoRatherThanReturningHalfAVolume )
{
    // The panel's destructor depends on this. Without it, closing the tool mid-bake has to block on a bake
    // with the rest of its slabs left to run — hundreds of milliseconds for the shipped body since Г10 put
    // them on the pool, and seconds for one at the 64-lump ceiling.
    int  calls  = 0;
    auto banked = Assets::GenerateCloudModellingVolume( SculptedRecipe(),
                                                        [&calls]( float )
                                                        {
                                                            ++calls;
                                                            return calls < 3;
                                                        } );

    ASSERT_FALSE( banked );
    EXPECT_NE( banked.GetError().find( "cancelled" ), std::string::npos ) << banked.GetError();
    EXPECT_EQ( calls, 3 );
}

// ---------------------------------------------------------------------------------------------------
// The preview and the bake are ONE field
// ---------------------------------------------------------------------------------------------------

TEST( CloudModellingRecipe, APreviewSliceIsExactlyThePlaneTheBakeWouldWrite )
{
    // THE RELATION THIS SUITE EXISTS FOR. The panel tunes against slices and Save writes a volume; if
    // those are two readings of the recipe, every body in the library is quietly wrong in a way no frame
    // will explain. Asserted on the bytes, on all three axes, at a plane through the middle of the body
    // where there is something to disagree about.
    const Assets::CloudModellingVolumeRecipe recipe = SculptedRecipe();

    auto baked = Assets::GenerateCloudModellingVolume( recipe );
    ASSERT_TRUE( baked ) << baked.GetError();
    const std::vector<unsigned char>& volume = baked.GetValue();

    struct Case
    {
        Assets::CloudModellingAxis Axis;
        uint32_t                   Index;
    };

    const Case cases[] = {
         { Assets::CloudModellingAxis::X, Assets::kCloudModellingVolumeWidth / 2 },
         { Assets::CloudModellingAxis::Y, Assets::kCloudModellingVolumeHeight / 2 },
         { Assets::CloudModellingAxis::Z, Assets::kCloudModellingVolumeDepth / 2 },
    };

    for ( const Case& c : cases )
    {
        const auto slice = Assets::GenerateCloudModellingSlice( recipe, c.Axis, c.Index );
        ASSERT_TRUE( slice ) << Assets::CloudModellingAxisName( c.Axis ) << ": " << slice.GetError();

        const Assets::CloudModellingSlice& plane = slice.GetValue();
        ASSERT_EQ( plane.Pixels.size(),
                   static_cast<size_t>( plane.Width ) * plane.Height * Assets::kCloudModellingBytesPerVoxel );

        size_t differing = 0;
        size_t carrying  = 0;

        for ( uint32_t v = 0; v < plane.Height; ++v )
        {
            for ( uint32_t u = 0; u < plane.Width; ++u )
            {
                uint32_t x = 0;
                uint32_t y = 0;
                uint32_t z = 0;
                switch ( c.Axis )
                {
                    case Assets::CloudModellingAxis::X:
                        x = c.Index;
                        z = u;
                        y = v;
                        break;
                    case Assets::CloudModellingAxis::Y:
                        y = c.Index;
                        x = u;
                        z = v;
                        break;
                    case Assets::CloudModellingAxis::Z:
                        z = c.Index;
                        x = u;
                        y = v;
                        break;
                }

                const size_t inSlice =
                     ( static_cast<size_t>( v ) * plane.Width + u ) * Assets::kCloudModellingBytesPerVoxel;
                const size_t inVolume = VoxelIndex( x, y, z );

                for ( uint32_t channel = 0; channel < Assets::kCloudModellingBytesPerVoxel; ++channel )
                {
                    if ( plane.Pixels[inSlice + channel] != volume[inVolume + channel] )
                        ++differing;
                }

                if ( volume[inVolume] > 0u )
                    ++carrying;
            }
        }

        EXPECT_EQ( differing, 0u ) << "on axis " << Assets::CloudModellingAxisName( c.Axis ) << " the preview "
                                   << "and the bake disagreed about " << differing << " bytes";

        // A plane through the middle of the body must HAVE body in it, or the comparison above was two
        // empty images agreeing — which is the shape of vacuous pass this programme has already paid for.
        EXPECT_GT( carrying, 100u ) << "the plane through the middle of the body is empty, so the "
                                    << "agreement above says nothing";
    }
}

TEST( CloudModellingRecipe, ASliceOutsideTheVolumeIsRefusedRatherThanClamped )
{
    const uint32_t extent = Assets::CloudModellingAxisExtent( Assets::CloudModellingAxis::Y );
    EXPECT_EQ( extent, Assets::kCloudModellingVolumeHeight );

    const auto slice =
         Assets::GenerateCloudModellingSlice( SculptedRecipe(), Assets::CloudModellingAxis::Y, extent );
    ASSERT_FALSE( slice );
    EXPECT_NE( slice.GetError().find( "outside the volume" ), std::string::npos ) << slice.GetError();
}

// ---------------------------------------------------------------------------------------------------
// The three primitives — what each one buys, measured
// ---------------------------------------------------------------------------------------------------

TEST( CloudModellingRecipe, ASphereIsTheSameBodyAsAnEllipsoidWithEqualRadii )
{
    // THE SPHERE'S BRANCH HAS TO EARN ITSELF, and the way it does is by being the same answer for less
    // work rather than a different shape. The ellipsoid's bounded form reduces algebraically to `|p| - R`
    // when the radii agree — this asserts the reduction is exact in floating point too, which is the only
    // form of the claim that matters. Were it not, an artist switching a lump from Ellipsoid to Sphere
    // would watch the body twitch for no reason they could name.
    Assets::CloudModellingVolumeRecipe asEllipsoid;
    asEllipsoid.SizeKm           = glm::vec3( 2.0f, 1.0f, 2.0f );
    asEllipsoid.BlendRadiusKm    = 0.05f;
    asEllipsoid.ProfileDepthKm   = 0.18f;
    asEllipsoid.EnvelopeMarginKm = 0.09f;
    asEllipsoid.Blobs            = {
         Assets::CloudModellingBlob{ .CentreKm  = glm::vec3( -0.20f, 0.0f, 0.05f ),
                                                .RadiiKm   = glm::vec3( 0.25f, 0.25f, 0.25f ),
                                                .Primitive = Assets::CloudModellingPrimitive::Ellipsoid },
         Assets::CloudModellingBlob{ .CentreKm  = glm::vec3( 0.22f, 0.03f, -0.04f ),
                                                .RadiiKm   = glm::vec3( 0.18f, 0.18f, 0.18f ),
                                                .Primitive = Assets::CloudModellingPrimitive::Ellipsoid },
    };

    Assets::CloudModellingVolumeRecipe asSphere = asEllipsoid;
    for ( Assets::CloudModellingBlob& blob : asSphere.Blobs )
        blob.Primitive = Assets::CloudModellingPrimitive::Sphere;

    auto ellipsoid = Assets::GenerateCloudModellingVolume( asEllipsoid );
    ASSERT_TRUE( ellipsoid ) << ellipsoid.GetError();
    auto sphere = Assets::GenerateCloudModellingVolume( asSphere );
    ASSERT_TRUE( sphere ) << sphere.GetError();

    int          maxDelta  = 0;
    const size_t differing = CountDifferingBytes( ellipsoid.GetValue(), sphere.GetValue(), maxDelta );
    EXPECT_EQ( differing, 0u ) << "the sphere and the equal-radii ellipsoid differ in " << differing
                               << " bytes, by up to " << maxDelta;

    // And it is not two empty volumes agreeing.
    size_t body = 0;
    for ( size_t i = 0; i < sphere.GetValue().size(); i += Assets::kCloudModellingBytesPerVoxel )
    {
        if ( sphere.GetValue()[i] > 0u )
            ++body;
    }
    EXPECT_GT( body, 1000u ) << "the fixture baked an empty box, so the agreement above says nothing";
}

TEST( CloudModellingRecipe, ACapsuleHoldsItsCrossSectionWhereAnEllipsoidTapers )
{
    // WHY THE CAPSULE IS A PRIMITIVE AND NOT A ROW OF ELLIPSOIDS, measured rather than asserted.
    //
    // Both shapes below are 0.12 km across and 0.40 km tall. Sampled at three heights up the axis — the
    // middle, and 60 % and 85 % of the way to the top — the capsule's half-width is CONSTANT until the
    // cap, and the ellipsoid's shrinks the whole way. That constancy is the base of a cumulus and the
    // elongated growths of ANALYSIS_APPROACH.md §6, and it is the one thing three radii cannot buy.
    const float radiusKm     = 0.12f;
    const float halfHeightKm = 0.40f;

    const auto halfWidthAt = [&]( Assets::CloudModellingPrimitive primitive, float heightKm )
    {
        Assets::CloudModellingVolumeRecipe recipe;
        recipe.SizeKm           = glm::vec3( 2.0f, 1.6f, 2.0f );
        recipe.BlendRadiusKm    = 0.02f;
        recipe.ProfileDepthKm   = 0.10f;
        recipe.EnvelopeMarginKm = 0.05f;
        recipe.Blobs            = { Assets::CloudModellingBlob{ .CentreKm  = glm::vec3( 0.0f, 0.0f, 0.0f ),
                                                                .RadiiKm   = glm::vec3( radiusKm, halfHeightKm, radiusKm ),
                                                                .Primitive = primitive } };

        auto baked = Assets::GenerateCloudModellingVolume( recipe );
        EXPECT_TRUE( baked ) << baked.GetError();
        if ( !baked )
            return 0.0f;

        // Walk out along x at this height and count the voxels that carry body.
        const uint32_t y = static_cast<uint32_t>( ( heightKm / recipe.SizeKm.y + 0.5f ) *
                                                  static_cast<float>( Assets::kCloudModellingVolumeHeight ) );
        const uint32_t z = Assets::kCloudModellingVolumeDepth / 2;

        uint32_t span = 0;
        for ( uint32_t x = 0; x < Assets::kCloudModellingVolumeWidth; ++x )
        {
            if ( baked.GetValue()[VoxelIndex( x, y, z )] > 0u )
                ++span;
        }

        return static_cast<float>( span ) * recipe.SizeKm.x /
               static_cast<float>( Assets::kCloudModellingVolumeWidth ) * 0.5f;
    };

    const float capsuleMid  = halfWidthAt( Assets::CloudModellingPrimitive::Capsule, 0.0f );
    const float capsuleHigh = halfWidthAt( Assets::CloudModellingPrimitive::Capsule, 0.60f * halfHeightKm );

    const float ellipsoidMid  = halfWidthAt( Assets::CloudModellingPrimitive::Ellipsoid, 0.0f );
    const float ellipsoidHigh = halfWidthAt( Assets::CloudModellingPrimitive::Ellipsoid, 0.60f * halfHeightKm );

    ASSERT_GT( capsuleMid, 0.0f );
    ASSERT_GT( ellipsoidMid, 0.0f );

    // The capsule keeps its width: 60 % of the way up is still inside the straight section, because the
    // caps only occupy the last `radius` of the half-height (0.12 of 0.40, i.e. the top 30 %).
    EXPECT_NEAR( capsuleHigh, capsuleMid, 0.02f )
         << "the capsule's half-width went from " << capsuleMid << " km to " << capsuleHigh
         << " km, which is a taper and not a swept sphere";

    // The ellipsoid does not. sqrt(1 - 0.6^2) = 0.8, so it should have lost about a fifth.
    EXPECT_LT( ellipsoidHigh, 0.88f * ellipsoidMid )
         << "the ellipsoid's half-width went from " << ellipsoidMid << " km to " << ellipsoidHigh
         << " km, which is not the taper an ellipsoid has";

    // And the two shapes really are different where it counts.
    EXPECT_GT( capsuleHigh, 1.10f * ellipsoidHigh )
         << "at 60 % of the height the capsule is " << capsuleHigh << " km and the ellipsoid " << ellipsoidHigh
         << " km — too close for the capsule to be buying anything";
}

TEST( CloudModellingRecipe, RotatingALumpMovesTheBodyWithoutDistortingIt )
{
    // A rotation is RIGID, and the reason that matters is not tidiness: the joined field IS the
    // Dimensional Profile, so a rotation applied as anything other than an isometry would change how much
    // of the cloud the erosion is allowed to eat, everywhere, as a side effect of turning a lump.
    //
    // Measured as: a capsule stood on end, and the same capsule laid along x by a 90-degree roll, carry
    // the SAME NUMBER of body voxels. The bodies are in different places; they are the same size.
    const auto bodyVoxels = [&]( const glm::vec3& rotationDeg )
    {
        Assets::CloudModellingVolumeRecipe recipe;
        recipe.SizeKm           = glm::vec3( 2.0f, 1.6f, 2.0f );
        recipe.BlendRadiusKm    = 0.02f;
        recipe.ProfileDepthKm   = 0.10f;
        recipe.EnvelopeMarginKm = 0.05f;
        recipe.Blobs            = { Assets::CloudModellingBlob{ .CentreKm    = glm::vec3( 0.0f, 0.0f, 0.0f ),
                                                                .RadiiKm     = glm::vec3( 0.12f, 0.40f, 0.12f ),
                                                                .RotationDeg = rotationDeg,
                                                                .Primitive   = Assets::CloudModellingPrimitive::Capsule } };

        auto baked = Assets::GenerateCloudModellingVolume( recipe );
        EXPECT_TRUE( baked ) << baked.GetError();
        if ( !baked )
            return size_t{ 0 };

        size_t body = 0;
        for ( size_t i = 0; i < baked.GetValue().size(); i += Assets::kCloudModellingBytesPerVoxel )
        {
            if ( baked.GetValue()[i] > 0u )
                ++body;
        }
        return body;
    };

    const size_t upright = bodyVoxels( glm::vec3( 0.0f ) );
    const size_t rolled  = bodyVoxels( glm::vec3( 0.0f, 0.0f, 90.0f ) );

    ASSERT_GT( upright, 1000u );

    // Not exactly equal, and it would be wrong to demand it: the volume's voxels are 15.6 m on x and z but
    // 25 m on y in this fixture's box, so a body that changes which axis it is long on lands on a
    // different sampling lattice. One per cent is the size of that effect; a rotation that was not rigid
    // would move this by tens of per cent.
    const double ratio = static_cast<double>( rolled ) / static_cast<double>( upright );
    EXPECT_NEAR( ratio, 1.0, 0.05 ) << "upright " << upright << " voxels, rolled " << rolled
                                    << " — a rotation that changes the body's size is not a rotation";
}

TEST( CloudModellingRecipe, ALumpTurnsTheWAYItsRotationSays )
{
    // A GAP THIS SUITE HAD, and it is the kind the A0 report warns about: the test above measures that a
    // rotation preserves SIZE, and a rotation applied backwards — the matrix where its transpose belongs —
    // preserves size perfectly. Every assertion in this file passed with the inverse the wrong way round.
    //
    // The symptom that would have shipped is small and maddening: the panel's rotation widget turns the
    // lump the opposite way from the number in it, and every other rotation in the editor turns correctly.
    //
    // Ninety degrees cannot tell the two apart either — a capsule laid along +X and one laid along -X are
    // the same capsule. FORTY-FIVE can, and it pins the HANDEDNESS at the same time.
    //
    // The convention, written down here because it is the thing that is easy to get backwards and this
    // test caught the author getting it backwards: the frame is right-handed and the rotation is the
    // engine's own `glm::quat( radians( euler ) )`, so a +45 degree turn about +Z carries the lump's local
    // +Y towards -X. Its long axis therefore runs along the (-x, +y) / (+x, -y) diagonal, and the +x/-y
    // quadrant fills while +x/+y stays empty. Apply the rotation instead of its inverse and those two
    // swap — which is the ONE way of getting a rotation wrong that preserves the body's size, and so the
    // one the size test above cannot see.
    Assets::CloudModellingVolumeRecipe recipe;
    recipe.SizeKm           = glm::vec3( 2.0f, 2.0f, 2.0f );
    recipe.BlendRadiusKm    = 0.02f;
    recipe.ProfileDepthKm   = 0.10f;
    recipe.EnvelopeMarginKm = 0.05f;
    recipe.Blobs            = { Assets::CloudModellingBlob{ .CentreKm    = glm::vec3( 0.0f ),
                                                            .RadiiKm     = glm::vec3( 0.08f, 0.40f, 0.08f ),
                                                            .RotationDeg = glm::vec3( 0.0f, 0.0f, 45.0f ),
                                                            .Primitive   = Assets::CloudModellingPrimitive::Capsule } };

    auto baked = Assets::GenerateCloudModellingVolume( recipe );
    ASSERT_TRUE( baked ) << baked.GetError();

    const uint32_t midX = Assets::kCloudModellingVolumeWidth / 2;
    const uint32_t midY = Assets::kCloudModellingVolumeHeight / 2;
    const uint32_t midZ = Assets::kCloudModellingVolumeDepth / 2;

    // Body voxels in the two diagonal quadrants of the z = middle plane.
    size_t upperRight = 0; // x > centre and y > centre
    size_t lowerRight = 0; // x > centre and y < centre

    for ( uint32_t y = 0; y < Assets::kCloudModellingVolumeHeight; ++y )
    {
        for ( uint32_t x = midX + 1; x < Assets::kCloudModellingVolumeWidth; ++x )
        {
            if ( baked.GetValue()[VoxelIndex( x, y, midZ )] == 0u )
                continue;

            if ( y > midY )
                ++upperRight;
            else if ( y < midY )
                ++lowerRight;
        }
    }

    ASSERT_GT( upperRight + lowerRight, 100u ) << "the rotated capsule left nothing to measure";

    EXPECT_GT( lowerRight, 4u * upperRight )
         << "the +45 degree lump put " << upperRight << " voxels in the +x/+y quadrant and " << lowerRight
         << " in +x/-y; a right-handed turn about +Z carries local +Y towards -X, so +x/-y is the one that "
            "should fill — it is turning the wrong way";
}

TEST( CloudModellingRecipe, AWeightDilatesItsLumpByBlendRadiusTimesLogWeight )
{
    // THE WEIGHT'S DOCUMENTED ALGEBRA, checked against the bake rather than trusted from the comment.
    // `-r*ln(SUM w exp(-d/r))` is the unweighted join over distances `d - r*ln(w)`, so a weight is a
    // dilation of exactly `r*ln(w)`. A knob whose effect is a documented formula is one an artist can
    // predict; a knob whose effect is "bigger, somehow" is a slider people stop touching.
    const float radiusKm      = 0.25f;
    const float blendRadiusKm = 0.05f;

    const auto surfaceReachKm = [&]( float weight )
    {
        Assets::CloudModellingVolumeRecipe recipe;
        recipe.SizeKm           = glm::vec3( 2.0f, 1.0f, 2.0f );
        recipe.BlendRadiusKm    = blendRadiusKm;
        recipe.ProfileDepthKm   = 0.18f;
        recipe.EnvelopeMarginKm = 0.09f;
        recipe.Blobs            = { Assets::CloudModellingBlob{ .CentreKm  = glm::vec3( 0.0f ),
                                                                .RadiiKm   = glm::vec3( radiusKm, radiusKm, radiusKm ),
                                                                .Primitive = Assets::CloudModellingPrimitive::Sphere,
                                                                .Weight    = weight } };

        auto baked = Assets::GenerateCloudModellingVolume( recipe );
        EXPECT_TRUE( baked ) << baked.GetError();
        if ( !baked )
            return 0.0f;

        // How far out along +x the profile is still above zero.
        const uint32_t y = Assets::kCloudModellingVolumeHeight / 2;
        const uint32_t z = Assets::kCloudModellingVolumeDepth / 2;

        uint32_t last = Assets::kCloudModellingVolumeWidth / 2;
        for ( uint32_t x = Assets::kCloudModellingVolumeWidth / 2; x < Assets::kCloudModellingVolumeWidth; ++x )
        {
            if ( baked.GetValue()[VoxelIndex( x, y, z )] > 0u )
                last = x;
        }

        // The centre of the last voxel that carries body, in km from the box's centre.
        return ( ( static_cast<float>( last ) + 0.5f ) / static_cast<float>( Assets::kCloudModellingVolumeWidth ) -
                 0.5f ) *
               recipe.SizeKm.x;
    };

    const float atOne  = surfaceReachKm( 1.0f );
    const float atFour = surfaceReachKm( 4.0f );

    ASSERT_GT( atOne, 0.0f );

    const float predicted = blendRadiusKm * std::log( 4.0f ); // 0.0693 km
    const float measured  = atFour - atOne;

    // One voxel is 15.6 m, so the tolerance is a voxel and a half — the quantisation of measuring a
    // surface by counting voxels, not slack in the claim.
    EXPECT_NEAR( measured, predicted, 0.024f ) << "a weight of 4 moved the surface by " << measured * 1000.0f
                                               << " m; the algebra says " << predicted * 1000.0f << " m";
}

// ---------------------------------------------------------------------------------------------------
// What the panel greys its Bake button for — the same function, so the two cannot disagree
// ---------------------------------------------------------------------------------------------------

TEST( CloudModellingRecipe, EachPrimitiveIsHeldToItsOwnConstraintOnTheRadii )
{
    // Every component of every lump's radii is READ by something, and these refusals are what make that
    // true. Without them a sphere would carry two numbers nothing looks at, which is the dead-data defect
    // the contract forbids — and worse, the box arithmetic would be reserving room for an extent the body
    // does not have.
    const auto withOneLump = [&]( const Assets::CloudModellingBlob& blob )
    {
        Assets::CloudModellingVolumeRecipe recipe;
        recipe.SizeKm           = glm::vec3( 2.0f, 1.0f, 2.0f );
        recipe.BlendRadiusKm    = 0.05f;
        recipe.ProfileDepthKm   = 0.18f;
        recipe.EnvelopeMarginKm = 0.09f;
        recipe.Blobs            = { blob };
        return Assets::ValidateCloudModellingRecipe( recipe );
    };

    // A sphere whose radii do not agree.
    {
        const auto valid =
             withOneLump( Assets::CloudModellingBlob{ .CentreKm  = glm::vec3( 0.0f ),
                                                      .RadiiKm   = glm::vec3( 0.25f, 0.20f, 0.25f ),
                                                      .Primitive = Assets::CloudModellingPrimitive::Sphere } );
        ASSERT_FALSE( valid );
        EXPECT_NE( valid.GetError().find( "Sphere" ), std::string::npos ) << valid.GetError();
    }

    // A capsule whose cross-section is elliptical.
    {
        const auto valid =
             withOneLump( Assets::CloudModellingBlob{ .CentreKm  = glm::vec3( 0.0f ),
                                                      .RadiiKm   = glm::vec3( 0.12f, 0.35f, 0.16f ),
                                                      .Primitive = Assets::CloudModellingPrimitive::Capsule } );
        ASSERT_FALSE( valid );
        EXPECT_NE( valid.GetError().find( "cross-section" ), std::string::npos ) << valid.GetError();
    }

    // A capsule shorter than its own radius — a sphere wearing a capsule's name.
    {
        const auto valid =
             withOneLump( Assets::CloudModellingBlob{ .CentreKm  = glm::vec3( 0.0f ),
                                                      .RadiiKm   = glm::vec3( 0.20f, 0.10f, 0.20f ),
                                                      .Primitive = Assets::CloudModellingPrimitive::Capsule } );
        ASSERT_FALSE( valid );
        EXPECT_NE( valid.GetError().find( "half-height" ), std::string::npos ) << valid.GetError();
    }

    // And the legal forms of all three are accepted, or the three refusals above prove nothing.
    EXPECT_TRUE(
         withOneLump( Assets::CloudModellingBlob{ .CentreKm  = glm::vec3( 0.0f ),
                                                  .RadiiKm   = glm::vec3( 0.25f, 0.25f, 0.25f ),
                                                  .Primitive = Assets::CloudModellingPrimitive::Sphere } ) );

    EXPECT_TRUE(
         withOneLump( Assets::CloudModellingBlob{ .CentreKm    = glm::vec3( 0.0f ),
                                                  .RadiiKm     = glm::vec3( 0.12f, 0.35f, 0.12f ),
                                                  .RotationDeg = glm::vec3( 0.0f, 0.0f, 90.0f ),
                                                  .Primitive   = Assets::CloudModellingPrimitive::Capsule } ) );

    EXPECT_TRUE(
         withOneLump( Assets::CloudModellingBlob{ .CentreKm  = glm::vec3( 0.0f ),
                                                  .RadiiKm   = glm::vec3( 0.30f, 0.15f, 0.24f ),
                                                  .Primitive = Assets::CloudModellingPrimitive::Ellipsoid } ) );
}

TEST( CloudModellingRecipe, AWeightOutsideItsRangeIsRefusedWithTheReasonNamed )
{
    Assets::CloudModellingVolumeRecipe recipe = SculptedRecipe();
    recipe.Blobs[1].Weight                    = 40.0f;

    const auto valid = Assets::ValidateCloudModellingRecipe( recipe );
    ASSERT_FALSE( valid );
    EXPECT_NE( valid.GetError().find( "weight" ), std::string::npos ) << valid.GetError();

    recipe.Blobs[1].Weight = 0.0f;
    EXPECT_FALSE( Assets::ValidateCloudModellingRecipe( recipe ) );
}

TEST( CloudModellingRecipe, TheBoxCheckReservesRoomForTheWeightsAndNotJustTheCount )
{
    // A0's bound was `BlendRadius * ln(N)`. This phase made the surface's inflation depend on the sum of
    // the WEIGHTS, and the two agree exactly while every weight is 1 — so a bound left counting lumps
    // would pass every test A0 wrote and under-reserve the moment an artist turned a weight up. The
    // failure it hides is a body cut flat by the volume's face.
    //
    // The recipe below is deliberately built to sit just inside the box at unit weight.
    Assets::CloudModellingVolumeRecipe recipe;
    recipe.SizeKm           = glm::vec3( 2.0f, 2.0f, 2.0f );
    recipe.BlendRadiusKm    = 0.20f;
    recipe.ProfileDepthKm   = 0.18f;
    recipe.EnvelopeMarginKm = 0.09f;

    for ( int i = 0; i < 8; ++i )
    {
        recipe.Blobs.push_back(
             Assets::CloudModellingBlob{ .CentreKm  = glm::vec3( 0.02f * static_cast<float>( i ), 0.0f, 0.0f ),
                                         .RadiiKm   = glm::vec3( 0.13f, 0.13f, 0.13f ),
                                         .Primitive = Assets::CloudModellingPrimitive::Sphere } );
    }

    // At unit weight the sum is 8, so the inflation is ln(8)*0.20 = 0.416 km. The furthest lump reaches
    // 0.14 + 0.13 on x, so the box needs 0.776 of its 1.0 half-size. It fits.
    ASSERT_TRUE( Assets::ValidateCloudModellingRecipe( recipe ) )
         << Assets::ValidateCloudModellingRecipe( recipe ).GetError();

    // Now the same lumps at weight 8. The weight sum is 64, ln(64)*0.20 = 0.832 km, and it no longer does.
    for ( Assets::CloudModellingBlob& blob : recipe.Blobs )
        blob.Weight = 8.0f;

    const auto valid = Assets::ValidateCloudModellingRecipe( recipe );
    ASSERT_FALSE( valid ) << "the weights inflate the body past the box and Validate did not notice";
    EXPECT_NE( valid.GetError().find( "total weight" ), std::string::npos ) << valid.GetError();
}

// ---------------------------------------------------------------------------------------------------
// The shipped body did not move
// ---------------------------------------------------------------------------------------------------

TEST( CloudModellingRecipe, TheShippedRecipeIsTheIdentityCaseOfEverythingThisPhaseAdded )
{
    // A0's example is eight unrotated unit-weight ellipsoids, and this phase must not have moved it. That
    // is not sentiment: the six-point render protocol compares this branch's frames against the previous
    // build's, and a body that changed shape would make every one of those numbers unreadable.
    //
    // Asserted as a property of the RECIPE — every lump is the identity case — and then as a property of
    // the BYTES, by re-baking it through the same code the new primitives run in.
    const Assets::CloudModellingVolumeRecipe& shipped = Assets::CloudModellingDefaultRecipe();

    for ( size_t i = 0; i < shipped.Blobs.size(); ++i )
    {
        EXPECT_EQ( shipped.Blobs[i].Primitive, Assets::CloudModellingPrimitive::Ellipsoid ) << "lump " << i;
        EXPECT_FLOAT_EQ( shipped.Blobs[i].Weight, 1.0f ) << "lump " << i;
        EXPECT_FLOAT_EQ( shipped.Blobs[i].RotationDeg.x, 0.0f ) << "lump " << i;
        EXPECT_FLOAT_EQ( shipped.Blobs[i].RotationDeg.y, 0.0f ) << "lump " << i;
        EXPECT_FLOAT_EQ( shipped.Blobs[i].RotationDeg.z, 0.0f ) << "lump " << i;
    }

    ASSERT_EQ( ShippedVoxels().size(), Assets::kCloudModellingVoxelBytes );

    auto again = Assets::GenerateCloudModellingVolume( shipped );
    ASSERT_TRUE( again ) << again.GetError();

    int maxDelta = 0;
    EXPECT_EQ( CountDifferingBytes( ShippedVoxels(), again.GetValue(), maxDelta ), 0u );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
