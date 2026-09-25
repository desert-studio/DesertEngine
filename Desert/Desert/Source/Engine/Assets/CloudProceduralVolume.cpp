#include "CloudProceduralVolume.hpp"

#include <Common/Core/Math/Rounding.hpp>
#include <Common/Core/JobSystem.hpp>
#include <Common/Core/Profiler.hpp>
#include <Common/Core/ResultStr.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>

namespace Desert::Assets
{
    namespace
    {
        /// How many blend radii past the nearest lump a lump may be before it is dropped from the join.
        ///
        /// FOURTEEN, AND THE NUMBER IS A QUANTISATION ARGUMENT rather than a feel. A dropped lump's term
        /// is `exp(-14) = 8.3e-7` of the nearest one's, so the error in the joined distance is at most
        /// `BlendRadiusKm * N * 8.3e-7`; at the shipped 60 m radius with six hundred lumps in range that
        /// is 3.0e-5 km, and divided by the 0.36 km profile depth it is 8.3e-5 of a unit profile — a
        /// fiftieth of the 1/255 the volume is quantised to. The cut is therefore invisible in the bytes,
        /// which is the only place it could ever be seen.
        ///
        /// IT WAS TEN AND THE SUITE CAUGHT IT. `N` is not a constant of the design — it is how many lumps
        /// reach a voxel — and when the clusters were widened to make the coverage slider mean the sky, it
        /// went from about a hundred to about six hundred. At ten radii the agreement with a gather over
        /// every lump went from 0.5 of a 255th to 1.24 of one, which is the assertion in
        /// Desert/Tests/Engine/CloudProceduralField failing exactly where it was written to.
        constexpr float kJoinCutoffRadii = 14.0f;

        /// HOW MUCH WIDER A CLUSTER IS MADE, per unit of coverage, to pay for the packing the free
        /// placement costs.
        ///
        /// WHAT IT PAYS FOR, AND IT IS NOT A FUDGE. A jittered lattice packs EFFICIENTLY: one cluster per
        /// cell, kept near its own site, so neighbours barely overlap and almost every square kilometre of
        /// cluster is a square kilometre of sky. Letting a cluster wander a whole cell is the cure for the
        /// grid — Tools/LatticePeak measures the lattice bump falling from 0.066 to 0.011 on that change
        /// alone — and independently placed bodies overlap, so the same amount of cloud covers less sky.
        /// Measured: at the shipped scene's coverage the sky went from 0.781 to 0.733 on the scatter
        /// alone, and to 0.640 with the size spread and the patches as well.
        ///
        /// THE SHAPE IS A LINE THROUGH ONE AT ZERO, because at zero coverage there is nothing to overlap
        /// and nothing to compensate, and the loss grows with how much cloud is in the sky.
        ///
        /// THE SLOPE IS MEASURED AT THE SHIPPED PLACEMENT AND NOWHERE ELSE, and saying so is the honest
        /// part: it is fitted on the top-down projection of the baked volume at five settings of the
        /// slider, with the density, the scatter and the size spread at the values this component ships.
        /// At 0.08 the sky the slider asks for and the sky it delivers are
        ///
        ///     Coverage  0.15   0.24   0.35   0.50   0.75
        ///     measured  0.169  0.249  0.357  0.519  0.741
        ///
        /// — out by at most 0.019, against the tenth Desert/Tests/Engine/CloudProceduralField allows and
        /// against the 0.11 the free placement was out by before this line existed.
        ///
        /// IT DOES NOT DEPEND ON THE SIZE SPREAD, which is by construction: the size draw is uniform in
        /// AREA with a mean of one. It depends only weakly on the density, which is what
        /// kDensityCompensation is for. It DOES depend on the SCATTER, which is precisely what it is
        /// compensating — an artist who returns the scatter to zero gets a sky a few points fuller than
        /// the slider says, which is inside the suite's tenth and is stated on the knob's own tooltip.
        ///
        /// AND IT DOES NOT DEPEND ON THE CELL, WHICH WAS MEASURED RATHER THAN ASSUMED — §CB, and it is
        /// recorded here because two phases in a row named the opposite as the cause of the cumulonimbus'
        /// slider and neither tested it. Both this line and the alive exponent below are fitted at the
        /// 3 km cell and neither carries a cell term, which LOOKS like the defect. It is not: the cover a
        /// placement delivers is SCALE-FREE in the cell, because the clusters per unit area fall as the
        /// cell's area exactly as fast as one cluster's footprint rises with it, so the cell cancels out of
        /// the expected cover. Measured at `Coverage 0.5` on ONE genus with the cell the only thing that
        /// changes — the shipped congestus at five settings of its Placement Scale, 8 realisations:
        ///
        ///     cell     1.500  2.400  3.000  6.000  12.000 km
        ///     sky      0.502  0.505  0.512  0.532  0.503
        ///
        /// — eight times of cell for a spread of 0.030, and NOT MONOTONE: the widest cell in the library
        /// is the second most accurate row. What is left is the estimator's own wobble on a region that
        /// holds only four coarse cells across, not a law. Against it, the cumulonimbus was out by 0.356.
        /// What its 6 km cell was carrying is its ANVIL, and that is priced by CloudClusterFootprintGain.
        constexpr float kPackingCompensation = 0.08f;

        /// How fast a cluster narrows as a cell is given more of them, as the exponent of the count.
        ///
        /// A HALF IS THE ANSWER TO THE WRONG QUESTION, and the suite is what said so. A half preserves the
        /// total AREA of the clusters in a cell exactly — `d` of them, each of `1/sqrt(d)` the width — and
        /// that would be the right compensation if the ground they covered were the sum of their areas. It
        /// is not, because they OVERLAP, and how much they overlap depends on how many there are: one
        /// cluster at the shipped size covers 1.63 cell-areas, so it saturates its own cell and spills into
        /// its neighbours, while four clusters of a quarter that area each cover 0.41 of a cell and between
        /// them leave 12 per cent of it open. Measured on the placement, a half took the sky's cover from
        /// 0.701 at a density of 1 to 0.597 at 4 — a tenth of the sky, which is the whole tolerance
        /// Desert/Tests/Engine/CloudProceduralField allows the Coverage slider.
        ///
        /// 0.40 IS MEASURED AND NOT DERIVED, and the difference is worth naming: the derivation would need
        /// the saturation of a cluster wider than its own cell, which depends on the coverage as well as on
        /// the count. What is asserted instead is the RELATION — Desert/Tests/Engine/CloudPlacementSpectrum
        /// re-measures the cover at a density of 1 and of 4 on every run and fails if they part company by
        /// more than a twentieth of the sky.
        constexpr float kDensityCompensation = 0.40f;

        /// How many lumps one cluster is built from. A COUNT AND NOT A CEILING ANY MORE, and the change is
        /// the visible half of §SIL.
        ///
        /// IT USED TO BE SHORTENED BY THE BAND, and that was the arithmetic behind the flat lens. The old
        /// rule gave each lump `band / count` of the height and set its vertical radius at 0.6 of that
        /// spacing, so the lump's height came from the TYPE's altitudes divided by a constant living in
        /// this file while its width came from the placement CELL — two numbers with no reason to agree,
        /// measured at 2.1 to 2.5 times wider than tall (CALIBRATION.md §RW2). Now a lump's height is
        /// derived from its own width (kLumpVerticalOverHorizontal) and the band decides only WHERE the
        /// stack sits, so there is nothing left for the count to protect: a lump too thin for the march is
        /// prevented by the per-lump floor at half of ResolvableChordKm, which is where that relation
        /// belongs.
        ///
        /// SIX IS A PROPERTY OF THE DISC AND NOT OF THE BAND. The lobes are spread over a disc one golden
        /// angle apart as well as up the band, and six is how many a disc needs before its outline reads as
        /// a lumpy mass rather than as a rosette. A thin type therefore gets six flattened lobes in a
        /// shallow pile, which is what a thin type IS, instead of two lobes that read as dots.
        constexpr uint32_t kBlobsPerCluster = 6u;

        /// A LUMP'S HEIGHT OVER ITS OWN WIDTH — the ONE ratio that turns the cluster's single size into
        /// both of a lump's radii, and the recorded decision of §SIL.
        ///
        /// WHAT THE ALTERNATIVE WAS, because the choice had to be made rather than fallen into. The other
        /// candidate was to make the lump's aspect a PROPERTY OF THE TYPE — a fifteenth number in
        /// `.decloudtype`, set nine times. It is refused on evidence: a lump is the convective PARCEL, and
        /// what makes a genus a genus is how the parcels are ARRANGED (a heap, a deck, a downwind band, a
        /// sheet), not what one parcel looks like. Every reference photograph in the licence record shows
        /// the same roughly-isotropic turret texture at the small scale under every genus name. Nine
        /// authored numbers with no independent evidence behind any of them are nine settings that can only
        /// ever be asserted equal to themselves — dead settings in the sense of DEV_CONTRACT.md §1.3,
        /// arrived at from the far side. One constant can be asserted: every shipped type's lumps measure
        /// this ratio, and Desert/Tests/Engine/CloudPlacementSpectrum measures it on the emitted lumps.
        ///
        /// THE FLATNESS OF A GENUS SURVIVES ANYWAY, and it survives THROUGH the type rather than beside it:
        /// the band clamp below caps a lump at half the band it lives in, so a stratus whose band is 400 m
        /// gets 200 m lumps however wide its cell is, and a congestus whose band is 3.6 km does not. The
        /// squashing is done by the layer the type declares — which is what squashes a real stratus — and
        /// not by a number an artist has to keep consistent with the altitudes beside it.
        ///
        /// 0.75, AND THE BOUND THAT HELD IT AT 0.45 WAS PAID FOR RATHER THAN MOVED — see CALIBRATION.md
        /// §SIL2. The ladder on the shipped congestus, `Clouds_Demo`'s configuration, at the shipped
        /// placement, 8 realisations, with the sky that shipped before §SIL in the left-hand column:
        ///
        ///                    §RW2    0.40    0.45    0.50    0.60    0.75    1.00
        ///     horiz chord    1.705   1.924   2.002   2.084   2.264   2.541   2.916 km
        ///     vert chord     0.569   0.681   0.790   0.905   1.144   1.509   1.992 km
        ///     solidity        0.33    0.38    0.44    0.49    0.60    0.77    0.94
        ///     CORE aspect     3.3     3.1     2.8     2.5     2.2     1.8     1.6  : 1
        ///     envelope        1.1     1.2     1.2     1.2     1.3     1.4     1.5  : 1
        ///
        /// **THE OPAQUE CORE IS WHAT THE EYE READS AND IT IS WHY THIS NUMBER MOVED.** §RW2 measured the
        /// shipped body as a core of 3.3 : 1 inside an envelope of 1.1 : 1 — a ball of air with a plate of
        /// cloud through the middle of it — and named it the largest thing left. §SIL took it to 2.8 at
        /// 0.45; this constant takes it to 1.8, which is a body with volume in it rather than a blin.
        ///
        /// **WHAT STOPPED §SIL AT 0.45 WAS NOT THE PICTURE, IT WAS §DS'S EROSION FLOOR.** A taller lump makes
        /// a body optically thicker per metre, so the same cut moves the surface at which the optical depth
        /// first reaches 1 a SHORTER distance, and that distance has a floor: the chord the march can be
        /// relied on to find (CloudFinestResolvableChordKm, 125 m). At 0.75 against §DS's shipped strength of
        /// 0.40 the travel is 101 m — under the floor — and `Desert/Tests/Engine/CloudField` goes red.
        ///
        /// **THE FLOOR IS CLEARED BY DEEPENING THE CUT, WHICH IS §DS'S OWN RECIPE APPLIED A SECOND TIME.**
        /// The layer's Detail Strength moves 0.40 -> 0.60 and the two thin types whose Detail Factors were
        /// re-based by §DS are re-based again by the same ratio, so their cut DEPTH — `strength x factor` —
        /// does not move at all. The measured ladder and the identity are in ECS::VolumetricCloudData beside
        /// the strength itself; the frames are `Shots/SIL2_*`.
        ///
        /// **AND THE PAIRING IS NOW A TESTED RELATION RATHER THAN A COINCIDENCE.** This constant is exported
        /// as `kCloudLumpVerticalOverHorizontal` for one reason: `Desert/Tests/Engine/CloudField` reads it
        /// and the component's Detail Strength together and asserts their PRODUCT still clears the march.
        /// §SIL committed 0.75 with frames and a report before its own sweep found the floor underneath it;
        /// the two numbers had never been named in one place, so nothing in the repository could say so.
        ///
        /// THE COVER DOES NOT MOVE — four ten-thousandths over the whole ladder — so this constant does not
        /// spend the Coverage slider and decision D-20 is untouched by it at any setting. What it spends is
        /// the SIZE OF A BODY: a taller lump fuses with its neighbours across a wider front.
        constexpr float kLumpVerticalOverHorizontal = kCloudLumpVerticalOverHorizontal;

        /// HOW FAR A FULL ANVIL SPREADS BEYOND THE TOWER IT CAPS, per unit of `AnvilStrength`. It is the
        /// authored meaning of that slider: at 1.0 the canopy is 1.8 times the cluster's radius.
        ///
        /// IT IS A NAMED CONSTANT BECAUSE TWO PLACES READ IT — the emission below and
        /// CloudClusterFootprintGain, which prices the sky that canopy covers. Written twice, a change to
        /// one would leave the Coverage slider paying for a canopy of the wrong size and the sky would drift
        /// with no test able to say why: the two-places-that-must-agree shape DEV_CONTRACT.md §2.3.1 is
        /// about, and the shape that produced the very defect this constant is part of the cure for.
        constexpr float kAnvilSpreadPerStrength = 0.8f;

        /// The canopy is not quite round — a tenth narrower across the wind than along it, which is what
        /// spreading against a stable layer under a shear looks like. Read by the emission and by the
        /// footprint gain, for the same reason as above.
        constexpr float kAnvilAcrossOverAlong = 0.9f;

