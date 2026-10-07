#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <vector>

namespace Desert::Graphic
{
    /**
     * THE VERTICAL PROFILE — the one UE capability this engine did not have, and the reason it is a
     * SAMPLED CURVE rather than the closed form it replaces.
     *
     * WHAT WAS MISSING. Horizontally an artist has had authority for three phases: `.dclayout` carries a
     * painted pattern and mask, `.dcmv` sculpts a hero volume, `.dcnv` authors the noise an edge is cut
     * from. Vertically there was one law, written in this engine and not in any asset:
     *
     *     halfWidth(t) = (0.62 - 0.16 t) * (1 - 0.5 * TopTaper * t)
     *
     * — a product of two falling lines, so it is MONOTONE DECREASING at every setting of its one knob.
     * A shelf, a waist, a body that widens with height, mass gathered at the top of its own band: none
     * of them is reachable, whatever `TopTaper` is set to. That is the capability gap, and it is a
     * property of the closed form rather than of the numbers fed to it.
     *
     * WHAT EPIC'S TEXTURE SAYS, because this is the thing being matched: "each channel of this texture
     * describes the profile shape AND RELATIVE ALTITUDE of a different cloud type". Both halves are here.
     * The shape is the samples below; the relative altitude falls out of them, because a lump's height is
     * derived from its own width by `kCloudLumpVerticalOverHorizontal` and the stack is fitted into the
     * type's band — so a curve whose mass sits high puts fat lumps high and thin ones low, and the body's
     * centre of mass moves without any second number saying so.
     *
     * WHY A CURVE AND NOT A FOUR-CHANNEL TEXTURE. Decision D-22 required a curve; the owner relaxed that
     * for Р2 and asked the MEASUREMENT to decide, because the painting canvas Р1 built made the texture
     * path far cheaper than it had been when D-22 was written. It was measured, and it decided the same
     * way for a different and much stronger reason than D-22 gave. Two findings, one of them numeric:
     *
     *   * THE CONSUMER RESOLVES SIX POINTS, AND THE STORAGE CANNOT OUTRUN IT. The profile is read by the
     *     lump stack and by nothing else, and the stack is `kBlobsPerCluster` = 6 lobes at fixed heights
     *     `t = ((step + 0.5) / 6) ^ 1.7`. So the entire difference a sample count can make is the
     *     piecewise-linear error at those six heights, and that is computable rather than arguable.
     *     Against the closed form, worst case over every taper the shipped library uses, expressed as the
     *     LUMP-RADIUS error it causes on the shipped congestus' 2.16 km cluster:
     *
     *         N =    4      6      8     12     16     32     64    256
     *         err 6.96 m 2.22 m 0.92 m 0.53 m 0.18 m 0.05 m 0.01 m 0.00 m
     *
     *     The march's finest resolvable chord is 125 m. Sixteen samples are already wrong by a
     *     seven-hundredth of the smallest thing the march can see; two hundred and fifty-six samples buy
     *     0.18 m of cloud. A painted raster's extra rows are not detail, they are unreachable storage.
     *   * CHANNEL = SPECIES IS ALREADY SPENT. Unreal packs four types into R/G/B/A because its profile is
     *     ONE texture shared by the whole material. Ours is not shared: a type IS a file
     *     (`.decloudtype`, decision D-11), so the four channels would carry four copies of one type's
     *     profile — three of them dead in the sense of DEV_CONTRACT.md §1.3.
     *
     * So the profile is stored the way the file already stores everything else: a short row of numbers a
     * human can read, diff and hand-edit, with no baked artefact that can go stale against the maths.
     *
     * WHAT WOULD CHANGE THE ANSWER, since a refusal has to say so: raise `kBlobsPerCluster`, or make the
     * stack's lobe count depend on the band, and the consumer stops being six-tap. A stack of sixty lobes
     * would read a curve sixty times and the argument above would have to be re-measured, not re-quoted.
     */

