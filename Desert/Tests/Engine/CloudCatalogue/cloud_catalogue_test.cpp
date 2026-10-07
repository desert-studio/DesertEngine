// THE CATALOGUE OF FORMS, and what makes it a catalogue rather than ten captions.
//
// `Docs/Clouds/ANALYSIS_APPROACH.md` §6 names ten genera and says the standard phase Э4 is measured by:
// "a genus indistinguishable from its neighbour is a caption, not a form". A test that only checked that
// ten recipes validate would pass on ten copies of the same cloud, so this suite MEASURES the baked
// voxels and asserts, for each genus, the property that makes it that genus:
//
//   humilis        four times wider than tall, and no growth upward
//   mediocris      as tall as it is wide
//   congestus      taller than it is wide
//   cumulonimbus   WIDER AT THE TOP THAN AT THE BOTTOM — the anvil, which is the one shape a vertical
//                  profile curve cannot produce, so it is the entry that answers the procedural producer
//   stratocumulus  ONE connected sheet, many times wider than thick, with structure IN it
//   stratus        the flattest of the ten, one component, featureless
//   altocumulus    MANY separate elements — the one genus the procedural producer already does
//   cirrus         Detail Type near zero over the whole body: the genus lives in the up-rez, not the
//                  silhouette
//   lenticular     smooth convex lenses, far longer across the wind than along it, and SEPARATE
//   freeform       one connected body WITH A HOLE THROUGH IT, which the Alligator cannot be by
//                  construction: `best - second` lays a zero between every pair of cells, so two lobes
//                  with air between them are always two components
//
// ... and then asserts that no two genera have the same signature, which is the standard stated as an
// arithmetic property rather than as an opinion.
//
// GPU-free: it bakes with the engine's own generator and counts voxels.