        /// THE TOWER'S OWN FOOTPRINT, at the two ends of the width law the profile curve replaced (which is
        /// `Graphic::CloudProfileFromTaper` at 0 and at 1), as the radius of the circle of
        /// the same area in cluster radii. Both come from one quadrature over the layout below — six lobes a
        /// golden angle apart on a disc of `0.48 * (1 - 0.55 t)` cluster radii, each `(0.62 - 0.16 t)` wide
        /// and scaled by a wobble on [0.85, 1.15], each displaced by up to 0.18 radii — with the union taken
        /// in projection. NOTHING IN THEM IS FITTED TO A SKY: no coverage, no cell, no genus and no seed
        /// enters the calculation, which is exactly the property `kPackingCompensation` beside them does not
        /// have and is honest about not having.
        ///
        /// THE LAW BETWEEN THE ENDS IS LINEAR TO 0.2 PER CENT — the quadrature gives 0.9363 at a taper of
        /// 0.4 against the 0.9377 the line predicts, and 0.9254 at 0.6 against 0.9268 — so a table would be
        /// three more numbers saying what two already say.
        constexpr float kTowerFootprintAtNoTaper   = 0.9594f;
        constexpr float kTowerFootprintAtFullTaper = 0.9051f;

        /// Radians to degrees, written out because a lump's rotation is authored in degrees and the wind
        /// arrives as a vector. Not `glm::degrees` only so that this file keeps its one glm include.
        constexpr float kDegreesPerRadian = 57.29577951308232f;

        /// The wrap offsets a lump is splatted at, in units of the region's period. NINE and not one,
        /// because the volume must be exactly periodic: a lump near the +X face has to appear at the -X
        /// face too, or REPEAT sampling shows a hard seam there. Offsets whose box misses the region cost
        /// one rejected box test.
        constexpr int kWrapRange = 1;

        /// A 32-bit integer hash. Murmur3's finalizer, which is the standard choice for turning a lattice
        /// index into an uncorrelated word, and it is written out rather than taken from a library because
        /// the bytes of the sky depend on it: a different mixer is a different sky, and a sky that changes
        /// when a dependency is upgraded is not reproducible.
        uint32_t HashWord( uint32_t value )
        {
            value ^= value >> 16;
            value *= 0x85ebca6bu;
            value ^= value >> 13;
            value *= 0xc2b2ae35u;
            value ^= value >> 16;
            return value;
        }

        uint32_t HashCombine( uint32_t seed, uint32_t value )
        {
            return HashWord( seed ^ ( HashWord( value ) + 0x9e3779b9u + ( seed << 6 ) + ( seed >> 2 ) ) );
        }

        /// A hash word as a number in [0, 1). The top 24 bits, so the result is exactly representable in a
        /// float and the mapping is uniform rather than very slightly biased at the last bit.
        float HashUnit( uint32_t word )
        {
            return static_cast<float>( word >> 8 ) * ( 1.0f / 16777216.0f );
        }

        /// A signed lattice index as an unsigned word, so that a hash is defined at negative coordinates —
        /// which every world west or north of the origin has.
        uint32_t IndexWord( int32_t index )
        {
            return static_cast<uint32_t>( index ) ^ 0x80000000u;
        }

        /// The horizontal frame the lattice is laid out in: the wind's direction and the axis across it.
        /// A zero wind means east, which is what Graphic::CloudSpeciesPlacementBasis also answers.
        void WindFrame( const glm::vec2& windAxis, glm::vec2& along, glm::vec2& across )
        {
            const float length = std::sqrt( windAxis.x * windAxis.x + windAxis.y * windAxis.y );
            along              = ( length > 1e-6f ) ? windAxis / length : glm::vec2( 1.0f, 0.0f );
            across             = glm::vec2( -along.y, along.x );
        }

        /// Where the centre of lattice cell (@p ix, @p iz) sits in the world, kilometres, XZ.
        glm::vec2 CellCentreKm( const glm::vec2& along, const glm::vec2& across, const glm::vec2& extent,
                                int32_t ix, int32_t iz )
        {
            const float u = ( static_cast<float>( ix ) + 0.5f ) * extent.x;
            const float v = ( static_cast<float>( iz ) + 0.5f ) * extent.y;
            return along * u + across * v;
        }

        /// A number in [-0.5, 0.5) from a hash word — the jitter that stops a lattice from reading as a
        /// grid.
        float HashSigned( uint32_t word )
        {
            return HashUnit( word ) - 0.5f;
        }

        /// One octave of smoothly interpolated value noise over the world's XZ plane, in [0, 1].
        ///
        /// VALUE NOISE AND NOT ANOTHER LATTICE OF LUMPS, because what this is for is the SLOW part of the
        /// sky — where the weather is busy and where it is clear — and that has no bodies in it. Smoothstep
        /// on the cell fraction rather than the fraction itself, so the field's first derivative is
        /// continuous and the patches have no facets along the octave's own axes.
        float ValueNoise( uint32_t seed, const glm::vec2& point )
        {
            const glm::vec2 base( std::floor( point.x ), std::floor( point.y ) );
            const glm::vec2 frac = point - base;

            const glm::vec2 weight( frac.x * frac.x * ( 3.0f - 2.0f * frac.x ),
                                    frac.y * frac.y * ( 3.0f - 2.0f * frac.y ) );

            const int32_t ix = static_cast<int32_t>( base.x );
            const int32_t iy = static_cast<int32_t>( base.y );

            const auto corner = [seed]( int32_t x, int32_t y )
            { return HashUnit( HashCombine( HashCombine( seed, IndexWord( x ) ), IndexWord( y ) ) ); };

            const float c00 = corner( ix, iy );
            const float c10 = corner( ix + 1, iy );
            const float c01 = corner( ix, iy + 1 );
            const float c11 = corner( ix + 1, iy + 1 );

            const float bottom = c00 + ( c10 - c00 ) * weight.x;
            const float top    = c01 + ( c11 - c01 ) * weight.x;
            return bottom + ( top - bottom ) * weight.y;
        }

        /// THE LARGE-SCALE MODULATION OF COVERAGE, in [0, 1] with a mean near a half.
        ///
        /// TWO OCTAVES AND THE SECOND IS ROTATED, because one octave of value noise is a lattice too: its
        /// extrema sit on ITS grid, and a patch field that is itself a grid would trade one visible period
        /// for another. The second octave runs at an irrational-ish ratio of the first's frequency and at
        /// 31.7 degrees to it, so the two never line up over any distance the region can show.
        float PatchField( uint32_t seed, const glm::vec2& worldKm, float tileKm )
        {
            const float safeTile = std::max( tileKm, 1e-3f );

            const glm::vec2 coarse = worldKm / safeTile;

            // cos/sin of 31.7 degrees, written out because the field's bytes depend on them.
            const glm::vec2 rotated( worldKm.x * 0.85072f - worldKm.y * 0.52561f,
                                     worldKm.x * 0.52561f + worldKm.y * 0.85072f );
            const glm::vec2 fine = rotated / ( safeTile * 0.47f );

            return 0.65f * ValueNoise( seed, coarse ) + 0.35f * ValueNoise( HashCombine( seed, 0x5eedu ), fine );
        }

        /// THE ONE PLACE A CELL'S COVERAGE IS DECIDED, and it is one place on purpose.
        ///
        /// Before the painted layout there were two lines here — the slider, then the procedural patch
        /// field folded into it. There are now two possible SOURCES for that same modulation, the painting
        /// and the hash, and the contract's rule about second paths (§1.3, §4.2) is what makes this a
        /// function rather than another `if` in the cell loop: a cell's coverage has exactly one modulator,
        /// chosen here, and no caller can apply both.
        ///
        /// WHY THE PAINTING WINS WHEN IT IS BOUND. It is the artist saying where the weather is, and the
        /// patch field is the engine guessing. Guessing is what happens when nobody has said. Turning
        /// `PatternStrength` down to zero hands the decision back to the guess rather than to nothing at
        /// all — which is what makes that end of the slider a live position instead of "no modulation",
        /// a state the sky already reads as "the whole sky is cloud" (CALIBRATION.md §RW).
        ///
        /// WHY THE PATTERN IS APPLIED ZERO-MEAN AND THE MASK IS NOT.
        ///
        ///   * The PATTERN is on by default the moment a painting is bound, so if it could move the sky's
        ///     average cover, then `Coverage` would stop meaning the fraction of sky it delivers — and that
        ///     mapping is what decision D-20 re-authorised every shipped scene against. Subtracting the
        ///     painting's own mean makes it redistribute cloud exactly as the patch field it replaces does
        ///     ("symmetric about the slider"), so the average is the slider again whatever is painted.
        ///   * The MASK is asymmetric, and that is its job: "add cloud here, remove it there" is the one
        ///     control an artist reaches for when they want MORE sky covered, and a symmetric version of it
        ///     could not do that. It is safe for D-20 in a way the pattern is not because a layout with no
        ///     mask table contributes exactly nothing, so no sky moves that an artist did not paint. This is
        ///     Unreal's own arrangement unchanged — the mask is summed into the assembled shape, and it
        ///     subtracts by carrying a negative weight rather than by multiplying
        ///     (Docs/Clouds/RESEARCH_LAYOUT_TEXTURES.md §3).
        ///
        /// THE SEEDS ARE DERIVED IN ONE PLACE, and it is not tidiness. The public
        /// CloudProceduralCellCoverage has to reach the same weather patch the bake's own loop reaches, or
        /// the panel's map and the baked sky would differ everywhere the painting is not the source — and
        /// they would differ SILENTLY, because both would look like plausible weather.
        uint32_t CloudSpeciesSeed( const CloudProceduralFieldParams& params, uint32_t slot )
        {
            return HashCombine( params.Seed, slot + 0x51ed270bu );
        }

        uint32_t CloudPatchSeed( uint32_t speciesSeed )
        {
            return HashCombine( speciesSeed, 0x9a71c4u );
        }

        float CloudCellCoverage( const CloudProceduralFieldParams& params, uint32_t slot, uint32_t patchSeed,
                                 const glm::vec2& centreKm )
        {
            const float base = std::clamp( params.Coverage, 0.0f, 1.0f );

            const CloudLayoutData* patternSource = params.PatternSource.get();
            const CloudLayoutData* maskSource    = params.MaskSource.get();

            const bool paintedPattern = patternSource != nullptr && patternSource->HasPattern() &&
                                        params.LayoutPlacement.PatternStrength > 1e-4f;

            float modulated = base;

            if ( paintedPattern )
            {
                const glm::vec2 uv = CloudLayoutUv( params.LayoutPlacement, params.RegionSizeKm, centreKm );

                // The channel is the SLOT and not a genus. Unreal fixes R/G/B/A to four named types for
                // ever; ours is whichever kind of cloud the artist dropped into that slot, which is the
                // more general arrangement and costs nothing.
                const float painted = SampleCloudLayoutPattern( *patternSource, slot, uv );
                const float centred =
                     painted - patternSource->PatternMean[std::min( slot, kCloudLayoutChannels - 1u )];

                // The same shape the patch field's own expression has, so the two sources push the slider
                // by comparable amounts and swapping one for the other is not also a change of scale. The
                // factor of two is what takes a centred fraction — which spans at most -1..1 and typically
                // far less — onto the same +/-100 per cent the patch field's `2*p - 1` covers.
                modulated = base * ( 1.0f + params.LayoutPlacement.PatternStrength * 2.0f * centred );
            }
            else
            {
                // Clamped HERE rather than by the caller, which is where it used to be. The clamp belongs
                // with the read: a strength that reached this function unclamped would scale the modulation
                // past its own documented range, and the caller that used to hold it is no longer the only
                // one there is.
                const float patchStrength = std::clamp( params.PatchStrength, 0.0f, 1.0f );
                if ( patchStrength > 1e-4f )
                {
                    const float patch = PatchField( patchSeed, centreKm, params.PatchTileKm );
                    modulated         = base * ( 1.0f + patchStrength * ( 2.0f * patch - 1.0f ) );
                }
            }

            // THE MASK COMES FROM ITS OWN SLOT, and it is read whether or not a pattern was. That is
            // Unreal's arrangement exactly (RESEARCH_LAYOUT_TEXTURES §3: the mask sits OUTSIDE and AFTER
            // everything, and is the one term that adds rather than multiplies), and it is what lets a
            // layer place its clouds procedurally and still have a painted region of cleared sky.
            if ( maskSource != nullptr && maskSource->HasMask() && params.LayoutPlacement.MaskStrength > 1e-4f )
            {
                const glm::vec2 uv = CloudLayoutUv( params.LayoutPlacement, params.RegionSizeKm, centreKm );
                const float     maskStrength = std::clamp( params.LayoutPlacement.MaskStrength, 0.0f, 1.0f );
                modulated += maskStrength * SampleCloudLayoutMask( *maskSource, uv );
            }

            return std::clamp( modulated, 0.0f, 1.0f );
        }

        /// How many clusters this cell carries, given a mean of @p density.
        ///
        /// A WHOLE NUMBER WITH THAT MEAN EXACTLY, by taking the integer part always and the fraction with
        /// its own probability. Rounding instead would make a density of 1.5 produce two clusters in every
        /// cell and a mean of two, which is a knob that lies about its own units.
        uint32_t ClusterCount( uint32_t cellSeed, float density )
        {
            const float clamped = std::max( density, 0.0f );
            const float whole   = std::floor( clamped );
            const float frac    = clamped - whole;

            const uint32_t base = static_cast<uint32_t>( whole );
            return base + ( ( HashUnit( HashCombine( cellSeed, 0x0c0u ) ) < frac ) ? 1u : 0u );
        }
    } // namespace

    float CloudProceduralLumpFloorKm( const CloudProceduralFieldParams& params )
    {
        // TWO BOUNDS AND THE VOLUME'S IS THE LARGER ONE, which is the whole finding — see the header.
        const float voxelKm = params.RegionSizeKm / static_cast<float>( params.VolumeSideVoxels );
        return std::max( 0.5f * params.ResolvableChordKm, voxelKm );
    }