    /// HOW MANY SAMPLES THE CURVE CARRIES, and the number is set by the AUTHORING grid rather than by
    /// fidelity, because fidelity was already spent at four samples (the table above).
    ///
    /// WHAT SETS IT IS THE LUMP SPACING. Sixteen samples put the artist's finest expressible feature at
    /// `band / 15` — 27 m on the shipped stratus, 240 m on the congestus, 540 m on the cumulonimbus. The
    /// thing that renders those features is a stack of six lobes spread over the same band, so the lobes
    /// are 67 m, 600 m and 1350 m apart respectively. Sixteen therefore gives the artist between two and
    /// two-and-a-half times the resolution the stack can carry, on every shipped type.
    ///
    /// NOT EIGHT, which puts the congestus' grid at 514 m against a 600 m lobe spacing — the artist would
    /// be authoring at exactly the granularity of the thing drawing it, with no headroom for a corner that
    /// lands between two lobes. NOT THIRTY-TWO, which is five times finer than the stack and is the
    /// unreachable storage the block above refuses.
    inline constexpr uint32_t kCloudProfileSamples = 16;

    /**
     * The type's silhouette: its horizontal half-width at `kCloudProfileSamples` equally spaced heights
     * across its OWN band, in units of the cluster's radius. Sample `i` is at height fraction
     * `i / (kCloudProfileSamples - 1)`, so sample 0 is the cloud base and the last sample is its top.
     *
     * IN CLUSTER RADII AND NOT NORMALISED TO ONE, which is deliberate. The cluster's radius already
     * carries how BIG this cloud is — it is the cell, the coverage and the size draw — so a curve
     * normalised to a peak of 1 would need a gain constant beside it to get back to the 0.62 the law it
     * replaces starts at, and that constant is a second place the width is decided. The samples are the
     * width. There is nothing else.
     *
     * LINEARLY INTERPOLATED between samples and CLAMPED outside, by `CloudProfileHalfWidth` below. Linear
     * and not a spline: a spline overshoots, and an overshoot here is a lump wider than any number in the
     * asset — a silhouette the artist did not draw, arrived at by the interpolator.
     */
    struct CloudVerticalProfile
    {

        // ── IS THIS THE SAME AUTHORED STATE? ──────────────────────────────────────────────────────────
        //
        // DEFAULTED, and that is the whole point of it rather than laziness: the compiler generates the
        // comparison from EVERY member, so a field added tomorrow is compared tomorrow. A hand-written
        // operator== over these fields would be a census kept by hand beside a struct, and the failure
        // when it fell behind would be a document that reports itself CLEAN while holding an unsaved edit
        // — the "middle link drops a property" shape, with a person's work as the cost.
        //
        // Its reader is ISubjectDocument::GetDiskState: a cloud document keeps a copy of what it loaded or
        // last wrote and compares the working state against it. Cheap because these are small.
        [[nodiscard]] bool                      operator==( const CloudVerticalProfile& ) const = default;
        std::array<float, kCloudProfileSamples> HalfWidth;

        // THE ALTITUDE DENSITY H(t) (H-BASE, format version 6): how much of the type's matter there is at
        // height fraction t of its band, in [0, 1], on the same sixteen heights as HalfWidth. Unreal's
        // VolumetricCloud material multiplies its profile by a curve of NormAltitudeInLayer; Nubis (2015,
        // 2017) by a height gradient per cloud type. Both do it BEFORE the coverage remap, and that order
        // is the point: remap(profile * H, 1 - g, 1, 0, 1) cuts hardest where H is small, so the base
        // rounds off and its corners are eaten instead of standing as the flat floor of a box. The bake
        // applies it (Assets::CloudProceduralAltitudeDensity), so the march's form is profile * H.
        //
        // THE RISING PART IS ALSO WHERE THE EDGE TURNS WISPY: up to the first sample at the curve's
        // maximum (CloudProfileWispTopFraction) the march blends the type's DetailCharacter towards 0,
        // which is Nubis' wispy-at-the-base, billowy-above erosion — one authority for both.
        std::array<float, kCloudProfileSamples> Density;
    };

    /// THE SHIPPED ALTITUDE DENSITY: zero at the base, a smoothstep to full over the lowest fifth of the
    /// band, full above it — the cumulus gradient of Nubis 2015 and the default ramp of Unreal's cloud
    /// altitude curve. What every generator below writes and what Tools/SceneMigrator gave the nine shipped
    /// types when the format moved to version 6.
    constexpr std::array<float, kCloudProfileSamples> CloudProfileDensityBaseRamp()
    {
        constexpr uint32_t last     = kCloudProfileSamples - 1;
        constexpr float    rampTopT = 0.2f;

        std::array<float, kCloudProfileSamples> density{};
        for ( uint32_t i = 0; i <= last; ++i )
        {
            const float t = static_cast<float>( i ) / static_cast<float>( last );
            const float x = std::clamp( t / rampTopT, 0.0f, 1.0f );
            density[i]    = x * x * ( 3.0f - 2.0f * x );
        }
        return density;
    }