#include <Engine/Assets/CloudModellingCatalogue.hpp>
#include <Engine/Assets/CloudModellingVolume.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
    namespace Assets = Desert::Assets;

    constexpr int kWidth  = static_cast<int>( Assets::kCloudModellingVolumeWidth );
    constexpr int kHeight = static_cast<int>( Assets::kCloudModellingVolumeHeight );
    constexpr int kDepth  = static_cast<int>( Assets::kCloudModellingVolumeDepth );

    /// What one genus measures. Every field is read off the BAKED VOXELS — none of it is quoted from the
    /// recipe, because a recipe is what was asked for and the voxels are what happened.
    struct Signature
    {
        float AspectRatio    = 0.0f; ///< widest horizontal extent / vertical extent, both in km
        float TopWidthRatio  = 0.0f; ///< width across the top fifth of the body / width across its bottom fifth
        float Occupancy      = 0.0f; ///< fraction of the box that carries body
        int   Components     = 0;    ///< six-connected components of the body
        float MeanDetailType = 0.0f; ///< 0 wispy, 1 billowy, averaged over the body
        int   AirPocket      = 0;    ///< the largest pocket of air a slice of the body ENCLOSES, in voxels
        int   BodyVoxels     = 0;

        /// A hole through the cloud rather than a crease in it. The threshold is a hundred voxels only to
        /// keep quantisation out; the measured values are 0 for nine genera and thousands for the arch,
        /// so nothing here depends on where between them the line is drawn.
        bool HasThroughHole() const
        {
            return AirPocket >= 100;
        }
    };

    struct Baked
    {
        Assets::CloudModellingVolumeRecipe Recipe;
        std::vector<unsigned char>         Voxels;
    };

    const Baked& Body( Assets::CloudModellingSpecies species )
    {
        static std::array<Baked, Assets::kCloudModellingSpeciesCount> bodies;
        static std::array<bool, Assets::kCloudModellingSpeciesCount>  ready{};

        const uint32_t index = static_cast<uint32_t>( species );
        if ( !ready[index] )
        {
            bodies[index].Recipe = Assets::CloudModellingCatalogueRecipe( species );

            auto voxels = Assets::GenerateCloudModellingVolume( bodies[index].Recipe );
            EXPECT_TRUE( voxels ) << Assets::CloudModellingSpeciesName( species ) << ": "
                                  << ( voxels ? std::string{} : voxels.GetError() );
            if ( voxels )
                bodies[index].Voxels = voxels.ExtractValue();

            ready[index] = true;
        }
        return bodies[index];
    }

    size_t VoxelIndex( int x, int y, int z )
    {
        return ( ( static_cast<size_t>( z ) * kHeight + y ) * kWidth + x ) * Assets::kCloudModellingBytesPerVoxel;
    }

    bool IsBody( const std::vector<unsigned char>& voxels, int x, int y, int z )
    {
        return voxels[VoxelIndex( x, y, z )] > 0u;
    }

    /// Six-connected components of the body, counted by flood fill — the same instrument A0 used to show
    /// the shipped cloud is one fused mass.
    int CountComponents( const std::vector<unsigned char>& voxels )
    {
        std::vector<uint8_t> seen( static_cast<size_t>( kWidth ) * kHeight * kDepth, 0u );

        const auto flat = []( int x, int y, int z )
        { return ( static_cast<size_t>( z ) * kHeight + y ) * kWidth + x; };

        int components = 0;

        std::vector<std::array<int, 3>> stack;
        for ( int z = 0; z < kDepth; ++z )
        {
            for ( int y = 0; y < kHeight; ++y )
            {
                for ( int x = 0; x < kWidth; ++x )
                {
                    if ( seen[flat( x, y, z )] || !IsBody( voxels, x, y, z ) )
                        continue;

                    ++components;
                    seen[flat( x, y, z )] = 1u;
                    stack.push_back( { x, y, z } );

                    while ( !stack.empty() )
                    {
                        const auto [cx, cy, cz] = stack.back();
                        stack.pop_back();

                        const int neighbours[6][3] = { { cx - 1, cy, cz }, { cx + 1, cy, cz },
                                                       { cx, cy - 1, cz }, { cx, cy + 1, cz },
                                                       { cx, cy, cz - 1 }, { cx, cy, cz + 1 } };

                        for ( const auto& n : neighbours )
                        {
                            if ( n[0] < 0 || n[0] >= kWidth || n[1] < 0 || n[1] >= kHeight || n[2] < 0 ||
                                 n[2] >= kDepth )
                                continue;
                            if ( seen[flat( n[0], n[1], n[2] )] || !IsBody( voxels, n[0], n[1], n[2] ) )
                                continue;

                            seen[flat( n[0], n[1], n[2] )] = 1u;
                            stack.push_back( { n[0], n[1], n[2] } );
                        }
                    }
                }
            }
        }

        return components;
    }

    /// The widest span of body along x at a given height, counted in voxels — the quantity an anvil is
    /// measured with, because "wider at the top" is a statement about horizontal extent per slice.
    int WidthAtHeight( const std::vector<unsigned char>& voxels, int y )
    {
        int minX = kWidth;
        int maxX = -1;

        for ( int z = 0; z < kDepth; ++z )
        {
            for ( int x = 0; x < kWidth; ++x )
            {
                if ( !IsBody( voxels, x, y, z ) )
                    continue;
                minX = std::min( minX, x );
                maxX = std::max( maxX, x );
            }
        }

        return maxX < 0 ? 0 : ( maxX - minX + 1 );
    }

    /**
     * The largest pocket of air a SLICE of the body encloses — air that cannot reach the edge of the
     * slice without crossing cloud. Counted in voxels, over every slice perpendicular to z.
     *
     * TOPOLOGY RATHER THAN A THRESHOLD, and the difference is the whole reason this function was
     * rewritten twice. The first version asked "does a line leave the body and come back", which every
     * genus here answers yes to — the congestus between two turrets, the stratocumulus between two rolls.
     * The second normalised that gap by the body's width and still scored the stratocumulus at 0.52
     * against the arch's 0.66, which is not a margin anybody should build an acceptance criterion on.
     *
     * A pocket is different in kind and not in degree: a crease, a cleft and a valley between two lobes
     * are all OPEN — the air in them reaches the sky. Air with cloud above it, below it and on both sides
     * is a hole through the cloud, and no amount of coverage tuning produces one from a sum of lobes
     * whose field is `best - second`.
     *
     * (In three dimensions the arch's air is not enclosed at all — it is a TUNNEL, open along z, which is
     * exactly what an arch is. That is why the fill is per-slice: the slice is where a handle shows.)
     */
    int LargestEnclosedAirPocket( const std::vector<unsigned char>& voxels )
    {
        int largest = 0;

        std::vector<uint8_t> reached( static_cast<size_t>( kWidth ) * kHeight );
        std::vector<int>     stack;

        for ( int z = 0; z < kDepth; ++z )
        {
            std::fill( reached.begin(), reached.end(), 0u );
            stack.clear();

            const auto flat = []( int x, int y ) { return y * kWidth + x; };
            const auto push = [&]( int x, int y )
            {
                if ( x < 0 || x >= kWidth || y < 0 || y >= kHeight )
                    return;
                if ( reached[flat( x, y )] || IsBody( voxels, x, y, z ) )
                    return;
                reached[flat( x, y )] = 1u;
                stack.push_back( flat( x, y ) );
            };

            // The outside, entered from every edge of the slice.
            for ( int x = 0; x < kWidth; ++x )
            {
                push( x, 0 );
                push( x, kHeight - 1 );
            }
            for ( int y = 0; y < kHeight; ++y )
            {
                push( 0, y );
                push( kWidth - 1, y );
            }

            while ( !stack.empty() )
            {
                const int cell = stack.back();
                stack.pop_back();

                const int x = cell % kWidth;
                const int y = cell / kWidth;

                push( x - 1, y );
                push( x + 1, y );
                push( x, y - 1 );
                push( x, y + 1 );
            }

            // Whatever the outside could not reach is a pocket. Its SIZE is measured, so that a single
            // voxel of quantisation noise trapped in a crease cannot be mistaken for an arch.
            std::vector<uint8_t> counted( static_cast<size_t>( kWidth ) * kHeight, 0u );
            for ( int y = 0; y < kHeight; ++y )
            {
                for ( int x = 0; x < kWidth; ++x )
                {
                    if ( reached[flat( x, y )] || IsBody( voxels, x, y, z ) || counted[flat( x, y )] )
                        continue;

                    int size = 0;
                    stack.clear();
                    counted[flat( x, y )] = 1u;
                    stack.push_back( flat( x, y ) );

                    while ( !stack.empty() )
                    {
                        const int cell = stack.back();
                        stack.pop_back();
                        ++size;

                        const int cx = cell % kWidth;
                        const int cy = cell / kWidth;

                        const int neighbours[4][2] = {
                             { cx - 1, cy }, { cx + 1, cy }, { cx, cy - 1 }, { cx, cy + 1 } };

                        for ( const auto& n : neighbours )
                        {
                            if ( n[0] < 0 || n[0] >= kWidth || n[1] < 0 || n[1] >= kHeight )
                                continue;
                            if ( counted[flat( n[0], n[1] )] || IsBody( voxels, n[0], n[1], z ) )
                                continue;
                            counted[flat( n[0], n[1] )] = 1u;
                            stack.push_back( flat( n[0], n[1] ) );
                        }
                    }

                    largest = std::max( largest, size );
                }
            }
        }

        return largest;
    }

    Signature Measure( Assets::CloudModellingSpecies species )
    {
        const Baked& baked = Body( species );

        Signature signature;
        if ( baked.Voxels.empty() )
            return signature;

        int minX = kWidth, maxX = -1;
        int minY = kHeight, maxY = -1;
        int minZ = kDepth, maxZ = -1;

        double detailSum = 0.0;

        for ( int z = 0; z < kDepth; ++z )
        {
            for ( int y = 0; y < kHeight; ++y )
            {
                for ( int x = 0; x < kWidth; ++x )
                {
                    if ( !IsBody( baked.Voxels, x, y, z ) )
                        continue;

                    minX = std::min( minX, x );
                    maxX = std::max( maxX, x );
                    minY = std::min( minY, y );
                    maxY = std::max( maxY, y );
                    minZ = std::min( minZ, z );
                    maxZ = std::max( maxZ, z );

                    detailSum += static_cast<double>( baked.Voxels[VoxelIndex( x, y, z ) + 1] ) / 255.0;
                    ++signature.BodyVoxels;
                }
            }
        }

        if ( signature.BodyVoxels == 0 )
            return signature;

        const glm::vec3 voxelKm =
             baked.Recipe.SizeKm / glm::vec3( static_cast<float>( kWidth ), static_cast<float>( kHeight ),
                                              static_cast<float>( kDepth ) );

        const float extentX = static_cast<float>( maxX - minX + 1 ) * voxelKm.x;
        const float extentY = static_cast<float>( maxY - minY + 1 ) * voxelKm.y;
        const float extentZ = static_cast<float>( maxZ - minZ + 1 ) * voxelKm.z;

        signature.AspectRatio = std::max( extentX, extentZ ) / extentY;
        signature.Occupancy =
             static_cast<float>( signature.BodyVoxels ) / static_cast<float>( kWidth * kHeight * kDepth );
        signature.MeanDetailType = static_cast<float>( detailSum / signature.BodyVoxels );
        signature.Components     = CountComponents( baked.Voxels );

        // The anvil measure: the widest slice in the top fifth of the body against the widest in the
        // bottom fifth. A fifth rather than a half because an anvil IS the top of a cumulonimbus and a
        // half would average it with the tower it sits on.
        const int span  = maxY - minY + 1;
        const int fifth = std::max( span / 5, 1 );

        int topWidth    = 0;
        int bottomWidth = 0;
        for ( int i = 0; i < fifth; ++i )
        {
            topWidth    = std::max( topWidth, WidthAtHeight( baked.Voxels, maxY - i ) );
            bottomWidth = std::max( bottomWidth, WidthAtHeight( baked.Voxels, minY + i ) );
        }

        signature.TopWidthRatio =
             bottomWidth > 0 ? static_cast<float>( topWidth ) / static_cast<float>( bottomWidth ) : 0.0f;

        signature.AirPocket = LargestEnclosedAirPocket( baked.Voxels );

        return signature;
    }

    const char* Name( Assets::CloudModellingSpecies species )
    {
        return Assets::CloudModellingSpeciesName( species );
    }
} // namespace