    glm::vec2 CloudProceduralCellExtentKm( const CloudProceduralFieldParams& params,
                                           const CloudProceduralSpecies&     species )
    {
        // THE FLOOR IS NOT DEFENSIVE PADDING, IT IS A MEASURED BOUND. A cell smaller than a few voxels
        // cannot be expressed by the volume at all — the cluster inside it is narrower than the trilinear
        // filter's own support — and the cost of trying is quadratic in the region: the suite authored a
        // species with a 50 m cell and the generator produced 4 180 731 lumps for one 48 km region, which
        // took a minute to place and could never have been baked. Four voxels is the narrowest cluster the
        // volume can carry with an inside and two edges.
        //
        // AND IT IS A BOUND ON THE CLUSTER, WHICH IS NOT A BOUND ON A LUMP — measured, because the sentence
        // above reads as though it were. A cluster is six lobes; at exactly this floor (0.75 km at the
        // shipped region) 34.5 % of the lumps came out under the two voxels the volume can express, the
        // narrowest at 130 m against 375. What makes that sentence true is CloudProceduralLumpFloorKm,
        // applied per lump at the emission site. This floor still earns its place — it is what keeps the
        // lump COUNT finite — but it is not the relation it was read as.
        //
        // NOR DOES IT BIND ON ANYTHING IN THE REPOSITORY. Every cloud scene here carries a 12 km Weather
        // Tile, so the lattice is 3 km and a cell is `3 km * PlacementScale`; the smallest Placement Scale
        // in the shipped library is the altocumulus' 0.30, giving 0.90 km against this 0.75 km. The floor
        // is live and it clamps — a species asked for 0.375 km bakes byte-for-byte identically to one asked
        // for 0.75 km — but no authored sky is standing on it.
        //
        // THE 0.75 km IN THAT PARAGRAPH IS THE VALUE AT THE SHIPPED GRID, and since O8 the grid is a
        // parameter: `4 * RegionSize / side` is 0.75 km at 256 and 1.5 km at the 128 an asset preview uses.
        // The margin above therefore narrows with the budget, and that is measured rather than assumed —
        // Assets::kCloudProceduralVolumeSideMin carries the table and the refusal of anything coarser.
        const float voxelKm = params.RegionSizeKm / static_cast<float>( params.VolumeSideVoxels );
        const float floorKm = std::max( 4.0f * voxelKm, 2.0f * params.ResolvableChordKm );

        const float anisotropy = std::max( species.Anisotropy, 1e-3f );
        const float root       = std::sqrt( anisotropy );
        const float cell       = std::max( species.CellKm, floorKm );
        return glm::vec2( cell * root, cell / root );
    }

    float CloudProfileMeanHalfWidth( const Graphic::CloudVerticalProfile& profile )
    {
        // AT THE STACK'S OWN HEIGHTS AND NOT AT EVEN SPACING, because these six are the only heights the
        // curve is ever read at — `t = pow(u, 1.7)` below is the stack's parameterisation, and a mean
        // taken anywhere else would be a mean of a curve nothing samples.
        float sum = 0.0f;
        for ( uint32_t step = 0; step < kBlobsPerCluster; ++step )
        {
            const float u = ( static_cast<float>( step ) + 0.5f ) / static_cast<float>( kBlobsPerCluster );
            sum += Graphic::CloudProfileHalfWidth( profile, std::pow( u, 1.7f ) );
        }
        return sum / static_cast<float>( kBlobsPerCluster );
    }

    float CloudClusterTowerFootprintRadii( const Graphic::CloudVerticalProfile& profile )
    {
        // THE CALIBRATED LINE IS KEPT AND THE CURVE IS MAPPED ONTO IT, rather than the quadrature being
        // re-run over the authored curve — and that is a MEASURED refusal, not an economy.
        //
        // Re-deriving the footprint from the curve directly was built and measured first: an independent
        // grid quadrature over this file's own layout (six lobes, golden angle, disc 0.48 x (1 - 0.55 t),
        // wobble on [0.85, 1.15], displacement to 0.18 radii) reproduces the two constants below to
        // 0.2 per cent — 0.9614 / 0.9040 against 0.9594 / 0.9051 — which is what makes the generalisation
        // legitimate at all. What it also has is REALISATION NOISE: the answer moves 0.5 per cent between
        // one set of wobble draws and another (0.9614 at 16 realisations, 0.9531 at 32, 0.9548 at 48), so
        // a runtime quadrature would make a calibrated constant depend on an arbitrary seed count. The
        // committed constants were measured once, carefully, and they are better numbers than anything
        // this can compute per bake. Revisit if the layout's lobe count or disc law changes, which is what
        // would make the constants stale rather than merely old.
        //
        // THE SCALAR IS THE MEAN LOBE WIDTH, because that is the only thing about the curve the footprint
        // can see: the projected union is a function of the six lobe radii, and the calibrated line
        // already says how that union moves as those radii shrink together. A curve that re-expresses a
        // taper therefore prices EXACTLY as that taper did, which is what keeps the version-3 library
        // rendering the version-2 sky.
        const float atNoTaper   = CloudProfileMeanHalfWidth( Graphic::CloudProfileFromTaper( 0.0f ) );
        const float atFullTaper = CloudProfileMeanHalfWidth( Graphic::CloudProfileFromTaper( 1.0f ) );

        const float mean = CloudProfileMeanHalfWidth( profile );

        // THE SPAN IS NEGATIVE AND THE GUARD HAS TO KNOW IT: a taper NARROWS the mean half-width, so
        // `atFullTaper` is below `atNoTaper` (0.4647 against 0.5610). A `std::max` against a small
        // POSITIVE epsilon here — which is what was written first — never returns the span at all, and the
        // equivalent taper comes out scaled by about -96 000. It was not a subtle failure and it was not
        // found by reading: `TheTowersFootprintConstantsSayWhatTheLayoutActuallyDoes` predicted a footprint
        // ratio of 6.2466 against a sky that said 0.9341, and three further tests went red behind it
        // because the gain had collapsed to 1 for every type. The guard is on the MAGNITUDE.
        const float span     = atFullTaper - atNoTaper;
        const float safeSpan = std::abs( span ) > 1e-6f ? span : -1e-6f;

        // NOT CLAMPED TO [0, 1], and the extrapolation is the point. A curve wider than anything the old
        // law could reach is a legal thing to author now, and it covers more sky than a full-width tower —
        // so it must be priced as covering more, or the Coverage slider starts lying again in the one
        // direction the new capability opens. The line is monotone, so the extension is monotone too.
        const float equivalentTaper = ( mean - atNoTaper ) / safeSpan;

        const float radii = kTowerFootprintAtNoTaper +
                            ( kTowerFootprintAtFullTaper - kTowerFootprintAtNoTaper ) * equivalentTaper;

        // A FLOOR AND NOT A CLAMP TO THE OLD RANGE: a curve of all zeroes is a type that draws no tower,
        // and the gain that divides by this must not divide by zero. Anything at or under a hundredth of a
        // cluster radius is that type.
        return std::max( radii, 0.01f );
    }

    float CloudClusterFootprintGain( const Graphic::CloudTypeShape& shape )
    {
        // THE SAME TEST THE EMISSION MAKES, and it has to be the same one: a canopy this function priced
        // and the emission then declined to place would shrink every storm in the sky for nothing. It is
        // now literally the same call rather than the same digits typed a second time — this comment
        // promised an agreement that three hand-written copies could only ever approximate, and the third
        // copy (Graphic::CloudTypeTopKm) had already drifted.
        if ( !Graphic::CloudTypeHasAnvil( shape ) )
            return 1.0f;

        // THE CANOPY'S FOOTPRINT IS EXACT AND NOT ESTIMATED, because it is ONE solid ellipse rather than a
        // union of lobes: `pi * a * b` with the two radii the emission writes below, so the equivalent
        // radius is their geometric mean. The lattice's `stretch` multiplies one and divides the other and
        // therefore cancels out of it exactly — which is why an anisotropic storm needs no second term
        // here, and is the same argument §SIL made for sizing a cluster by the cell's geometric mean.
        const float strength = std::clamp( shape.AnvilStrength, 0.0f, 1.0f );
        const float anvil    = ( 1.0f + kAnvilSpreadPerStrength * strength ) * std::sqrt( kAnvilAcrossOverAlong );

        const float tower = CloudClusterTowerFootprintRadii( shape.Profile );

        // FLOORED AT ONE, and the floor is a statement rather than a guard: a canopy narrower than the
        // tower it caps sits INSIDE the tower's own silhouette and costs the sky nothing at all, so there
        // is nothing to pay for. Widening the cluster to "compensate" for it would be this file inventing
        // cloud the type never asked for.
        return std::max( 1.0f, anvil / std::max( tower, 1e-3f ) );
    }

    bool CloudProceduralParamsEqual( const CloudProceduralFieldParams& a, const CloudProceduralFieldParams& b )
    {
        // THE GRID IS PART OF THE ANSWER, not of the request. Two volumes of different resolutions over one
        // region are different bytes, so a view that changes its budget must re-bake — without this line a
        // preview would keep marching the grid it was baked at before the budget moved, and the setting
        // would be dead in the §1.3 sense while looking wired.
        if ( a.VolumeSideVoxels != b.VolumeSideVoxels )
            return false;

        if ( a.RegionSizeKm != b.RegionSizeKm || a.LayerBottomKm != b.LayerBottomKm ||
             a.LayerThicknessKm != b.LayerThicknessKm || a.BlendRadiusKm != b.BlendRadiusKm ||
             a.ProfileDepthKm != b.ProfileDepthKm || a.Coverage != b.Coverage ||
             a.CoverageContrast != b.CoverageContrast || a.Seed != b.Seed || a.WindAxis != b.WindAxis ||
             a.ResolvableChordKm != b.ResolvableChordKm )
            return false;

        if ( a.PlacementDensity != b.PlacementDensity || a.PlacementScatter != b.PlacementScatter ||
             a.PlacementSizeVariety != b.PlacementSizeVariety || a.PatchStrength != b.PatchStrength ||
             a.PatchTileKm != b.PatchTileKm )
            return false;

        // THE PAINTING IS COMPARED BY ITS CONTENT HASH AND NOT BY ITS POINTER, and the difference matters
        // in both directions. By pointer, a hot reload into a different allocation with identical pixels
        // would re-bake two million voxels for nothing; by pixels, every slider move would memcmp up to
        // five megabytes. The hash is that number stated once, and it is the CRC the container already
        // verified — so "the painting changed" and "the file's checksum changed" cannot come apart.
        //
        // Null on either side reads as 0, which is the same 0 an unpainted layer carries: binding no layout
        // and binding none again is not a change, and an unpainted sky is never re-baked by this line.
        // BOTH SLOTS, SEPARATELY. Combining the two hashes into one number would make swapping the pattern
        // for the mask's painting and the mask for the pattern's look like no change at all — the same
        // "two orderings, one checksum" trap task O-3 measured on the noise sheet, where reversing the tile
        // order in both codecs at once left the round trip perfect and the layout agreement unstated.
        const uint32_t patternHashA = a.PatternSource ? a.PatternSource->ContentHash : 0u;
        const uint32_t patternHashB = b.PatternSource ? b.PatternSource->ContentHash : 0u;
        if ( patternHashA != patternHashB )
            return false;

        const uint32_t maskHashA = a.MaskSource ? a.MaskSource->ContentHash : 0u;
        const uint32_t maskHashB = b.MaskSource ? b.MaskSource->ContentHash : 0u;
        if ( maskHashA != maskHashB )
            return false;

        // AND THE PLACEMENT ONLY WHEN THERE IS SOMETHING TO PLACE. With no painting bound, none of those
        // five numbers reaches a single lump — CloudCellCoverage does not read them — so comparing them
        // would call a rebake of two million voxels for a slider that provably changed nothing. That is not
        // a saving for its own sake: an artist dragging Layout Repeats on an unpainted layer would stall
        // the editor for seconds per frame, and the cause would look like the region shifting.
        //
        // The test that walks every field asserts these five on a base that HAS a painting, which is the
        // only state in which they mean anything.
        if ( ( patternHashA != 0u || maskHashA != 0u ) &&
             !CloudLayoutPlacementEqual( a.LayoutPlacement, b.LayoutPlacement ) )
            return false;

        if ( a.Species.size() != b.Species.size() )
            return false;

        for ( size_t slot = 0; slot < a.Species.size(); ++slot )
        {
            if ( a.Species[slot].CellKm != b.Species[slot].CellKm ||
                 a.Species[slot].Anisotropy != b.Species[slot].Anisotropy )
                return false;

            // The SHAPE decides where the lumps go and how tall they are, so a type edited in place — which
            // the renderer's generation counter already catches — and a type whose numbers were reached
            // some other way both have to re-bake.
            //
            // COMPARED AS VALUES, NOT AS BYTES. This was a memcmp under a comment claiming CloudTypeShape
            // is "a flat aggregate of floats with no padding". Padding was never the argument that mattered:
            // a FLOAT has no unique object representation whatever the layout, so -0.0f and 0.0f — one
            // authored number, two bit patterns — compared as DIFFERENT and forced a re-bake of an
            // unchanged sky, while two distinct NaNs with equal bits compared as the same shape. The type
            // already carries a defaulted operator== for exactly this question (CloudTypeShape.hpp), which
            // is also the one the documents' GetDiskState uses, so the two can no longer disagree.
            if ( !( a.Species[slot].Shape == b.Species[slot].Shape ) )
            {
                return false;
            }
        }

        return true;
    }

    namespace
    {
        // A NEW FIELD HAS TO BE CONSIDERED HERE, not silently left out of the key: these sizes are the field
        // lists SerializeCloudProceduralBakeInputs writes. Growing either struct fails the build right here.
        static_assert( sizeof( Graphic::CloudTypeShape ) ==
                            ( 13u + Graphic::kCloudProfileSamples ) * sizeof( float ),
                       "CloudTypeShape gained a field: add it to SerializeCloudProceduralBakeInputs" );
        static_assert( sizeof( CloudLayoutPlacement ) == 2u * sizeof( uint32_t ) + 4u * sizeof( float ),
                       "CloudLayoutPlacement gained a field: add it to SerializeCloudProceduralBakeInputs" );

        void KeyU32( std::string& out, const uint32_t value )
        {
            for ( uint32_t shift = 0; shift < 32; shift += 8 )
                out.push_back( static_cast<char>( ( value >> shift ) & 0xffu ) );
        }

        // By VALUE, like CloudProceduralParamsEqual: -0 and +0 are one authored number and one key.
        void KeyF32( std::string& out, const float value )
        {
            KeyU32( out, std::bit_cast<uint32_t>( value == 0.0f ? 0.0f : value ) );
        }
    } // namespace