    /// H at height fraction @p t of the band: linear between samples, clamped outside — the same reading
    /// CloudProfileHalfWidth makes of the silhouette, for the same reason (no overshoot past the authored).
    inline float CloudProfileDensity( const CloudVerticalProfile& profile, float t )
    {
        constexpr uint32_t last = kCloudProfileSamples - 1;

        const float clamped  = std::clamp( t, 0.0f, 1.0f );
        const float position = clamped * static_cast<float>( last );

        const uint32_t low      = std::min( static_cast<uint32_t>( position ), last );
        const uint32_t high     = std::min( low + 1u, last );
        const float    fraction = position - static_cast<float>( low );

        return profile.Density[low] + ( profile.Density[high] - profile.Density[low] ) * fraction;
    }

    /// Where the wispy base ends: the height fraction of the FIRST sample at the curve's maximum. Below it
    /// the density is still rising and the edge is wispy; above it the type's own DetailCharacter stands.
    inline float CloudProfileWispTopFraction( const CloudVerticalProfile& profile )
    {
        const auto peak = std::max_element( profile.Density.begin(), profile.Density.end() );
        return static_cast<float>( std::distance( profile.Density.begin(), peak ) ) /
               static_cast<float>( kCloudProfileSamples - 1 );
    }

    /// A density sample the validator accepts: finite and in [0, 1].
    inline bool CloudProfileDensitySampleIsLegal( float density )
    {
        return std::isfinite( density ) && density >= 0.0f && density <= 1.0f;
    }

    /// The half-width at height fraction @p t up the type's band, in cluster radii.
    ///
    /// CLAMPED AT BOTH ENDS rather than extrapolated. `t` arrives from the stack layout, where it is a
    /// curve of the step index and can sit a hair outside [0, 1]; extrapolating a linear segment there
    /// produces a negative width at one end and an unbounded one at the other, both of which are lumps
    /// nobody authored.
    inline float CloudProfileHalfWidth( const CloudVerticalProfile& profile, float t )
    {
        constexpr uint32_t last = kCloudProfileSamples - 1;

        const float clamped  = std::clamp( t, 0.0f, 1.0f );
        const float position = clamped * static_cast<float>( last );

        const uint32_t low      = std::min( static_cast<uint32_t>( position ), last );
        const uint32_t high     = std::min( low + 1u, last );
        const float    fraction = position - static_cast<float>( low );

        return profile.HalfWidth[low] + ( profile.HalfWidth[high] - profile.HalfWidth[low] ) * fraction;
    }

    /**
     * THE CLOSED FORM THIS CURVE REPLACES, sampled — and it is kept as a named function for three jobs
     * that all need the SAME numbers: it rewrote the nine shipped `.decloudtype` files when the format
     * moved to version 3, it is what the panel's "classic taper" preset starts an artist from, and it is
     * the oracle `Desert/Tests/Engine/CloudType` compares the shipped library against.
     *
     * `(0.62 - 0.16 t) * (1 - 0.5 * taper * t)` is the law that stood in the stack layout up to version 2
     * of the format, transcribed with no change of scale. That is what makes the format move a
     * re-expression rather than a re-authoring: the library at version 3 renders the sky the library at
     * version 2 rendered, to the 0.18 m of lump radius tabulated above.
     */
    /// CONSTEXPR, and that is not decoration. `CloudTypeShape` is an aggregate, and several suites build
    /// one with a POSITIONAL initialiser — `{ 0.15f, 0.55f, 0.88f, 0.12f, ... }`. When the profile took
    /// `TopTaper`'s place in that list, brace elision let the taper's old value initialise
    /// `Profile.HalfWidth[0]` and shifted every number after it one slot up the array. It COMPILED, in a
    /// `constexpr` context, and produced shapes that drew no cloud at all. The fix is for every such
    /// initialiser to name the profile, and they can only do that if this function is usable where they are.
    constexpr CloudVerticalProfile CloudProfileFromTaper( float taper )
    {
        constexpr uint32_t last = kCloudProfileSamples - 1;

        const float clamped = std::clamp( taper, 0.0f, 1.0f );

        CloudVerticalProfile profile{};
        for ( uint32_t i = 0; i <= last; ++i )
        {
            const float t        = static_cast<float>( i ) / static_cast<float>( last );
            profile.HalfWidth[i] = ( 0.62f - 0.16f * t ) * ( 1.0f - 0.5f * clamped * t );
        }
        profile.Density = CloudProfileDensityBaseRamp();
        return profile;
    }