using Species = Assets::CloudModellingSpecies;

TEST( CloudCatalogue, EveryGenusValidatesAndBakes )
{
    // The floor, and nothing more: ten recipes the generator accepts. Everything worth saying about the
    // catalogue is below this test, which is the point of it being first and shortest.
    for ( uint32_t i = 0; i < Assets::kCloudModellingSpeciesCount; ++i )
    {
        const auto species = static_cast<Species>( i );
        const auto valid =
             Assets::ValidateCloudModellingRecipe( Assets::CloudModellingCatalogueRecipe( species ) );

        EXPECT_TRUE( valid ) << Name( species ) << ": " << ( valid ? std::string{} : valid.GetError() );
        EXPECT_EQ( Body( species ).Voxels.size(), static_cast<size_t>( Assets::kCloudModellingVoxelBytes ) )
             << Name( species );
        EXPECT_GT( Measure( species ).BodyVoxels, 1000 ) << Name( species ) << " baked almost nothing";
    }
}

TEST( CloudCatalogue, TheNamesAndKeysAreDistinctAndComplete )
{
    std::vector<std::string> names;
    std::vector<std::string> keys;

    for ( uint32_t i = 0; i < Assets::kCloudModellingSpeciesCount; ++i )
    {
        const auto species = static_cast<Species>( i );
        names.emplace_back( Assets::CloudModellingSpeciesName( species ) );
        keys.emplace_back( Assets::CloudModellingSpeciesKey( species ) );

        EXPECT_FALSE( names.back().empty() );
        EXPECT_FALSE( keys.back().empty() );
    }

    std::sort( names.begin(), names.end() );
    std::sort( keys.begin(), keys.end() );

    EXPECT_EQ( std::adjacent_find( names.begin(), names.end() ), names.end() );
    EXPECT_EQ( std::adjacent_find( keys.begin(), keys.end() ), keys.end() );
}