    std::string SerializeCloudProceduralBakeInputs( const CloudProceduralFieldParams& params,
                                                    const glm::vec2&                  regionOriginKm )
    {
        std::string out;
        KeyF32( out, regionOriginKm.x );
        KeyF32( out, regionOriginKm.y );

        KeyU32( out, params.VolumeSideVoxels );
        KeyF32( out, params.RegionSizeKm );
        KeyF32( out, params.LayerBottomKm );
        KeyF32( out, params.LayerThicknessKm );
        KeyF32( out, params.BlendRadiusKm );
        KeyF32( out, params.ProfileDepthKm );
        KeyF32( out, params.Coverage );
        KeyF32( out, params.CoverageContrast );
        KeyU32( out, params.Seed );
        KeyF32( out, params.WindAxis.x );
        KeyF32( out, params.WindAxis.y );
        KeyF32( out, params.ResolvableChordKm );

        KeyF32( out, params.PlacementDensity );
        KeyF32( out, params.PlacementScatter );
        KeyF32( out, params.PlacementSizeVariety );
        KeyF32( out, params.PatchStrength );
        KeyF32( out, params.PatchTileKm );

        const uint32_t patternHash = params.PatternSource ? params.PatternSource->ContentHash : 0u;
        const uint32_t maskHash    = params.MaskSource ? params.MaskSource->ContentHash : 0u;
        KeyU32( out, patternHash );
        KeyU32( out, maskHash );

        // Only with a painting bound, exactly as CloudProceduralParamsEqual: unpainted, the bake never reads
        // these five, and keying them would make an unchanged sky miss.
        if ( patternHash != 0u || maskHash != 0u )
        {
            const CloudLayoutPlacement& placement = params.LayoutPlacement;
            KeyU32( out, placement.RepeatsPerRegion );
            KeyU32( out, placement.QuarterTurns );
            KeyF32( out, placement.OffsetKm.x );
            KeyF32( out, placement.OffsetKm.y );
            KeyF32( out, placement.PatternStrength );
            KeyF32( out, placement.MaskStrength );
        }

        KeyU32( out, static_cast<uint32_t>( params.Species.size() ) );
        for ( const CloudProceduralSpecies& species : params.Species )
        {
            KeyF32( out, species.CellKm );
            KeyF32( out, species.Anisotropy );

            const Graphic::CloudTypeShape& shape = species.Shape;
            KeyF32( out, shape.BaseAltitudeKm );
            KeyF32( out, shape.TopAltitudeKm );
            KeyF32( out, shape.EdgeTopFraction );
            KeyF32( out, shape.BaseRampFraction );
            for ( const float halfWidth : shape.Profile.HalfWidth )
                KeyF32( out, halfWidth );
            KeyF32( out, shape.AnvilAltitudeKm );
            KeyF32( out, shape.AnvilThicknessKm );
            KeyF32( out, shape.AnvilStrength );
            KeyF32( out, shape.DetailCharacter );
            KeyF32( out, shape.DetailFactor );
            KeyF32( out, shape.DensityFactor );
            KeyF32( out, shape.ExtinctionFactor );
            KeyF32( out, shape.PlacementScale );
            KeyF32( out, shape.PlacementAnisotropy );
        }
        return out;
    }

    uint64_t CloudProceduralVolumeCacheKey( const CloudProceduralFieldParams& params,
                                            const glm::vec2& regionOriginKm, const Common::DDC::Deriver& deriver )
    {
        const std::string inputs = SerializeCloudProceduralBakeInputs( params, regionOriginKm );
        return Common::DDC::MakeKey( deriver, 0u, inputs.data(), inputs.size() );
    }

    Common::ResultStr<CloudProceduralCachedBake>
    BakeCloudProceduralVolumeCached( const CloudProceduralFieldParams& params, const glm::vec2& regionOriginKm,
                                     const CloudProceduralBakeProgressFn& onProgress )
    {
        CloudProceduralCachedBake result;
        result.Key = CloudProceduralVolumeCacheKey( params, regionOriginKm );

        if ( auto hit = Common::DDC::Get( kCloudModellingDeriver, result.Key ); hit.has_value() )
        {
            const uint64_t expected = CloudProceduralVoxelBytes( params.VolumeSideVoxels );
            if ( hit->size() != expected )
                return Common::MakeFormattedError<CloudProceduralCachedBake>(
                     "the cached modelling volume '{}' holds {} bytes where a {}-voxel grid is {} — the entry is "
                     "damaged; delete it to re-bake",
                     Common::DDC::PathFor( kCloudModellingDeriver, result.Key ).string(), hit->size(),
                     params.VolumeSideVoxels, expected );
            result.Voxels.assign( hit->begin(), hit->end() );
            result.FromCache = true;
            return Common::MakeSuccess( std::move( result ) );
        }

        auto baked = BakeCloudProceduralVolume( params, regionOriginKm, onProgress );
        if ( !baked.IsSuccess() )
            return Common::MakeError<CloudProceduralCachedBake>( baked.GetError() );
        result.Voxels = std::move( baked.GetValue() );

        const std::string_view bytes( reinterpret_cast<const char*>( result.Voxels.data() ),
                                      result.Voxels.size() );
        if ( auto put = Common::DDC::Put( kCloudModellingDeriver, result.Key, bytes ); !put.IsSuccess() )
            result.CacheWriteError = put.GetError();
        return Common::MakeSuccess( std::move( result ) );
    }