    /// The profile of the type an empty slot resolves to — the shipped congestus, which stood at a taper
    /// of 0.5. One expression of it, so the default asset and the default curve cannot come apart.
    inline CloudVerticalProfile CloudProfileDefault()
    {
        return CloudProfileFromTaper( 0.5f );
    }

    /**
     * A DECK: the same width from base to top. The silhouette of a sheet — a slab of cloud with no
     * convective structure up its height, which is what a stratus or an altostratus IS seen edge on.
     *
     * REACHABLE UNDER THE OLD LAW ONLY AS AN APPROXIMATION, and that is worth saying because it is the
     * weaker of the two presets as evidence. `TopTaper` at 0 gave `0.62 - 0.16 t`, which still loses a
     * quarter of its width by the top. This is flat to the last digit.
     */
    inline CloudVerticalProfile CloudProfileFlatDeck()
    {
        CloudVerticalProfile profile{};
        profile.HalfWidth.fill( 0.62f );
        profile.Density = CloudProfileDensityBaseRamp();
        return profile;
    }

    /**
     * A TOWER: pinched at the base, swelling through the upper half, closing at the very top.
     *
     * THIS IS THE SHAPE THAT PROVES THE CAPABILITY, because it is NOT REACHABLE under the law the curve
     * replaced at any setting of any number. That law was `(0.62 - 0.16 t) * (1 - 0.5 * taper * t)` — a
     * product of two lines that both fall — so its derivative is negative everywhere for every taper in
     * range. A profile whose maximum is in its interior cannot be written as such a product, so no
     * version-2 `.decloudtype` could describe this and no slider could reach it.
     *
     * WHAT IT LOOKS LIKE IN THE SKY, and why the two lobes are not an anvil: the mass moves UP the band.
     * A lump's height is derived from its own width, so a curve that is thin low and fat high puts small
     * lumps at the base and large ones near the top — a cauliflower standing on a stalk. The anvil is a
     * separate lobe with a GAP under it (`CloudProceduralVolume.cpp`), a thing no single curve expresses;
     * this is one connected body whose waist is low.
     */
    inline CloudVerticalProfile CloudProfileTower()
    {
        constexpr uint32_t last = kCloudProfileSamples - 1;

        CloudVerticalProfile profile{};
        for ( uint32_t i = 0; i <= last; ++i )
        {
            const float t = static_cast<float>( i ) / static_cast<float>( last );

            // A raised cosine centred at three quarters of the band, on a narrow stalk. The peak is 0.92
            // against the deck's 0.62 and the base is 0.15, so the silhouette is unmistakable from any
            // angle rather than a subtle re-weighting the dome sweep would have to be squinted at.
            const float lobe     = std::cos( ( t - 0.75f ) * 3.14159265f / 0.62f );
            profile.HalfWidth[i] = 0.15f + 0.77f * std::max( lobe, 0.0f );
        }
        profile.Density = CloudProfileDensityBaseRamp();
        return profile;
    }

    /// Rejects a profile the stack layout cannot honour, naming the sample that is wrong. Pure, so the
    /// panel refuses to save for the same reason the loader refuses to read.
    ///
    /// A CEILING OF FOUR, and it is the cluster's radius times four rather than a round number: the
    /// footprint compensation that keeps the Coverage slider honest is a quadrature over this curve, and
    /// a width far outside the range that quadrature was checked over would be priced by extrapolation.
    inline bool CloudProfileSampleIsLegal( float halfWidth )
    {
        return std::isfinite( halfWidth ) && halfWidth >= 0.0f && halfWidth <= 4.0f;
    }