TEST( CloudCatalogue, TheCumulusLadderRisesAndTheProportionsAreTheGenus )
{
    // humilis, mediocris, congestus are ONE family separated by nothing but proportion, so they are the
    // hardest three to keep distinct and the ones a catalogue is most likely to reduce to captions.
    const Signature humilis   = Measure( Species::CumulusHumilis );
    const Signature mediocris = Measure( Species::CumulusMediocris );
    const Signature congestus = Measure( Species::CumulusCongestus );

    // A fair-weather cumulus is a pancake; a congestus is a tower. The ladder is monotone in aspect.
    EXPECT_GT( humilis.AspectRatio, 2.5f ) << "humilis aspect " << humilis.AspectRatio;
    EXPECT_GT( humilis.AspectRatio, mediocris.AspectRatio );
    EXPECT_GT( mediocris.AspectRatio, congestus.AspectRatio );
    EXPECT_LT( congestus.AspectRatio, 1.0f ) << "congestus aspect " << congestus.AspectRatio;

    // ... and it is monotone by a MARGIN, not by a hair, which is what "distinguishable" has to mean.
    EXPECT_GT( humilis.AspectRatio / mediocris.AspectRatio, 1.5f );
    EXPECT_GT( mediocris.AspectRatio / congestus.AspectRatio, 1.5f );

    // All three are ONE body: a cumulus with a detached lobe is not a cumulus, it is two clouds.
    EXPECT_EQ( humilis.Components, 1 );
    EXPECT_EQ( mediocris.Components, 1 );
    EXPECT_EQ( congestus.Components, 1 );
}