    Common::BoolResultStr ValidateCloudProceduralParams( const CloudProceduralFieldParams& params )
    {
        if ( !( params.RegionSizeKm > 0.0f ) || !std::isfinite( params.RegionSizeKm ) )
            return Common::MakeFormattedError<bool>( "region size must be a positive length, got {} km",
                                                     params.RegionSizeKm );

        if ( !( params.LayerThicknessKm > 0.0f ) || !std::isfinite( params.LayerThicknessKm ) )
            return Common::MakeFormattedError<bool>( "layer thickness must be a positive length, got {} km",
                                                     params.LayerThicknessKm );

        if ( !std::isfinite( params.LayerBottomKm ) )
            return Common::MakeFormattedError<bool>( "layer bottom altitude is not finite, got {} km",
                                                     params.LayerBottomKm );

        if ( !( params.BlendRadiusKm > 0.0f ) || !std::isfinite( params.BlendRadiusKm ) )
            return Common::MakeFormattedError<bool>( "blend radius must be a positive length, got {} km",
                                                     params.BlendRadiusKm );

        if ( !( params.ProfileDepthKm > 0.0f ) || !std::isfinite( params.ProfileDepthKm ) )
            return Common::MakeFormattedError<bool>( "profile depth must be a positive length, got {} km",
                                                     params.ProfileDepthKm );

        if ( !( params.ResolvableChordKm > 0.0f ) || !std::isfinite( params.ResolvableChordKm ) )
            return Common::MakeFormattedError<bool>(
                 "the march's resolvable chord must be a positive length, got {} km — it is the bound every "
                 "lump is sized against and a zero would let the generator place structure no ray can find",
                 params.ResolvableChordKm );

        // THE FOUR PLACEMENT NUMBERS, AND THE DENSITY IS THE ONE WITH TEETH. A density is the mean number
        // of clusters per cell and the cost of a bake is linear in the count, so a mistyped 250 is a bake
        // of three hundred thousand lumps that never returns rather than a sky that looks wrong. The
        // ceiling is where the cost stops being a stall and starts being a hang: at the shipped 3 km cell a
        // region holds 256 cells, so eight clusters each is 2048 of them and about eight seconds in Debug.
        if ( !std::isfinite( params.PlacementDensity ) || params.PlacementDensity < 0.0f ||
             params.PlacementDensity > 8.0f )
            return Common::MakeFormattedError<bool>(
                 "placement density must be a mean count between 0 and 8 clusters per cell, got {} — the "
                 "bake's cost is linear in the count and a region holds hundreds of cells",
                 params.PlacementDensity );

        if ( !std::isfinite( params.PlacementScatter ) || params.PlacementScatter < 0.0f ||
             params.PlacementScatter > 4.0f )
            return Common::MakeFormattedError<bool>(
                 "placement scatter is measured in CELLS and must be between 0 and 4, got {}",
                 params.PlacementScatter );

        if ( !std::isfinite( params.PlacementSizeVariety ) || params.PlacementSizeVariety < 0.0f ||
             params.PlacementSizeVariety > 1.0f )
            return Common::MakeFormattedError<bool>( "placement size variety is a fraction 0..1, got {}",
                                                     params.PlacementSizeVariety );

        if ( !std::isfinite( params.PatchStrength ) || params.PatchStrength < 0.0f || params.PatchStrength > 1.0f )
            return Common::MakeFormattedError<bool>( "patch strength is a fraction 0..1, got {}",
                                                     params.PatchStrength );

        if ( params.Species.empty() )
            return Common::MakeError<bool>( "a layer with no species in it has nothing to place; the renderer "
                                            "resolves at least one before it asks for a bake" );

        if ( params.Species.size() > Graphic::kCloudSpeciesSlots )
            return Common::MakeFormattedError<bool>(
                 "{} species were given but a volume has {} channels, one per species", params.Species.size(),
                 Graphic::kCloudSpeciesSlots );

        // THE REGION AGAINST THE MARCH, and it is the relation this phase is most likely to break. A voxel
        // is RegionSize/Width across, trilinear filtering cannot express a feature narrower than two of
        // them, and the march searches at ResolvableChordKm — so a region small enough to make the voxel
        // finer than half that chord fills the volume with structure the ray finds only when its jitter
        // happens to land on it, which is the definition of speckle.
        //
        // THE SIDE ITSELF IS CHECKED FIRST, because it is a parameter now and a zero would divide by zero
        // three lines down while a million would ask for a terabyte. The ceiling is the shipped default:
        // the volume is what a view marches, and raising it above 256 was BUILT, MEASURED at +1.7 m of
        // silhouette for four times the memory, and refused (see the header). What this range exists for is
        // the other direction — an asset preview buying its frame rate back.
        if ( params.VolumeSideVoxels < kCloudProceduralVolumeSideMin ||
             params.VolumeSideVoxels > kCloudProceduralVolumeSide )
            return Common::MakeFormattedError<bool>(
                 "a volume side of {} voxels is outside the {}..{} this subsystem bakes", params.VolumeSideVoxels,
                 kCloudProceduralVolumeSideMin, kCloudProceduralVolumeSide );

        const float voxelKm = params.RegionSizeKm / static_cast<float>( params.VolumeSideVoxels );
        if ( 2.0f * voxelKm < params.ResolvableChordKm )
            return Common::MakeFormattedError<bool>(
                 "a region of {:.1f} km over {} voxels gives a voxel of {:.0f} m, whose finest expressible "
                 "feature is {:.0f} m — thinner than the {:.0f} m the march can be relied on to find. Either "
                 "the region grows or the march steps finer (CLOUD_DISTANCE_TO_MAX_STEPS_KM)",
                 params.RegionSizeKm, params.VolumeSideVoxels, voxelKm * 1000.0f, 2.0f * voxelKm * 1000.0f,
                 params.ResolvableChordKm * 1000.0f );

        for ( size_t slot = 0; slot < params.Species.size(); ++slot )
        {
            const CloudProceduralSpecies& species = params.Species[slot];

            if ( !( species.CellKm > 0.0f ) || !std::isfinite( species.CellKm ) )
                return Common::MakeFormattedError<bool>( "species {} has a cell of {} km, which is not a length",
                                                         slot, species.CellKm );

            if ( !( species.Anisotropy > 0.0f ) || !std::isfinite( species.Anisotropy ) )
                return Common::MakeFormattedError<bool>(
                     "species {} has an anisotropy of {}, which is not a ratio", slot, species.Anisotropy );

            if ( !( species.Shape.TopAltitudeKm > species.Shape.BaseAltitudeKm ) )
                return Common::MakeFormattedError<bool>(
                     "species {} has its top at {} km and its base at {} km — a band with no height in it "
                     "cannot hold a lump",
                     slot, species.Shape.TopAltitudeKm, species.Shape.BaseAltitudeKm );
        }

        // THE PATCH MUST BE COARSER THAN THE LATTICE OR IT IS A SECOND LATTICE. Its whole purpose is the
        // slow part of the sky — where the weather is busy and where it is clear — and a modulation whose
        // period is near a cell's would decide cells one by one, which is a checkerboard rather than a
        // weather system. Only checked when the modulation is actually on, so that a layer with it off is
        // not refused for a number nothing reads.
        if ( params.PatchStrength > 1e-4f )
        {
            if ( !std::isfinite( params.PatchTileKm ) || !( params.PatchTileKm > 0.0f ) )
                return Common::MakeFormattedError<bool>( "patch tile must be a positive length, got {} km",
                                                         params.PatchTileKm );

            for ( const CloudProceduralSpecies& species : params.Species )
            {
                const glm::vec2 extent = CloudProceduralCellExtentKm( params, species );
                const float     longer = std::max( extent.x, extent.y );
                if ( params.PatchTileKm < 3.0f * longer )
                    return Common::MakeFormattedError<bool>(
                         "a patch tile of {:.1f} km against a cell of {:.1f} km is only {:.1f} cells across "
                         "— the modulation would decide cells one at a time instead of regions of sky. "
                         "Either the patch grows or the weather tile shrinks",
                         params.PatchTileKm, longer, params.PatchTileKm / longer );
            }
        }

        if ( auto layout = ValidateCloudProceduralLayout( params ); !layout )
            return layout;

        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr ValidateCloudProceduralLayout( const CloudProceduralFieldParams& params )
    {
        // THE PLACEMENT IS CHECKED WHETHER OR NOT A PAINTING IS BOUND, and that is deliberate: a repeat
        // count of zero is a division by zero and a rotation of 7 is a lattice that does not map onto
        // itself, and neither becomes safe because the slot happens to be empty today. A number that is
        // only validated when it is read is a number that goes wrong the first time somebody fills the
        // slot — which is the worst moment for it.
        if ( auto placement = ValidateCloudLayoutPlacement( params.LayoutPlacement ); !placement )
            return placement;

        for ( const CloudLayoutTable table : { CloudLayoutTable::Pattern, CloudLayoutTable::Mask } )
            if ( auto one = ValidateCloudProceduralLayoutTable( params, table ); !one )
                return one;

        return Common::MakeSuccess( true );
    }

    const char* CloudLayoutTableName( CloudLayoutTable table )
    {
        return table == CloudLayoutTable::Pattern ? "pattern" : "mask";
    }

    Common::BoolResultStr ValidateCloudProceduralLayoutTable( const CloudProceduralFieldParams& params,
                                                              CloudLayoutTable                  table )
    {
        const CloudLayoutData* layout =
             table == CloudLayoutTable::Pattern ? params.PatternSource.get() : params.MaskSource.get();

        if ( layout == nullptr )
            return Common::MakeSuccess( true );

        // The PAINTING is checked when there is one. A layout that reached here unusable would place its
        // clouds from whatever bytes happened to be in the vectors, and the symptom would be a sky that is
        // merely not the one that was painted.
        if ( auto valid = ValidateCloudLayoutData( *layout ); !valid )
            return Common::MakeFormattedError<bool>( "the layout bound to the {} input is unusable: {}",
                                                     CloudLayoutTableName( table ), valid.GetError() );

        // THE SLOT MUST BE FED THE TABLE IT READS. A `.dclayout` carrying only a mask, dropped on the
        // pattern input, is a slot an artist fills and never sees anything from — the exact dead-setting
        // shape the contract's §1.3 names. It is said here, once, rather than discovered as a sky that did
        // not change.
        const bool hasTable = table == CloudLayoutTable::Pattern ? layout->HasPattern() : layout->HasMask();
        if ( !hasTable )
            return Common::MakeFormattedError<bool>(
                 "the layout bound to the {} input carries no {} table, so that slot decides nothing. It has "
                 "{} and {}",
                 CloudLayoutTableName( table ), CloudLayoutTableName( table ),
                 layout->HasPattern() ? "a pattern" : "no pattern", layout->HasMask() ? "a mask" : "no mask" );

        // THE PAINTING MUST BE ABLE TO DESCRIBE A CELL, and this is the relation that says so. One texel of
        // the painting spans `RegionSize / (Repeats * Resolution)` kilometres; if that is coarser than the
        // lattice cell the painting cannot distinguish two neighbouring cells at all and every shape in it
        // is rounded to the cell grid — the artist draws a letter and the sky shows a staircase. Checked
        // against the FINEST species, because it is the one that loses most.
        const float texelKm = params.RegionSizeKm /
                              ( static_cast<float>( std::max( params.LayoutPlacement.RepeatsPerRegion, 1u ) ) *
                                static_cast<float>( std::max( layout->Resolution, 1u ) ) );

        for ( const CloudProceduralSpecies& species : params.Species )
        {
            const glm::vec2 extent  = CloudProceduralCellExtentKm( params, species );
            const float     shorter = std::min( extent.x, extent.y );
            if ( texelKm > shorter )
                return Common::MakeFormattedError<bool>(
                     "one {} texel is {:.2f} km against a cell of {:.2f} km, so the painting cannot tell "
                     "two neighbouring cells apart and every shape in it would be rounded to the lattice. "
                     "Either the layout gains resolution, Layout Repeats rises, or the region shrinks",
                     CloudLayoutTableName( table ), texelKm, shorter );
        }

        return Common::MakeSuccess( true );
    }

    float CloudProceduralSnapKm( const CloudProceduralFieldParams& params )
    {
        // THE COARSEST CELL IN THE LAYER, because the snap has to be a whole number of cells for EVERY
        // species at once — a shift of half a cell would re-roll that species' clusters and the sky would
        // boil where it should have stood still.
        //
        // Floored at a kilometre so that a layer of very fine species does not ask for a rebake every few
        // hundred metres of camera travel: below that the cost of the bake dominates what it buys, and the
        // invariance the snap protects is already exact for anything that stays in the region.
        float coarsest = 1.0f;
        for ( const CloudProceduralSpecies& species : params.Species )
        {
            const glm::vec2 extent = CloudProceduralCellExtentKm( params, species );
            coarsest               = std::max( coarsest, std::max( extent.x, extent.y ) );
        }
        return coarsest;
    }

    glm::vec2 CloudProceduralRegionOriginKm( const CloudProceduralFieldParams& params, float cameraXKm,
                                             float cameraZKm )
    {
        const float snap = CloudProceduralSnapKm( params );
        const float half = params.RegionSizeKm * 0.5f;

        // FLOOR AND NOT ROUND, so that the origin is a monotone step function of the camera: rounding puts
        // the step at the half-cell and gives the same answer either side of it, which is fine, but the
        // floor makes "which snap cell is the camera in" a single division that a test can restate.
        const float x = std::floor( ( cameraXKm - half ) / snap ) * snap;
        const float z = std::floor( ( cameraZKm - half ) / snap ) * snap;
        return glm::vec2( x, z );
    }

    std::vector<CloudModellingBlob> GenerateCloudProceduralBlobs( const CloudProceduralFieldParams& params,
                                                                  uint32_t slot, const glm::vec2& regionOriginKm )
    {
        std::vector<CloudModellingBlob> blobs;

        if ( slot >= params.Species.size() )
            return blobs;

        const CloudProceduralSpecies&  species = params.Species[slot];
        const Graphic::CloudTypeShape& shape   = species.Shape;

        glm::vec2 along;
        glm::vec2 across;
        WindFrame( params.WindAxis, along, across );

        const glm::vec2 extent = CloudProceduralCellExtentKm( params, species );

        // THE SET OF CELLS IS EXACTLY ONE PERIOD'S WORTH — those whose CENTRE lies in the region — and not
        // one cell more. The bake wraps every lump across the region's faces to make the volume periodic,
        // so generating the neighbouring cells as well would place each of them TWICE: once as itself and
        // once as the wrap of the cell a period away.
        //
        // The lattice is laid out in the wind's frame and the region is axis-aligned, so the range of
        // indices is found by mapping the region's four corners into that frame and taking the extremes.
        // An index range that is a superset costs a rejected containment test per cell and never a wrong
        // cloud; a subset would cut a band off the sky.
        const float side = params.RegionSizeKm;

        float minU = 0.0f;
        float maxU = 0.0f;
        float minV = 0.0f;
        float maxV = 0.0f;
        for ( int corner = 0; corner < 4; ++corner )
        {
            const glm::vec2 point =
                 regionOriginKm + glm::vec2( ( corner & 1 ) ? side : 0.0f, ( corner & 2 ) ? side : 0.0f );
            const float u = point.x * along.x + point.y * along.y;
            const float v = point.x * across.x + point.y * across.y;

            minU = ( corner == 0 ) ? u : std::min( minU, u );
            maxU = ( corner == 0 ) ? u : std::max( maxU, u );
            minV = ( corner == 0 ) ? v : std::min( minV, v );
            maxV = ( corner == 0 ) ? v : std::max( maxV, v );
        }

        const int32_t firstU = static_cast<int32_t>( std::floor( minU / extent.x ) ) - 1;
        const int32_t lastU  = static_cast<int32_t>( std::floor( maxU / extent.x ) ) + 1;
        const int32_t firstV = static_cast<int32_t>( std::floor( minV / extent.y ) ) - 1;
        const int32_t lastV  = static_cast<int32_t>( std::floor( maxV / extent.y ) ) + 1;

        const float bandKm = shape.TopAltitudeKm - shape.BaseAltitudeKm;

        const uint32_t stackCount = kBlobsPerCluster;

        // THE WIDEST A CLUSTER'S BASE LUMP GETS, and the number was raised from two fifths of the cell's
        // short side to eleven twentieths by MEASUREMENT: at two fifths a coverage of 0.35 put cloud over
        // four per cent of the sky, because a cluster covered about a ninth of the cell its hash had won.
        // A slider documented as "what fraction of the sky is cloud" has to mean it, and it only can if an
        // alive cell is mostly full. Above a half the clusters of two adjacent alive cells OVERLAP, which
        // is the whole point — that is where a bank of cloud comes from rather than a row of cushions.
        //
        // THE GEOMETRIC MEAN OF THE CELL'S TWO SIDES AND NOT THE SHORTER OF THEM, and the difference is
        // §SIL's first defect. `CloudProceduralCellExtentKm` holds the cell's AREA constant under
        // anisotropy — `cell * root` by `cell / root` — precisely so that stretching the lattice draws a
        // cluster out into a band instead of emptying the sky. Sizing the cluster by `min(extent)` threw
        // that away and let the sky empty as the square of the stretch: measured at Coverage 0.5, one
        // species, everything else shipped, the sky went 0.519 at anisotropy 1, 0.378 at 1.6, 0.143 at 0.2
        // and 0.089 at 8 — so the shipped cirrus, whose anisotropy IS 8, delivered a fifth of the sky its
        // own slider asked for. FOUR OF THE NINE SHIPPED TYPES were affected. The geometric mean is exactly
        // `species.CellKm` again, so the cluster is the size the artist's Placement Scale says whatever the
        // stretch does, and the stretch is spent on the cluster's SHAPE below instead of on its area.
        // AND THE TYPE'S OWN CANOPY IS PAID FOR HERE, which is §CB's whole content. The Coverage mapping
        // below is a statement about the AREA one cluster covers, and this file already holds that area
        // still against the three things that could move it — the density, the size spread and the
        // anisotropy. The fourth was the ANVIL: a canopy 1.70 times the tower's footprint covers 2.9 times
        // the sky, and nothing priced it, so the cumulonimbus delivered 0.856 of the sky for a slider of
        // 0.5 while every other genus in the library sat within 0.06 of its setting.
        //
        // IT IS THE CLUSTER THAT SHRINKS AND NOT THE CANOPY, and the reason is what `AnvilStrength` means:
        // it is authored as how far the canopy spreads BEYOND its tower, so scaling the canopy alone would
        // silently redefine the artist's number. A factor here leaves the storm's proportions exactly as
        // its asset states them and changes only how much sky one storm is worth.
        //
        // ABOVE THE MARCH'S FLOOR, still: the floor is what keeps a cluster findable by the ray, and a
        // compensation that pushed a body under it would trade a lying slider for speckle.
        const float cellMeanKm    = std::sqrt( extent.x * extent.y );
        const float footprintGain = CloudClusterFootprintGain( shape );
        const float baseRadiusKm = std::max( 0.72f * cellMeanKm / footprintGain, 0.5f * params.ResolvableChordKm );

        // AND THE STRETCH THE CELL NO LONGER SPENDS ON ITS AREA IS SPENT ON THE CLUSTER'S SHAPE. A cluster
        // is drawn out along the wind by the same factor its cell is, so a cluster covers the same fraction
        // of its own cell at every anisotropy — which is what makes the Coverage slider mean the sky for a
        // cirrus as well as for a cumulus, and it is the relation Desert/Tests/Engine/CloudPlacementSpectrum
        // asserts by measuring the cover at four settings of it.
        //
        // TAKEN FROM THE EXTENTS AND NOT FROM `species.Anisotropy`, deliberately: the extents are what the
        // cell function returned, floors and all, so there is one statement of the stretch rather than two
        // that have to be kept in step — the defect class §2.3.1 names and the one this phase is fixing.
        const float stretch = std::sqrt( extent.x / std::max( extent.y, 1e-6f ) );

        // THE LUMPS TURN WITH THE LATTICE. A stretched lump whose axes stayed world-aligned would be a band
        // pointing east in a sky whose wind blows north-west. The rotation is about Y and is RIGID, so the
        // distance field stays a true distance field — the property the whole join is built on.
        //
        // `glm::quat( radians( 0, yaw, 0 ) )` maps local +X to `( cos yaw, 0, -sin yaw )`, so the yaw that
        // carries local +X onto the wind is `atan2( -along.z, along.x )`. The sign is pinned by a test that
        // asks the DISTANCE FIELD which way the lump is long rather than by this comment.
        const float yawDeg = std::atan2( -along.y, along.x ) * kDegreesPerRadian;

        const uint32_t speciesSeed = CloudSpeciesSeed( params, slot );

        // ---------------------------------------------------------------------------------------------
        // WHAT FRACTION OF THE CELLS IS ALIVE, WHICH IS NOT THE SLIDER
        // ---------------------------------------------------------------------------------------------
        //
        // The slider means what a person looking up would measure: the fraction of the SKY with cloud
        // somewhere in the column. A cell being alive is not that — a cluster does not fill its cell, two
        // neighbouring clusters overlap, and how full a cluster is depends on how deep inside the
        // threshold its own hash fell. Taken as the alive fraction directly, the slider under-delivered
        // by a factor that grew with the setting: 0.24 gave 0.105 of the sky and 0.75 gave 0.450 — and
        // the frame that came out of it had clouds on the horizon and an EMPTY ZENITH, which is the
        // defect Docs/Clouds/REVIEW_622a01a6.md names and the one the owner found by looking up.
        //
        // 0.68 IS MEASURED, on the top-down projection of the baked volume at five settings, and
        // Desert/Tests/Engine/CloudProceduralField re-measures it on every run and fails if the slider
        // and the sky part company by more than a tenth. Being a power it keeps both ends EXACT, which is
        // the property the ends were built to have: 0 stays empty and 1 stays full.
        //
        // IT IS EVALUATED INSIDE THE CELL LOOP AND NOT HERE, because the patch modulation moves the
        // threshold from place to place: the whole point of that modulation is that the sky is not equally
        // busy everywhere, so there is no single alive fraction for a region any more.

        // THE DENSITY DOES NOT ADD MATTER, IT REDISTRIBUTES IT. A cell that carries `d` clusters narrows
        // each of them by `d` to the power of kDensityCompensation, so the ground they cover between them
        // is the ground one covered. Without this line the density knob would move the sky's cover, and
        // the Coverage mapping decision D-20 re-authorised every scene against would have to be measured
        // again for every setting of it — which is a knob that silently invalidates another knob.
        const float density      = std::max( params.PlacementDensity, 0.0f );
        const float densityScale = std::pow( std::max( density, 1e-3f ), -kDensityCompensation );

        const float scatter = std::max( params.PlacementScatter, 0.0f );
        const float variety = std::clamp( params.PlacementSizeVariety, 0.0f, 1.0f );

        const uint32_t patchSeed = CloudPatchSeed( speciesSeed );

        for ( int32_t iv = firstV; iv <= lastV; ++iv )
        {
            for ( int32_t iu = firstU; iu <= lastU; ++iu )
            {
                const glm::vec2 centre = CellCentreKm( along, across, extent, iu, iv );

                // ONE PERIOD, decided on the cell's own centre. Half-open so that a centre landing exactly
                // on a face belongs to one region and not to two.
                //
                // THIS TEST IS THE INVARIANT, AND IT IS THE ONLY ONE THE WRAP NEEDS. It was once defended
                // by also confining a cluster to the cell that produced it, and that confinement is what
                // made the sky a grid. It was never what the wrap required: the bake splats every lump at
                // plus and minus one period, so a cluster displaced out through one face of the region
                // arrives back through the opposite one, and the volume stays exactly periodic however far
                // from its own site a cluster sits. What the confinement bought is named on
                // CloudProceduralFieldParams::PlacementScatter, and it is a strip at the region's edge
                // 24 km from the camera.
                const glm::vec2 local = centre - regionOriginKm;
                if ( local.x < 0.0f || local.x >= params.RegionSizeKm || local.y < 0.0f ||
                     local.y >= params.RegionSizeKm )
                    continue;

                // THE CELL'S IDENTITY IS ITS ABSOLUTE LATTICE INDEX, which is what makes the field
                // invariant under the region scrolling: a cell that is in the region before a shift and
                // after it hashes to exactly the same clusters, so nothing inside the region moves when the
                // window does.
                const uint32_t cellSeed =
                     HashCombine( HashCombine( speciesSeed, IndexWord( iu ) ), IndexWord( iv ) );

                // WHERE THE WEATHER IS BUSY AND WHERE IT IS CLEAR. Sampled at the CELL's own site rather
                // than per voxel, so it modulates whole clouds into and out of existence instead of eroding
                // their edges — which is what a weather system does and what erosion does not.
                //
                // WHICH SOURCE DECIDES IT — the artist's painting or the procedural patch — is settled
                // inside CloudCellCoverage, and it is settled in one place because two mechanisms setting
                // one number is the second path the contract forbids. The choice is NOT made here and must
                // not be: the moment this loop could apply both, the sky would answer to two sliders that
                // do not know about each other.
                const float cellCoverage = CloudCellCoverage( params, slot, patchSeed, centre );

                const float aliveFraction = std::pow( cellCoverage, 0.68f );

                // AND THE CLUSTER IS WIDENED TO PAY FOR THE PACKING THE FREE PLACEMENT COSTS. See
                // kPackingCompensation: the 0.68 above was calibrated against a placement that kept every
                // cluster near its own lattice site, which packs efficiently, and the whole cure for the
                // grid is to stop doing that. Without this line the slider would be out by fourteen points
                // at the shipped scene's setting and the suite's tenth would fail at 0.75.
                const float radiusGain = 1.0f + kPackingCompensation * cellCoverage;

                // COVERAGE ADDRESSES A FRACTION OF SKY DIRECTLY. A cell is alive when its own hash falls
                // below the slider, so 0 is exactly empty and 1 is exactly full — for any seed, any cell
                // size and any species, with no distribution to calibrate. That is the property the
                // quantile map this replaces was built to fake on a field whose spread it had to measure.
                const float draw = HashUnit( cellSeed );
                if ( draw >= aliveFraction )
                    continue;

                // AND CONTRAST IS THE WIDTH OF THE RAMP INTO IT. A cell that only just qualified grows a
                // small cluster; one well inside the threshold grows a full one. Above 1 the sky is
                // decisively cloud or decisively clear; below 1 the sizes spread out, which is what a
                // broken deck looks like.
                const float softness = ( 1.0f - std::clamp( params.Coverage, 0.0f, 1.0f ) ) /
                                            std::max( params.CoverageContrast, 1e-2f ) +
                                       0.02f;
                const float fill =
                     std::clamp( ( aliveFraction - draw ) / std::max( softness, 1e-4f ), 0.0f, 1.0f );

                // EDGE TOP FRACTION IS WHAT A SHALLOW CLUSTER LOSES. The type says how tall it is where the
                // patch has only just begun, and `fill` is how far inside the patch this cell is — so a
                // rim cell is low and flat and a core cell is a tower. That is decision D-13's whole
                // intent, carried by the placement instead of by a second axis of a table.

                // EVERY CLUSTER GETS THE WHOLE STACK, and `fullness` shrinks the BAND it is spread over
                // rather than the number of lobes in it. Cutting the count instead was measured and was
                // wrong twice over: a shallow cell came out with one or two lobes, which is a dot and not a
                // cloud, and the lobes it kept were the same size as a full cluster's, so a low cumulus
                // humilis read as a truncated congestus. Six flattened lobes over half a band is a
                // pancake — which is what a humilis IS.

                // HOW MANY CLOUDS THIS CELL HOLDS, and it is the line that removes "exactly one per cell"
                // from the field. The count is drawn per cell with the density as its mean, so the number
                // density of clouds stops being a constant of the lattice.
                const uint32_t clusters = ClusterCount( cellSeed, density );

                for ( uint32_t index = 0; index < clusters; ++index )
                {
                    // EACH CLUSTER IS ITS OWN CLOUD. Everything below hangs off this seed rather than off
                    // the cell's, so two clusters in one cell differ in place, in size and in which way
                    // their lobes spiral — otherwise a density above one would put N copies of one cloud
                    // in one place, which is one cloud with N times the arithmetic.
                    const uint32_t clusterSeed = HashCombine( cellSeed, 0x51u + index );

                    // THE CLUSTER'S DISPLACEMENT FROM ITS LATTICE SITE, in cells. At the shipped scatter it
                    // crosses into the neighbouring cell's territory, which is exactly what the lattice
                    // peak measures the absence of.
                    const glm::vec2 jitter( HashSigned( HashCombine( clusterSeed, 0x1u ) ) * extent.x * scatter,
                                            HashSigned( HashCombine( clusterSeed, 0x2u ) ) * extent.y * scatter );

                    const glm::vec2 clusterXZ = centre + along * jitter.x + across * jitter.y;

                    // HOW BIG THIS PARTICULAR CLOUD IS, and the draw is UNIFORM IN AREA. `size` is the
                    // square root of a number uniform on [1 - variety, 1 + variety], whose mean is one — so
                    // the mean area a cluster covers does not move with the setting and the Coverage
                    // mapping stays where D-20 left it. Spreading the RADIUS uniformly instead would have
                    // raised the mean area by a twelfth of the spread squared.
                    const float area =
                         1.0f - variety + 2.0f * variety * HashUnit( HashCombine( clusterSeed, 0x4u ) );
                    const float size = std::sqrt( std::max( area, 1e-4f ) );

                    // THE CLUSTER'S OVERALL HORIZONTAL HALF-EXTENT — the size of the CLOUD, not of a lobe.
                    const float clusterRadiusKm =
                         baseRadiusKm * ( 0.60f + 0.40f * fill ) * size * densityScale * radiusGain;

                    // A SMALL CLOUD IS ALSO A FLAT ONE, which is what a cumulus field looks like and what
                    // keeps a quarter-width cluster from being a full-height tower on a narrow base. The
                    // type's own Edge Top Fraction is still the floor, so a stratus stays a sheet.
                    const float shortening = std::clamp( fill * size, 0.0f, 1.0f );
                    const float fullness   = std::clamp( shape.EdgeTopFraction, 0.0f, 1.0f ) +
                                           ( 1.0f - std::clamp( shape.EdgeTopFraction, 0.0f, 1.0f ) ) * shortening;

                    // Where the spiral starts, per cluster, so that two clusters of the same fullness are
                    // not the same cloud rotated into the same place.
                    const float phase = HashUnit( HashCombine( clusterSeed, 0x3u ) ) * 6.2831853f;

                    // ---------------------------------------------------------------------------------
                    // THE STACK IS LAID OUT BEFORE IT IS EMITTED, because the band decides WHERE
                    // ---------------------------------------------------------------------------------
                    //
                    // The lump's own two radii are one quantity now (kLumpVerticalOverHorizontal), so the
                    // band has stopped deciding how TALL a lump is and decides instead how far the stack
                    // may travel: the first lump sits its own radius above the base and the last sits its
                    // own radius below the top, and the body's vertical envelope is therefore the type's
                    // band and nothing else. That is the second half of §RW2's finding — the old stack
                    // stood `band * fullness + 2 * lumpRadius` tall, which is a body that pokes out of the
                    // altitudes its own asset declares.
                    const float bandFullKm = bandKm * fullness;

                    float lumpT[kBlobsPerCluster];
                    float lumpRadiusKm[kBlobsPerCluster];
                    float lumpVerticalKm[kBlobsPerCluster];
                    float lumpWobbleAlong[kBlobsPerCluster];
                    float lumpWobbleAcross[kBlobsPerCluster];

                    // TWO FLOORS, BECAUSE THEY ANSWER TWO DIFFERENT QUESTIONS — see
                    // CloudProceduralLumpFloorKm. Horizontally a lump has to survive the VOLUME, whose
                    // voxel is a fixed RegionSize/Width; vertically it has to survive the MARCH, because
                    // the volume's rows are spread over the layer and their height is therefore the
                    // layer's business rather than a constant.
                    const float lumpFloorKm  = CloudProceduralLumpFloorKm( params );
                    const float marchFloorKm = 0.5f * params.ResolvableChordKm;

                    for ( uint32_t step = 0; step < stackCount; ++step )
                    {
                        const float u = ( static_cast<float>( step ) + 0.5f ) / static_cast<float>( stackCount );
                        const float t = std::pow( u, 1.7f );

                        // THE TYPE'S OWN SILHOUETTE, READ HERE AND NOWHERE ELSE. Up to format version 2
                        // this line was `(0.62 - 0.16 t) * (1 - 0.5 * TopTaper * t)` — a product of two
                        // falling lines, hence monotone decreasing at every setting of its one knob, so a
                        // shelf, a waist or a body that widens with height could not be authored at all.
                        // The curve IS that law now, sampled into the asset.
                        //
                        // IT ENTERS HERE AND IS NOT APPLIED AFTERWARDS, which is the whole of decision
                        // D-22 and the reason the vertical still has one source of truth. Everything below
                        // this line is fed by `radius`: the lump's vertical radius is derived from it by
                        // `kLumpVerticalOverHorizontal`, the band clamp squashes THAT, and the travel is
                        // fitted to what the end lumps leave. A multiplier laid over the finished stack
                        // would have been a second vertical authority fighting the aspect-and-erosion
                        // calibration, which is exactly the defect D-22 names.
                        const float radius = clusterRadiusKm * Graphic::CloudProfileHalfWidth( shape.Profile, t );

                        // BASE RAMP FRACTION IS THE THICKNESS OF THE LOWEST LOBE against the ones above it:
                        // a type whose base fills in slowly has a thin, spreading floor and a fat body over
                        // it. It is the one authored number that reshapes a single lump, and it reshapes
                        // exactly one of them.
                        const float ramp       = std::clamp( shape.BaseRampFraction, 0.05f, 1.0f );
                        const float rampFactor = ( step == 0 ) ? ramp + ( 1.0f - ramp ) * 0.5f : 1.0f;

                        // THE WOBBLE IS DRAWN HERE, WITH THE LAYOUT, AND A TEST IS WHY. It scales the lump
                        // by up to 1.15, and drawn at emission time — where it was first written — the
                        // vertical radius the band was fitted against was not the vertical radius the lump
                        // ended up with: `EveryLumpStandsInsideItsTypesOwnBand` measured the shipped
                        // congestus reaching 5.883 km out of a 5.80 km band and 2.079 km under a 2.20 km
                        // base. A fit against a number that is then multiplied is the same two-places
                        // defect one scale smaller.
                        const uint32_t lumpSeed = HashCombine( clusterSeed, 0x100u + step );

                        lumpWobbleAlong[step]  = 0.85f + 0.3f * HashUnit( HashCombine( lumpSeed, 0xcu ) );
                        lumpWobbleAcross[step] = 0.85f + 0.3f * HashUnit( HashCombine( lumpSeed, 0xdu ) );

                        // THE BAND CLAMP IS WHERE A GENUS'S FLATNESS COMES FROM, and it is the reason the
                        // lump's aspect did not have to become a fifteenth authored number. A lump may not
                        // be taller than half the band it lives in, so a 400 m stratus deck gets 200 m
                        // lumps however wide its cell is while a 3.6 km congestus never meets the clamp at
                        // all. The type squashes its own lumps through the altitudes it already declares.
                        const float wobbleUp = std::sqrt( lumpWobbleAlong[step] * lumpWobbleAcross[step] );

                        lumpT[step]        = t;
                        lumpRadiusKm[step] = radius;
                        lumpVerticalKm[step] =
                             std::max( std::min( kLumpVerticalOverHorizontal * radius * rampFactor * wobbleUp,
                                                 0.5f * bandFullKm ),
                                       marchFloorKm );
                    }

                    // The travel is what is left of the band once both end lumps have been let in, divided
                    // by the last lump's own parameter so that the top lump's crown lands ON the top rather
                    // than short of it — `t` is a curve and not a fraction of the band.
                    const float travelKm =
                         std::max( bandFullKm - lumpVerticalKm[0] - lumpVerticalKm[stackCount - 1], 0.0f ) /
                         std::max( lumpT[stackCount - 1], 1e-4f );

                    for ( uint32_t step = 0; step < stackCount; ++step )
                    {
                        const uint32_t lumpSeed = HashCombine( clusterSeed, 0x100u + step );

                        // Where up the stack this lump sits, 0 at the base and approaching 1 at the top.
                        // BOTTOM-HEAVY, and the exponent is measured rather than chosen. Spread evenly, six
                        // lobes put one or two at the wide base and four up the narrow tower, so the base was
                        // a rosette with holes in it: a full cell measured 48 per cent covered from below when
                        // the geometry says a full cluster should cover it. A cumulus is a WIDE FLOOR with a
                        // turret or two on top, which is the same thing said about the picture and about the
                        // number.
                        const float t      = lumpT[step];
                        const float radius = lumpRadiusKm[step];

                        // THE LOBES ARE SPREAD OVER A DISC AND NOT STACKED CONCENTRICALLY, and this is the line
                        // that decides whether the sky is a cumulus field or a field of dots.
                        //
                        // The first written form displaced each lobe by a third of its OWN radius, which put
                        // every lobe of a cluster inside every other one: the join of six concentric ellipsoids
                        // is one ellipsoid, and a top-down projection of the volume came out as a scatter of
                        // round dots — the SAME defect the Alligator threshold had, arrived at from the other
                        // side. What a convective mass is made of is lobes that overlap PARTLY, so each shows
                        // its own shoulder while the body stays one connected surface.
                        //
                        // The golden angle spreads them without a pattern, and the disc narrows going up so the
                        // pile is a dome rather than a column: at the base the lobes sit half a cluster-radius
                        // out, at the top they close over the middle.
                        //
                        // HOW FAR THE LOBES OVERLAP IS THE WHOLE ARGUMENT OF PHASE Э5, so it is arithmetic and
                        // not a feel. Two lobes one golden angle apart on a circle of radius `spread` are
                        // `2 * spread * sin(68.5 deg) = 1.86 * spread` apart; with `spread = 0.42 R` that is
                        // 0.78 R against a sum of radii of 1.20 R, so they interpenetrate by 0.42 R — a third
                        // of a lobe. At the first written pair (0.52, 0.50) the same numbers were 0.97 R
                        // against 1.00 R, the lobes only TOUCHED, and the top-down projection came out as
                        // clusters of separate dots: fusion is not free just because the join can express it,
                        // the bodies have to be inside one another.
                        const float angle  = phase + 2.39996323f * static_cast<float>( step );
                        const float spread = clusterRadiusKm * 0.48f * ( 1.0f - 0.55f * t );

                        CloudModellingBlob blob;
                        blob.Primitive = CloudModellingPrimitive::Ellipsoid;

                        const float wobble = 0.18f * clusterRadiusKm;

                        // THE DISC IS AN ELLIPSE IN THE WIND'S FRAME, by the same factor the cell is. At an
                        // anisotropy of 1 `stretch` is 1 and the two axes below are the wind's own frame,
                        // which for the shipped +X wind is the world's — so an isotropic type's lobes land
                        // exactly where they always did.
                        const float offsetAlong = ( std::cos( angle ) * spread +
                                                    HashSigned( HashCombine( lumpSeed, 0xau ) ) * wobble ) *
                                                  stretch;
                        const float offsetAcross = ( std::sin( angle ) * spread +
                                                     HashSigned( HashCombine( lumpSeed, 0xbu ) ) * wobble ) /
                                                   stretch;

                        // WHERE THE LUMP SITS UP THE BAND. Clamped so that it is INSIDE the type's own
                        // altitudes on both sides — the relation the layout above exists to make true, and
                        // the one Desert/Tests/Engine/CloudPlacementSpectrum asserts lump by lump. The clamp
                        // bites only where the resolvable floor has forced a lump taller than half its band,
                        // which is a type authored thinner than the march can see.
                        const float halfBandKm = 0.5f * bandFullKm;
                        const float lowKm      = std::min( lumpVerticalKm[step], halfBandKm );
                        const float highKm     = std::max( bandFullKm - lumpVerticalKm[step], lowKm );
                        const float upKm       = std::clamp( lumpVerticalKm[0] + travelKm * t, lowKm, highKm );

                        blob.CentreKm = glm::vec3( clusterXZ.x + along.x * offsetAlong + across.x * offsetAcross,
                                                   shape.BaseAltitudeKm + upKm,
                                                   clusterXZ.y + along.y * offsetAlong + across.y * offsetAcross );

                        // THE LUMP IS NEVER THINNER THAN THE VOLUME CAN CARRY, on both horizontal axes — and
                        // that used to say "than the march can find", which is a different and smaller number.
                        // It is a clamp and not an assertion because the inputs are an artist's: a type
                        // authored with a 40 m band is a legal thing to write in a `.decloudtype`, and the
                        // honest answer is a lobe the volume can hold rather than speckle or a refusal to draw
                        // the sky.
                        //
                        // THE WOBBLE SCALES THE LUMP AND DOES NOT RESHAPE IT. Two draws vary the plan-view
                        // outline, and the vertical radius takes their GEOMETRIC MEAN — so `radii.y` over the
                        // geometric mean of the two horizontal radii is exactly kLumpVerticalOverHorizontal
                        // for every lump that the ramp and the band clamp have left alone. A lump has ONE
                        // size; that is the whole of the decision, and it is stated in a form a test can
                        // read off the emitted lumps.
                        const float floorKm = lumpFloorKm;

                        blob.RadiiKm = glm::vec3( std::max( radius * lumpWobbleAlong[step] * stretch, floorKm ),
                                                  lumpVerticalKm[step],
                                                  std::max( radius * lumpWobbleAcross[step] / stretch, floorKm ) );

                        blob.RotationDeg  = glm::vec3( 0.0f, yawDeg, 0.0f );
                        blob.Weight       = 1.0f;
                        blob.DetailType   = std::clamp( shape.DetailCharacter, 0.0f, 1.0f );
                        blob.DensityScale = 1.0f;

                        blobs.push_back( blob );
                    }

                    // THE ANVIL, and it is the shape no vertical curve could express: a lobe of cloud at the
                    // tropopause with a GAP between it and the tower that fed it. A product of two ramps has
                    // exactly one maximum for any choice of constants, which is the argument decision D-13 made
                    // for a table; a second lump makes it without a table at all.
                    //
                    // THE QUESTION IS ASKED BY Graphic::CloudTypeHasAnvil AND NOT BY TWO LITERALS HERE,
                    // because Graphic::CloudTypeTopKm asks the SAME question to decide how tall the shell
                    // has to be. While the two were spelled out separately they disagreed — the envelope
                    // grew for any strength above zero and this site drew nothing below a thousandth — and
                    // the symptom is a shell kilometres taller than anything in it, with the vertical
                    // resolution and the march's search step paying for it silently. See the note above
                    // that predicate.
                    if ( Graphic::CloudTypeHasAnvil( shape ) )
                    {
                        CloudModellingBlob anvil;
                        anvil.Primitive = CloudModellingPrimitive::Ellipsoid;
                        anvil.CentreKm  = glm::vec3( clusterXZ.x, shape.AnvilAltitudeKm, clusterXZ.y );

                        // Wider than the tower and much flatter, which is what spreading against a stable layer
                        // looks like. The strength decides how far it spreads and how much matter is in it.
                        const float spread = baseRadiusKm * ( 0.60f + 0.40f * fill ) * size * densityScale *
                                             radiusGain * ( 1.0f + kAnvilSpreadPerStrength * shape.AnvilStrength );

                        // THE CANOPY IS A LUMP AND IS FLOORED LIKE ONE, from the two floors the tower
                        // already uses rather than from a second copy of `0.5 * ResolvableChordKm` — which
                        // is what stood here, and which is the same one-quantity-stated-twice this file
                        // removes everywhere else. Its THICKNESS takes the march's floor for the reason a
                        // lump's vertical radius does: the volume's rows belong to the layer, not to a
                        // constant of the subsystem.
                        //
                        // THE CANOPY IS DRAWN OUT WITH THE CLUSTER IT CAPS, by the same `stretch`. A storm in
                        // an anisotropic lattice whose tower was a band and whose anvil was a circle would be
                        // two bodies, and the anvil's own thickness is the one radius it authors itself.
                        anvil.RadiiKm =
                             glm::vec3( std::max( spread * stretch, lumpFloorKm ),
                                        std::max( shape.AnvilThicknessKm, marchFloorKm ),
                                        std::max( spread * kAnvilAcrossOverAlong / stretch, lumpFloorKm ) );

                        anvil.RotationDeg = glm::vec3( 0.0f, yawDeg, 0.0f );
                        anvil.Weight      = 1.0f;
                        anvil.DetailType  = std::clamp( shape.DetailCharacter, 0.0f, 1.0f );
                        // The anvil is ice and is THINNER than the tower, and this is the one place a lump's
                        // own density scale is not 1: the softmax weights of the join turn it into a smooth
                        // per-voxel field over the crease between the anvil and the body.
                        anvil.DensityScale = std::clamp( shape.AnvilStrength, 0.0f, 1.0f );

                        blobs.push_back( anvil );
                    }
                }
            }
        }

        // CANONICAL ORDER, for the reason phase Э4 measured: the join is commutative and associative in
        // real arithmetic and neither in floating point, so a bake whose bytes must not depend on the order
        // its lumps were emitted in sorts first. Here the emission order is a loop over a lattice, which is
        // stable — but it changes when the wind turns the frame, and a field that shifts by a 255th when
        // the wind direction is nudged is exactly the class of drift the sort removes.
        SortCloudModellingBlobs( blobs );

        return blobs;
    }

    float EvaluateCloudProceduralProfile( const CloudProceduralFieldParams&      params,
                                          const std::vector<CloudModellingBlob>& blobs, const glm::vec3& pointKm )
    {
        if ( blobs.empty() )
            return 0.0f;

        const float invBlend = 1.0f / std::max( params.BlendRadiusKm, 1e-6f );

        float nearest = 0.0f;
        for ( size_t k = 0; k < blobs.size(); ++k )
        {
            const float distance = CloudModellingBlobDistanceKm( PrepareCloudModellingBlob( blobs[k] ), pointKm );
            nearest              = ( k == 0 ) ? distance : std::min( nearest, distance );
        }

        float sum = 0.0f;
        for ( const CloudModellingBlob& blob : blobs )
        {
            const float distance = CloudModellingBlobDistanceKm( PrepareCloudModellingBlob( blob ), pointKm );
            sum += CloudModellingJoinTerm( blob.Weight, distance, nearest, invBlend );
        }

        const float joined = CloudModellingJoinKm( nearest, sum, params.BlendRadiusKm );
        return std::clamp( -joined / std::max( params.ProfileDepthKm, 1e-6f ), 0.0f, 1.0f );
    }

    size_t CountCloudProceduralBlobs( const CloudProceduralFieldParams& params, const glm::vec2& regionOriginKm )
    {
        size_t total = 0;
        for ( uint32_t slot = 0; slot < params.Species.size(); ++slot )
            total += GenerateCloudProceduralBlobs( params, slot, regionOriginKm ).size();
        return total;
    }

    Common::ResultStr<std::vector<unsigned char>>
    BakeCloudProceduralVolume( const CloudProceduralFieldParams& params, const glm::vec2& regionOriginKm )
    {
        return BakeCloudProceduralVolume( params, regionOriginKm, CloudProceduralBakeProgressFn{} );
    }

    Common::ResultStr<std::vector<unsigned char>>
    BakeCloudProceduralVolume( const CloudProceduralFieldParams& params, const glm::vec2& regionOriginKm,
                               const CloudProceduralBakeProgressFn& onProgress )
    {
        if ( auto valid = ValidateCloudProceduralParams( params ); !valid )
            return Common::MakeFormattedError<std::vector<unsigned char>>( "parameters are not usable: {}",
                                                                           valid.GetError() );

        const uint32_t width  = params.VolumeSideVoxels;
        const uint32_t height = kCloudProceduralVolumeHeight;
        const uint32_t depth  = params.VolumeSideVoxels;

        std::vector<unsigned char> voxels(
             static_cast<size_t>( CloudProceduralVoxelBytes( params.VolumeSideVoxels ) ), 0u );

        // THE UNIT OF PROGRESS IS ONE XZ SLICE OF ONE SPECIES, which is also the unit of cancellation. A
        // species that places nothing still counts, so the fraction is monotone whatever the layer holds.
        const uint32_t slices = std::max<uint32_t>( 1u, static_cast<uint32_t>( params.Species.size() ) * depth );
        uint32_t       sliceDone = 0u;

        // THE XZ SLICES ARE ALSO THE UNIT OF PARALLELISM, and it is the same boundary because it is the
        // same independence: a slice reads the species' lump bin and writes `height x width` voxels that
        // no other slice touches. Slice z owns the bytes [z*height*width*4, (z+1)*height*width*4), which
        // are contiguous and 64 KiB wide at the shipped grid — disjoint by construction, not by luck, and
        // too far apart to share a cache line.
        //
        // WHY THE BAKE AND NOT ITS CALLER DOES THIS. The per-species setup — the lumps, their wrapped
        // copies, the bin — is a third of nothing and all of it would be repeated if the caller split the
        // volume up and baked the pieces. Splitting inside also means the tools, the tests and both
        // renderers get it without each remembering to.
        //
        // PROGRESS AND CANCELLATION MOVE UNDER ONE LOCK. The hook is at most `4 x side` calls over a bake
        // of seconds, so serialising them is free, and it buys two properties the header promises: the
        // fraction is still strictly monotone (the counter advances in lock order, whichever slice got
        // there), and exactly ONE call can be the one told to stop — every later slice tests the flag
        // under the same lock and never reaches the callback. Without that, a cancelled bake would call
        // the hook once per participant and "the bake carried on past a callback that said stop" would be
        // reported against a bake that did no such thing.
        std::mutex progressMutex;
        bool       cancelled = false;

        const float voxelXKm = params.RegionSizeKm / static_cast<float>( width );
        const float voxelZKm = params.RegionSizeKm / static_cast<float>( depth );
        const float voxelYKm = params.LayerThicknessKm / static_cast<float>( height );

        // How far a lump reaches before its term in the join is below the quantisation floor. See
        // kJoinCutoffRadii; the profile depth is added because a voxel that far INSIDE a body still has to
        // know about it.
        const float influenceKm = params.ProfileDepthKm + params.BlendRadiusKm * kJoinCutoffRadii;

        const float invBlend   = 1.0f / params.BlendRadiusKm;
        const float invProfile = 1.0f / params.ProfileDepthKm;

        for ( uint32_t slot = 0; slot < params.Species.size(); ++slot )
        {
            const std::vector<CloudModellingBlob> blobs =
                 GenerateCloudProceduralBlobs( params, slot, regionOriginKm );

            if ( blobs.empty() )
            {
                sliceDone += depth;
                continue;
            }

            // EVERY LUMP AT EVERY WRAP THAT REACHES THE REGION. This is what makes the volume periodic and
            // therefore what makes REPEAT sampling seamless — see the header note. A lump in the middle of
            // the region produces exactly one entry; one against a face produces two; one in a corner four.
            struct Placed
            {
                CloudModellingPreparedBlob Blob;
                glm::vec3                  MinKm;
                glm::vec3                  MaxKm;
            };

            std::vector<Placed> placed;
            placed.reserve( blobs.size() * 2u );

            for ( const CloudModellingBlob& blob : blobs )
            {
                const glm::vec3 extent = CloudModellingBlobHalfExtentKm( blob ) + glm::vec3( influenceKm );

                for ( int wz = -kWrapRange; wz <= kWrapRange; ++wz )
                {
                    for ( int wx = -kWrapRange; wx <= kWrapRange; ++wx )
                    {
                        CloudModellingBlob shifted = blob;
                        shifted.CentreKm.x += static_cast<float>( wx ) * params.RegionSizeKm;
                        shifted.CentreKm.z += static_cast<float>( wz ) * params.RegionSizeKm;

                        const glm::vec3 minKm = shifted.CentreKm - extent;
                        const glm::vec3 maxKm = shifted.CentreKm + extent;

                        // Reject the copies that cannot touch the region at all, which is seven of the nine
                        // for a lump in the middle of it.
                        if ( maxKm.x <= regionOriginKm.x || minKm.x >= regionOriginKm.x + params.RegionSizeKm )
                            continue;
                        if ( maxKm.z <= regionOriginKm.y || minKm.z >= regionOriginKm.y + params.RegionSizeKm )
                            continue;
                        if ( maxKm.y <= params.LayerBottomKm ||
                             minKm.y >= params.LayerBottomKm + params.LayerThicknessKm )
                            continue;

                        placed.push_back( Placed{ PrepareCloudModellingBlob( shifted ), minKm, maxKm } );
                    }
                }
            }

            if ( placed.empty() )
            {
                sliceDone += depth;
                continue;
            }

            // A COARSE XZ BIN OVER THE REGION, so a voxel asks about the lumps that can reach it rather
            // than about all of them. Without it the bake is `voxels x lumps` — two million by a thousand —
            // and with it the inner list is the handful of lumps whose boxes overlap this bin.
            //
            // THE LISTS STAY IN THE LUMPS' CANONICAL ORDER because `placed` is walked in that order and a
            // lump is appended to each bin it touches. That is what carries phase Э4's order-independence
            // into this bake: the sum a voxel performs is over an ascending subsequence of one sorted list,
            // whatever the lattice loop did.
            const uint32_t bins   = 32u;
            const float    binKm  = params.RegionSizeKm / static_cast<float>( bins );
            const float    invBin = 1.0f / binKm;

            std::vector<std::vector<uint32_t>> binList( static_cast<size_t>( bins ) * bins );

            for ( uint32_t index = 0; index < placed.size(); ++index )
            {
                const Placed& item = placed[index];

                const int firstX =
                     std::max( 0, static_cast<int>( std::floor( ( item.MinKm.x - regionOriginKm.x ) * invBin ) ) );
                const int lastX =
                     std::min( static_cast<int>( bins ) - 1,
                               static_cast<int>( std::floor( ( item.MaxKm.x - regionOriginKm.x ) * invBin ) ) );
                const int firstZ =
                     std::max( 0, static_cast<int>( std::floor( ( item.MinKm.z - regionOriginKm.y ) * invBin ) ) );
                const int lastZ =
                     std::min( static_cast<int>( bins ) - 1,
                               static_cast<int>( std::floor( ( item.MaxKm.z - regionOriginKm.y ) * invBin ) ) );

                for ( int bz = firstZ; bz <= lastZ; ++bz )
                    for ( int bx = firstX; bx <= lastX; ++bx )
                        binList[static_cast<size_t>( bz ) * bins + bx].push_back( index );
            }

            // ONE SLICE PER CLAIM. Slices differ in cost by an order of magnitude — one crossing a cluster
            // does the full join at every column, one over clear sky rejects at the bin — so handing out
            // equal blocks in advance would leave most participants idle behind the unlucky one. A grain
            // of 1 is worth its claim here: a slice is tens of milliseconds of work against a mutex.
            Common::JobSystem::Get().ParallelRanges(
                 depth, 1u,
                 [&]( size_t zBegin, size_t zEnd )
                 {
                     DESERT_PROFILE_SCOPE( "Clouds: modelling bake XZ slice" );

                     // PER RANGE AND NOT PER BAKE: these are the scratch the inner loops refill, and one
                     // shared pair would be the only write two slices could ever contend on.
                     std::vector<float>    distances;
                     std::vector<uint32_t> column;

                     for ( uint32_t z = static_cast<uint32_t>( zBegin ); z < static_cast<uint32_t>( zEnd ); ++z )
                     {
                         // BETWEEN SLICES AND NOT INSIDE THEM, exactly as the sculpted bake does it and for the
                         // same arithmetic: at most `4 x side` calls over a bake of seconds is a check whose cost
                         // is unmeasurable, where a call per voxel would be millions of indirect calls through a
                         // std::function and would dominate the work it is reporting on. At the shipped 256 that
                         // is one check every ~40 ms of Debug bake, which is the granularity a cancel is honoured
                         // at.
                         if ( onProgress )
                         {
                             std::lock_guard<std::mutex> lk( progressMutex );
                             if ( cancelled )
                                 return;
                             if ( !onProgress( static_cast<float>( sliceDone ) / static_cast<float>( slices ) ) )
                             {
                                 cancelled = true;
                                 return;
                             }
                             ++sliceDone;
                         }

                         const float worldZ = regionOriginKm.y + ( static_cast<float>( z ) + 0.5f ) * voxelZKm;
                         const int   binZ = std::clamp( static_cast<int>( ( worldZ - regionOriginKm.y ) * invBin ),
                                                        0, static_cast<int>( bins ) - 1 );

                         for ( uint32_t x = 0; x < width; ++x )
                         {
                             const float worldX = regionOriginKm.x + ( static_cast<float>( x ) + 0.5f ) * voxelXKm;
                             const int   binX =
                                  std::clamp( static_cast<int>( ( worldX - regionOriginKm.x ) * invBin ), 0,
                                              static_cast<int>( bins ) - 1 );

                             const std::vector<uint32_t>& list =
                                  binList[static_cast<size_t>( binZ ) * bins + binX];
                             if ( list.empty() )
                                 continue;

                             // THE COLUMN'S OWN CANDIDATES, decided once for all 32 rows above this ground
                             // position. The horizontal half of the box test does not depend on the altitude, and
                             // performing it inside the y loop repeated it thirty-two times for the same answer —
                             // measured at 642 ms per bake for one species, most of it in rejections. The list
                             // stays in the lumps' canonical order because `list` is, which is what carries the
                             // join's order-independence through this optimisation.
                             column.clear();
                             for ( uint32_t index : list )
                             {
                                 const Placed& item = placed[index];
                                 if ( worldX < item.MinKm.x || worldX > item.MaxKm.x || worldZ < item.MinKm.z ||
                                      worldZ > item.MaxKm.z )
                                     continue;
                                 column.push_back( index );
                             }

                             if ( column.empty() )
                                 continue;

                             for ( uint32_t y = 0; y < height; ++y )
                             {
                                 const float worldY =
                                      params.LayerBottomKm + ( static_cast<float>( y ) + 0.5f ) * voxelYKm;

                                 const glm::vec3 point( worldX, worldY, worldZ );

                                 // THE SAME TWO LOOPS THE SCULPTED BAKE PERFORMS, in the same order, over the same
                                 // three shared functions — the nearest distance, then the shifted sum. Only the
                                 // SET is different, and it is a subset chosen so that everything left out is
                                 // below the quantisation floor.
                                 distances.clear();

                                 float nearest = 0.0f;
                                 bool  any     = false;

                                 for ( uint32_t index : column )
                                 {
                                     const Placed& item = placed[index];

                                     if ( point.y < item.MinKm.y || point.y > item.MaxKm.y )
                                     {
                                         distances.push_back( std::numeric_limits<float>::infinity() );
                                         continue;
                                     }

                                     const float distance = CloudModellingBlobDistanceKm( item.Blob, point );
                                     distances.push_back( distance );

                                     nearest = any ? std::min( nearest, distance ) : distance;
                                     any     = true;
                                 }

                                 if ( !any )
                                     continue;

                                 float sum = 0.0f;
                                 for ( size_t k = 0; k < distances.size(); ++k )
                                 {
                                     if ( !std::isfinite( distances[k] ) )
                                         continue;
                                     sum += CloudModellingJoinTerm( placed[column[k]].Blob.Weight, distances[k],
                                                                    nearest, invBlend );
                                 }

                                 const float joined = CloudModellingJoinKm( nearest, sum, params.BlendRadiusKm );
                                 if ( joined >= 0.0f )
                                     continue;

                                 // The Dimensional Profile: 0 at the surface and 1 at ProfileDepth inside, which
                                 // is Guerrilla's own quantity (deck p.85) obtained analytically rather than by a
                                 // distance transform — and the normalised distance field variant C §3 point 2
                                 // asks the profile to BE.
                                 const float profile = std::clamp( -joined * invProfile, 0.0f, 1.0f );

                                 const size_t at = ( ( static_cast<size_t>( z ) * height + y ) * width + x ) *
                                                   kCloudProceduralBytesPerVoxel;

                                 voxels[at + slot] = Common::Math::QuantiseUnitToByte( profile );
                             }
                         }
                     }
                 } );

            // ASKED AFTER THE LOOP AND NOT INSIDE IT. ParallelRanges returns only once every claimed slice
            // has finished, so this is the first moment at which "somebody said stop" is a settled fact
            // rather than a value another participant is still deciding.
            if ( cancelled )
                return Common::MakeError<std::vector<unsigned char>>(
                     "the procedural modelling bake was cancelled before it finished" );
        }

        if ( onProgress )
            onProgress( 1.0f );

        return Common::MakeSuccess( std::move( voxels ) );
    }

    float CloudProceduralCellCoverage( const CloudProceduralFieldParams& params, uint32_t slot,
                                       const glm::vec2& centreKm )
    {
        return CloudCellCoverage( params, slot, CloudPatchSeed( CloudSpeciesSeed( params, slot ) ), centreKm );
    }

    Common::ResultStr<CloudLayoutPreview> BuildCloudLayoutPreview( const CloudProceduralFieldParams& params,
                                                                   uint32_t slot, float spanKm, uint32_t maxSide )
    {
        if ( slot >= params.Species.size() )
            return Common::MakeFormattedError<CloudLayoutPreview>( "cannot map slot {}: this layer has {} species",
                                                                   slot, params.Species.size() );

        if ( !std::isfinite( spanKm ) || spanKm <= 0.0f )
            return Common::MakeFormattedError<CloudLayoutPreview>( "the mapped span must be a positive length, "
                                                                   "got {} km",
                                                                   spanKm );

        if ( maxSide == 0u )
            return Common::MakeFormattedError<CloudLayoutPreview>( "the map's side ceiling must be at least 1" );

        CloudLayoutPreview preview;
        preview.SpanKm = spanKm;

        // THE MAPPED SLOT'S OWN CELL, and it is deliberately NOT the finest cell in the layer.
        //
        // A channel of the painting decides where ONE species goes, and the lattice that species is placed
        // on is its own — a type's Placement Scale says how much coarser or finer than the layer it is.
        // Drawing slot 2's map on slot 0's finer cell would show detail slot 2 cannot place, and the
        // legibility bound quoted beside it would be the most permissive one in the layer rather than the
        // one this channel has to clear: a 1.2 km stroke reads on a 1 km cell and breaks into clumps on a
        // 4 km one, so an artist told the finest number is told the wrong number about their own channel.
        //
        // ValidateCloudProceduralLayout still takes the FINEST, and that is a different question with a
        // different right answer: it asks whether one layout texel can tell two neighbouring cells apart,
        // which fails first for the species with the smallest cells. Legibility fails first for the
        // species with the LARGEST. The two bounds run in opposite directions, which is exactly why they
        // must not share a number.
        //
        // The SHORTER side, because an anisotropic cell is long along the wind and it is across the stretch
        // that a stroke has to survive.
        const glm::vec2 extent = CloudProceduralCellExtentKm( params, params.Species[slot] );
        const float     cellKm = std::min( extent.x, extent.y );
        preview.CellKm         = cellKm;

        const uint32_t wanted =
             static_cast<uint32_t>( std::max( 1.0f, std::ceil( spanKm / std::max( cellKm, 1e-3f ) ) ) );
        preview.Side          = std::min( wanted, maxSide );
        preview.SamplePitchKm = spanKm / static_cast<float>( preview.Side );

        // The two ends of the pattern slider, evaluated on the same lattice as the picture. Copies rather
        // than a flag inside the coverage function: the function under test must be the one the bake calls,
        // unchanged, or the measurement would be of a different function than the sky.
        CloudProceduralFieldParams atZero      = params;
        atZero.LayoutPlacement.PatternStrength = 0.0f;
        CloudProceduralFieldParams atOne       = params;
        atOne.LayoutPlacement.PatternStrength  = 1.0f;

        const size_t cells = static_cast<size_t>( preview.Side ) * preview.Side;
        preview.Coverage.resize( cells, 0.0f );
        preview.Cells = static_cast<uint32_t>( cells );

        double sum = 0.0;

        for ( uint32_t iv = 0; iv < preview.Side; ++iv )
        {
            for ( uint32_t iu = 0; iu < preview.Side; ++iu )
            {
                const glm::vec2 centre(
                     ( ( static_cast<float>( iu ) + 0.5f ) / static_cast<float>( preview.Side ) - 0.5f ) * spanKm,
                     ( ( static_cast<float>( iv ) + 0.5f ) / static_cast<float>( preview.Side ) - 0.5f ) *
                          spanKm );

                const float coverage = CloudProceduralCellCoverage( params, slot, centre );

                preview.Coverage[static_cast<size_t>( iv ) * preview.Side + iu] = coverage;
                sum += static_cast<double>( coverage );

                // ONE 255TH IS THE FLOOR OF WHAT CAN SHOW. The volume is quantised to bytes, so a coverage
                // difference below that reaches neither the picture nor the sky, and counting it would
                // report a live knob where the frames come back byte-identical.
                if ( std::fabs( CloudProceduralCellCoverage( atOne, slot, centre ) -
                                CloudProceduralCellCoverage( atZero, slot, centre ) ) > 1.0f / 255.0f )
                    ++preview.CellsPatternMoves;

                if ( coverage <= 0.0f || coverage >= 1.0f )
                    ++preview.CellsClamped;
            }
        }

        preview.MeanCoverage = static_cast<float>( sum / static_cast<double>( cells ) );

        return Common::MakeSuccess( std::move( preview ) );
    }
} // namespace Desert::Assets