    /**
     * WHAT A KIND OF CLOUD IS, as thirteen numbers and a curve — and nothing about how they are turned
     * into a field.
     *
     * WHAT THIS HEADER USED TO ALSO HOLD, AND WHY IT DOES NOT. Until phase Э5 it carried a parametric
     * curve `f(altitude, placement pattern)` and the generator that evaluated it into a 256 x 64 RGBA32F
     * table the march sampled — decision D-13, and it was the right answer to the question it was asked.
     * The question changed. That table was one half of `coverage x profile`, and the other half was a
     * threshold on the Alligator noise, whose field is `best - second` and therefore ZERO wherever two
     * feature points contribute equally: a wall of zeros between every pair of cells, so two lobes could
     * never merge at any setting of any slider and the sky read as a deck of separate cushions.
     *
     * The profile is a three-dimensional field now — a sum of smoothed volumetric lumps joined by an
     * exponential smooth minimum, baked into a camera-centric volume by
     * Engine/Assets/CloudProceduralVolume.hpp — which is variant C of ANALYSIS_APPROACH.md §3 point 2 as
     * decision D-1 approved it. The numbers below did not change and NOT ONE OF THEM LOST ITS CONSUMER;
     * what changed is what reads them:
     *
     *   Base / Top Altitude               the band a cluster of lumps spans
     *   Edge Top Fraction                 how short the shallowest cluster is against the fullest
     *   Base Ramp Fraction                the thickness of the lowest lobe against the ones above it
     *   Profile                           the silhouette: half-width against height up the band. It
     *                                     REPLACED `Top Taper` in format version 3 — see the block above
     *                                     the struct — and it is the only authority over the vertical
     *   Anvil Altitude / Thickness / Strength   one extra, wider, flatter lump above the tower — which is
     *                                     the shape D-13 reached for a two-dimensional table to express,
     *                                     and a second lump expresses without a table at all
     *   Placement Scale / Anisotropy      the lattice a cluster is drawn in, and its stretch downwind
     *   Detail Character / Detail, Density, Extinction Factor   what the cloud is MADE of — these four
     *                                     still travel to the march in the parameter block, untouched
     *
     * THE ANCHOR OF §5.1 SURVIVES UNCHANGED and is the reason the altitudes are still ABSOLUTE kilometres
     * rather than fractions of a layer: a set of ratios is satisfied at any absolute scale, so a layer
     * that has drifted into the stratosphere fails no test unless some number is pinned in metres. These
     * are those numbers, and Desert/Tests/Engine/CloudType pins the shipped library against meteorology.
     *
     * THIS HEADER IS DEPENDENCY-FREE ON PURPOSE. It is included by the asset layer (which owns the
     * numbers), by CloudPayload.hpp (which computes the shell from a type's absolute altitudes), by the
     * procedural generator (which places the lumps) and by two test suites. Anything heavier here would
     * drag the renderer into a test that links nothing.
     */

    /**
     * Everything the generator needs to draw one cloud type's family of curves, plus the three factors
     * that describe what the type is MADE OF rather than what shape it is.
     *
     * The pattern axis is what makes the profile a FAMILY. `EdgeTopFraction` is how much of its full
     * height the type reaches where the placement pattern has only just crossed into cloud; at the core
     * of a patch it reaches all of it. A stratus is near 1 — a sheet is a sheet everywhere — and a
     * congestus is near a tenth, which is the difference between a flat pad at the rim of a patch and a
     * tower in its middle.
     *
     * THE THREE FACTORS ARE MULTIPLIERS ON THE ARTIST'S OWN SCALES, not second copies of them. The
     * component carries Density Scale, Extinction Scale and Detail Strength for the LAYER; a type carries
     * how much of each it is relative to a cumulus. The product is formed once, in PackCloudParams, so
     * "1" on the component keeps meaning "this type as it is" whichever type is in the slot. Two absolute
     * values for one quantity would be two numbers that can disagree — the defect class §2.3.1 of the
     * contract is about.
     */
    struct CloudTypeShape
    {
        float BaseAltitudeKm;   // where the cloud base sits — the lifting condensation level of the type
        float TopAltitudeKm;    // the top it reaches at the CORE of a placement patch
        float EdgeTopFraction;  // fraction of (Top - Base) it reaches where the patch has just begun
        float BaseRampFraction; // fraction of the cloud's own height the base ramp takes to reach full

        // THE SILHOUETTE, and it is where `TopTaper` went. That number was one knob on a monotone law
        // and this is the law itself, authored: half-width against height up the band, in cluster radii.
        // It is read at `CloudProceduralVolume.cpp` by the stack layout — which is the ONLY place the
        // vertical silhouette is decided, and the reason the knob could not stay beside it. Two authorities
        // over one quantity is the defect class §2.3.1 of the contract is about, and the calibration the
        // second one would have fought is pinned by
        // `TheLumpsAspectAndTheErosionsStrengthAreOneCalibrationAndNotTwoNumbers`.
        CloudVerticalProfile Profile;