TEST( CloudCatalogue, TheCumulonimbusIsWiderAtTheTopThanAtTheBottom )
{
    // THE ENTRY THAT ANSWERS THE PROCEDURAL PRODUCER DIRECTLY. Its vertical profile is a curve of
    // altitude applied to a coverage field, which can taper a whole layer but cannot put a wide ice
    // canopy over a narrow tower and leave the air beside the tower empty. An anvil is that, and this is
    // the number.
    const Signature storm = Measure( Species::Cumulonimbus );

    EXPECT_GT( storm.TopWidthRatio, 1.8f ) << "top/bottom width " << storm.TopWidthRatio;
    EXPECT_EQ( storm.Components, 1 ) << "the canopy has come off the tower";

    // The canopy is ICE and the tower is water, which the volume says in its SECOND channel: the anvil's
    // lumps carry Detail Type near zero so the up-rez erodes them into wisps. Averaged over the body that
    // pulls the mean well below the pure-billow 1 the cumulus ladder sits at.
    EXPECT_LT( storm.MeanDetailType, 0.8f ) << "mean detail type " << storm.MeanDetailType;

    // And it is the tallest of the ten in kilometres, which is what a cumulonimbus is.
    const Assets::CloudModellingVolumeRecipe& recipe =
         Assets::CloudModellingCatalogueRecipe( Species::Cumulonimbus );
    for ( uint32_t i = 0; i < Assets::kCloudModellingSpeciesCount; ++i )
    {
        if ( static_cast<Species>( i ) == Species::Cumulonimbus )
            continue;
        EXPECT_GT( recipe.SizeKm.y, Assets::CloudModellingCatalogueRecipe( static_cast<Species>( i ) ).SizeKm.y )
             << "the cumulonimbus is not taller than " << Name( static_cast<Species>( i ) );
    }
}

TEST( CloudCatalogue, TheSheetsAreOneBodyAndTheFieldIsMany )
{
    // The pair that says what phase Э4 is FOR and what it is not for, in one test.
    //
    // A stratocumulus is a continuous deck with rolls IN it — one component with structure. That is
    // exactly the combination the Alligator cannot hold: its lobes make the rolls and can never fuse
    // them. An altocumulus is many separate elements on a lattice, which is what the Alligator produces
    // for free, and this suite records that rather than pretending otherwise.
    const Signature stratocumulus = Measure( Species::Stratocumulus );
    const Signature stratus       = Measure( Species::Stratus );
    const Signature altocumulus   = Measure( Species::Altocumulus );

    EXPECT_EQ( stratocumulus.Components, 1 ) << "the rolls did not fuse into a deck";
    EXPECT_GT( stratocumulus.AspectRatio, 5.0f ) << "aspect " << stratocumulus.AspectRatio;

    EXPECT_EQ( stratus.Components, 1 );
    EXPECT_GT( stratus.AspectRatio, stratocumulus.AspectRatio ) << "stratus is not the flatter of the two";

    EXPECT_GE( altocumulus.Components, 9 ) << "a mackerel sky is many elements, not " << altocumulus.Components;
}

TEST( CloudCatalogue, TheCirrusLivesInTheDetailChannelRatherThanInTheSilhouette )
{
    // Its silhouette is a set of thin rods, which is not a cloud. What makes it cirrus is Detail Type at
    // zero over the whole body, which hands the up-rez the WISPY erosion — the volume carrying the
    // silhouette and the noise carrying the cloud, which is the arrangement 15.6 m voxels were chosen on
    // the strength of (PLAN_AUTHORED_CLOUDS.md §2).
    const Signature cirrus = Measure( Species::Cirrus );

    EXPECT_LT( cirrus.MeanDetailType, 0.2f ) << "mean detail type " << cirrus.MeanDetailType;
    EXPECT_GT( cirrus.AspectRatio, 4.0f ) << "aspect " << cirrus.AspectRatio;

    // Every other genus is billowy somewhere; cirrus is the only one that is wispy everywhere.
    for ( uint32_t i = 0; i < Assets::kCloudModellingSpeciesCount; ++i )
    {
        const auto species = static_cast<Species>( i );
        if ( species == Species::Cirrus )
            continue;
        EXPECT_GT( Measure( species ).MeanDetailType, cirrus.MeanDetailType ) << Name( species );
    }
}

TEST( CloudCatalogue, TheLenticularIsAStackOfSeparateLenses )
{
    // The one place in this catalogue where more than one component is the RIGHT answer: a mountain wave
    // stacks plates with clear air between them, and fusing them would be the defect.
    const Signature lens = Measure( Species::Lenticular );

    EXPECT_EQ( lens.Components, 3 ) << "the pile of plates is " << lens.Components << " components";

    // Far longer across the wind than along it, which is what a wave cloud is and what distinguishes it
    // from a flattened cumulus of the same thickness.
    const Assets::CloudModellingVolumeRecipe& recipe =
         Assets::CloudModellingCatalogueRecipe( Species::Lenticular );
    EXPECT_GT( recipe.SizeKm.x / recipe.SizeKm.z, 1.4f );
}