        float AnvilAltitudeKm;  // centre of the SECOND lobe; zero strength means the type has none
        float AnvilThicknessKm; // half-height of that lobe
        float AnvilStrength;    // how dense the anvil is against the tower that feeds it
        float DetailCharacter;  // 0 = wispy erosion, 1 = billowy — the type's edge, not its silhouette
        float DetailFactor;     // multiplies the layer's Detail Strength: how deeply the erosion cuts
        float DensityFactor;    // multiplies the layer's Density Scale: how much matter this type is
        float ExtinctionFactor; // multiplies the layer's Extinction Scale: how opaque that matter is

        // ---- WHERE THIS TYPE IS, which is a different question from what shape it is -----------------
        //
        // These two are T3's, and they exist because the profile could not answer either of the two
        // questions below however it was authored:
        //
        //   * a cirrus is not a field of blobs that happen to be thin, it is a set of FIBROUS BANDS
        //     drawn out along the wind. No profile makes a band out of a round patch: the patch is the
        //     placement field's, and until T3 there was one placement field, isotropic, shared by
        //     everything in the sky;
        //   * a stratocumulus deck one kilometre thick made of three-kilometre cells is a single cell
        //     filling the zenith. The cell size is the placement field's period, and one period for the
        //     whole layer means the type that wants small cells and the type that wants large ones
        //     cannot both have them.
        //
        // Unreal answers the same two with `Layout_CloudGlobalPattern` — one CHANNEL of placement per
        // type — and `Layout_CloudPerTypeScale`, a scale per type on top of it (decision D-14,
        // Docs/Clouds/ANALYSIS_APPROACH.md §7, and EpicDoc_CloudMaterial.md §2-3). These are that pair.

        // The period of THIS type's placement field, as a MULTIPLE of the layer's Weather Tile Size.
        //
        // RELATIVE AND NOT ABSOLUTE, which is the opposite of the choice made for the altitudes above, and
        // the reason is that the two numbers are anchored to different things. An altitude is anchored to
        // meteorology — a stratus at 400 m is a fact about weather — so §5.1 of the analysis demands it be
        // absolute or no test can catch a layer that has drifted. A placement period is anchored to the
        // CAMERA: Max View Distance divided by the tile is the number of times the field repeats between
        // the viewer and the vanishing point, and five repeats against twenty is the difference between a
        // cumulus field and unmissable moire (CALIBRATION.md §4). That ratio is the layer's to keep, so a
        // type states how much coarser or finer than the layer it is and the pair stays calibrated.
        float PlacementScale;

        // How much longer the placement field's period is ALONG THE WIND than across it. 1 is a round
        // patch; above 1 the patches are drawn out downwind into bands.
        //
        // THE AXIS IS THE WIND'S rather than a second authored angle, because that is what the shape is
        // made of: fibrous cirrus is ice falling through a shear, and the streak lies along the flow by
        // construction. A separate angle would be a second statement of a direction the layer already
        // carries — two values that can disagree, the §2.3.1 defect class.

        /// Same authored state? DEFAULTED so the compiler generates it from every member — see the long
        /// note at CloudVerticalProfile::operator==. Read by the cloud documents' GetDiskState.
        [[nodiscard]] bool operator==( const CloudTypeShape& ) const = default;
        float PlacementAnisotropy;
    };

    /**
     * HOW MANY KINDS OF CLOUD ONE SKY HOLDS AT ONCE, and the number is structural rather than chosen.
     *
     * The profile table is one RGBA image and a type owns one CHANNEL of it — which is Unreal's
     * arrangement exactly: its three layout textures fix four types to R, G, B and A (Stratocumulus,
     * Altostratus, Cirrostratus, Nimbostratus — EpicDoc_CloudMaterial.md §2-3). Four channels, four types.
     *
     * The LIBRARY on disk is not limited by this at all; a project may ship a hundred `.decloudtype`
     * files. What is limited is how many of them a single layer carries, and the limit is the width of a
     * texel.
     */
    inline constexpr uint32_t kCloudSpeciesSlots = 4;

    /// The shell a set of types needs: the union of their altitude ranges. Bottom above top means "no
    /// active type", which the packer answers with the built-in default rather than with a degenerate
    /// shell.
    struct CloudEnvelopeKm
    {
        float BottomKm;
        float TopKm;
    };