TEST( CloudCatalogue, TheFreeformArchIsOneBodyWithAHoleThroughIt )
{
    // THE PROOF OF THE WHOLE PHASE, as a pair of measurements that cannot both be true of the procedural
    // producer. Its coverage field is `best - second`, exactly zero on the bisector between every pair of
    // feature points, so two lobes with air between them are ALWAYS two components — three tasks measured
    // that independently before Э4 was approved. A body that is CONNECTED and HOLED is unreachable by
    // construction rather than by tuning.
    const Signature arch = Measure( Species::Freeform );

    EXPECT_EQ( arch.Components, 1 ) << "the legs came off the span, so this is not an arch";
    EXPECT_TRUE( arch.HasThroughHole() ) << "the arch encloses no air: pocket " << arch.AirPocket;

    // ... and no other genus in the catalogue is both, so the property is the arch's and not an artefact
    // of how it is measured.
    int both = 0;
    for ( uint32_t i = 0; i < Assets::kCloudModellingSpeciesCount; ++i )
    {
        const Signature signature = Measure( static_cast<Species>( i ) );
        if ( signature.Components == 1 && signature.HasThroughHole() )
            ++both;
    }
    EXPECT_EQ( both, 1 );
}

TEST( CloudCatalogue, NoTwoGeneraHaveTheSameSignature )
{
    // THE STANDARD §6 SETS, as an assertion: "a genus indistinguishable from its neighbour is a caption,
    // not a form". Two bodies are distinguishable when at least one of five measured quantities separates
    // them by a margin — not by a rounding error, which is why every threshold below is a RATIO or a
    // count and not an epsilon.
    struct Row
    {
        Species   Which;
        Signature Value;
    };

    std::vector<Row> rows;
    for ( uint32_t i = 0; i < Assets::kCloudModellingSpeciesCount; ++i )
        rows.push_back( Row{ static_cast<Species>( i ), Measure( static_cast<Species>( i ) ) } );

    for ( size_t a = 0; a < rows.size(); ++a )
    {
        for ( size_t b = a + 1; b < rows.size(); ++b )
        {
            const Signature& x = rows[a].Value;
            const Signature& y = rows[b].Value;

            const bool byAspect =
                 std::max( x.AspectRatio, y.AspectRatio ) > 1.4f * std::min( x.AspectRatio, y.AspectRatio );
            const bool byTop        = std::abs( x.TopWidthRatio - y.TopWidthRatio ) > 0.35f;
            const bool byComponents = x.Components != y.Components;
            const bool byDetail     = std::abs( x.MeanDetailType - y.MeanDetailType ) > 0.15f;
            const bool byHole       = x.HasThroughHole() != y.HasThroughHole();

            EXPECT_TRUE( byAspect || byTop || byComponents || byDetail || byHole )
                 << Name( rows[a].Which ) << " and " << Name( rows[b].Which ) << " measure the same: aspect "
                 << x.AspectRatio << " vs " << y.AspectRatio << ", top " << x.TopWidthRatio << " vs "
                 << y.TopWidthRatio << ", components " << x.Components << " vs " << y.Components << ", detail "
                 << x.MeanDetailType << " vs " << y.MeanDetailType;
        }
    }
}

TEST( CloudCatalogue, EveryGenusIsPrintedSoTheReportIsMeasuredRatherThanClaimed )
{
    // Not an assertion — the TABLE. The report of phase A3 quotes these numbers, and a table a reader can
    // regenerate is worth more than one they have to trust.
    std::printf( "%-18s %8s %8s %8s %6s %8s %8s\n", "genus", "aspect", "top/bot", "occup%", "comps", "detail",
                 "pocket" );
    for ( uint32_t i = 0; i < Assets::kCloudModellingSpeciesCount; ++i )
    {
        const auto      species   = static_cast<Species>( i );
        const Signature signature = Measure( species );

        std::printf( "%-18s %8.2f %8.2f %8.2f %6d %8.2f %8d\n", Assets::CloudModellingSpeciesKey( species ),
                     signature.AspectRatio, signature.TopWidthRatio, 100.0f * signature.Occupancy,
                     signature.Components, signature.MeanDetailType, signature.AirPocket );
    }

    SUCCEED();
}