    /**
     * DOES THIS TYPE HAVE AN ANVIL AT ALL — asked in ONE place, because two places asked it differently and
     * the disagreement inflates the shell.
     *
     * The bake emits the canopy only when BOTH numbers are past these thresholds
     * (Engine/Assets/CloudProceduralVolume.cpp, the anvil block); the envelope below used to ask only
     * `AnvilStrength > 0`. A strength inside `(0, kCloudAnvilMinStrength]` is a legal `.decloudtype` — the
     * loader's own range is [0, 1] and its only cross-check is that a strength above zero comes with a
     * thickness above zero — so a type could declare a canopy at 16 km that the bake refuses to draw and
     * still take the shell up there. Nothing is drawn, nothing is logged, and the layer pays twice:
     *
     *   * the modelling volume has a FIXED kCloudProceduralVolumeHeight rows spread over the shell, so a
     *     shell stretched from 0.40 km to 15.85 km takes the vertical voxel from 12.5 m to 495 m and a
     *     stratus deck stops being expressible by the trilinear fetch at all;
     *   * the march's step is `segment / MaxSteps` once the segment passes
     *     CLOUD_DISTANCE_TO_MAX_STEPS_KM, so a taller shell is a longer segment and a coarser SEARCH —
     *     measured on a 0.15..9.40 km shell against a 2.20..5.80 km one, the resolvable chord at 45° goes
     *     159 m to 409 m.
     *
     * Both halves are the same defect: a shell that is not the shell the bake fills. One predicate, read by
     * the envelope here and by the generator there, is what makes that impossible rather than merely
     * unlikely.
     */
    inline constexpr float kCloudAnvilMinStrength    = 1e-3f;
    inline constexpr float kCloudAnvilMinThicknessKm = 1e-4f;

    inline bool CloudTypeHasAnvil( const CloudTypeShape& shape )
    {
        return shape.AnvilStrength > kCloudAnvilMinStrength && shape.AnvilThicknessKm > kCloudAnvilMinThicknessKm;
    }

    // The bottom and the top of the shell a layer of this type needs. The ANVIL is above the tower, so
    // the top is not simply TopAltitudeKm — a type whose second lobe is outside the shell would have that
    // lobe silently cut off by the layer geometry, which is the "sky was a ceiling" defect in a new
    // costume (commit 54330ab9).
    inline float CloudTypeBaseKm( const CloudTypeShape& shape )
    {
        return shape.BaseAltitudeKm;
    }

    inline float CloudTypeTopKm( const CloudTypeShape& shape )
    {
        const float anvilTop = CloudTypeHasAnvil( shape ) ? shape.AnvilAltitudeKm + shape.AnvilThicknessKm : 0.0f;
        return std::max( shape.TopAltitudeKm, anvilTop );
    }

    /**
     * THE ENVELOPE OF A SET, and it is the union of the members' ranges rather than any of them.
     *
     * This is the same rule T0 wrote for one type — the shell is computed, never authored — carried
     * unchanged to a set, which is the whole claim T1 made about the shape of this code and the one T3 had
     * to check. A union and not an intersection: a stratocumulus deck from 0.6 to 1.6 km standing beside a
     * congestus tower from 2.2 to 5.8 km needs a shell from 0.6 to 5.8, and marching only where they
     * overlap would render neither.
     *
     * @p count above kCloudSpeciesSlots is clamped rather than read past; a count of zero returns an
     * inverted range, which is the caller's signal that no type is active.
     */
    inline CloudEnvelopeKm CloudTypeSetEnvelopeKm( const CloudTypeShape* shapes, uint32_t count )
    {
        CloudEnvelopeKm envelope{ 0.0f, -1.0f };

        const uint32_t used = std::min( count, kCloudSpeciesSlots );
        for ( uint32_t i = 0; i < used; ++i )
        {
            const float base = CloudTypeBaseKm( shapes[i] );
            const float top  = CloudTypeTopKm( shapes[i] );

            if ( envelope.TopKm < envelope.BottomKm )
            {
                envelope.BottomKm = base;
                envelope.TopKm    = top;
                continue;
            }

            envelope.BottomKm = std::min( envelope.BottomKm, base );
            envelope.TopKm    = std::max( envelope.TopKm, top );
        }

        return envelope;
    }

} // namespace Desert::Graphic
