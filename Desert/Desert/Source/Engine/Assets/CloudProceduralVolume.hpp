#pragma once

#include <Engine/Assets/CloudLayout.hpp>
#include <Engine/Assets/CloudModellingVolume.hpp>
#include <Engine/Graphic/Clouds/CloudTypeShape.hpp>

#include <Common/Content/DerivedDataCache.hpp>
#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Desert::Assets
{
    /**
     * @file
     * @brief The PROCEDURAL modelling volume: a camera-centric 3D texture whose shape field is a sum of
     *        smoothed volumetric lumps placed by a hash, joined by the same exponential smooth minimum
     *        phase Э4 built for sculpted bodies.
     *
     * WHY IT EXISTS, AND THE DEFECT IT IS THE CURE FOR. The procedural producer took its coverage from a
     * threshold on the Alligator noise, and `Alligator = best - second` IS ZERO WHEREVER TWO FEATURE
     * POINTS CONTRIBUTE EQUALLY. There is a wall of zeros between every pair of cells, so two lobes cannot
     * merge at any setting of any slider, and a procedural sky reads as a DECK OF SEPARATE CUSHIONS rather
     * than as a convective mass. Three tasks measured that independently — T2 measured the field, T0 showed
     * the profile does not move it horizontally, T3 showed that several independent placement fields do not
     * help — and each of them worked around it rather than removing it.
     *
     * The exponential smooth minimum has no wall of zeros. Merging is its DEFINING PROPERTY and not a
     * setting: `-r*ln(SUM exp(-d_k/r))` of two overlapping solids is one connected body with a smoothed
     * crease, and no choice of r can put a zero between them. That is the whole of the cure, and it is
     * variant C of Docs/Clouds/ANALYSIS_APPROACH.md §3 point 2 — approved as decision D-1 and then
     * substituted for the cheaper threshold — put back.
     *
     * WHAT IS REUSED RATHER THAN REWRITTEN. Everything below the placement: `CloudModellingBlob`, the three
     * primitives, the signed distances, the join, the canonical sort and the softmax weights are phase Э4's
     * and are called here, not copied (Engine/Assets/CloudModellingVolume.hpp). This file adds exactly two
     * things Э4 did not have — a hash that puts the lumps in the sky instead of an artist, and a bake that
     * scrolls with the camera and wraps at the region's edge.
     *
     * WHAT IS NEW AND HAS TO BE ARGUED FOR:
     *
     *   * THE VOLUME IS PERIODIC IN X AND Z. Every sampler in this engine is LINEAR/REPEAT, so a volume
     *     that was not periodic would show a HARD SEAM at every region boundary — the field would jump from
     *     one edge's value to the other's. Wrapping the lumps across the boundary at bake time costs one
     *     extra splat per lump near an edge and makes REPEAT exactly continuous, which is what turns "what
     *     happens outside the region" from a defect into the degenerate far path of §3 point 3: beyond the
     *     region the sky is this region again, at a distance where a cloud's angular size is already below
     *     what the march resolves (§4.5).
     *
     *   * THE REGION SNAPS TO THE LUMP LATTICE. A lump's identity is the hash of its ABSOLUTE cell index,
     *     so a cell that is in the region before a shift and after it produces the identical lump — which
     *     is what makes the field invariant under scrolling anywhere more than the influence cutoff inside
     *     both regions. Snapping to the lattice is what makes "the same cell" a meaningful statement;
     *     without it every shift would re-roll every lump and the sky would boil. The same snap, for the
     *     same reason, that the cloud shadow map takes against its 20 km grid, where it was measured at
     *     0.545/255 of boiling with the snap against 2.291/255 without it.
     */

    /// The volume's shape, FIXED like the sculpted volume's is, and for the same reason: the shader needs
    /// the height as a compile-time number, and two statements of one extent is the defect class §2.3.1
    /// names.
    ///
    /// ONLY THE HEIGHT IS MIRRORED, by CLOUD_PROCEDURAL_VOLUME_HEIGHT in
    /// Editor/Resources/Shaders/Common/CloudField.glslh, and Desert/Tests/Engine/CloudField asserts the two
    /// agree — that suite compiles the header as C++, which is where the comparison can be made. The width
    /// and the depth are NOT mirrored and must not be: the horizontal mapping is
    /// `(world - origin) * invRegionSize`, which is already in texture units.
    ///
    /// THIS COMMENT USED TO NAME THE WRONG SUITE, and a sabotage run is what found it: setting the shader's
    /// height to 64 left both suites green, because neither of them included this header beside that one.
    /// A claim about which test protects a relation is itself a thing that has to be true.
    ///
    /// 256 x 32 x 256 RGBA8 is 8.00 MiB, and the number is chosen by TWO bounds rather than by ambition:
    ///
    ///   * BELOW, by what the march can find. Trilinear filtering cannot express a feature narrower than
    ///     two voxels, and the march SEARCHES at CloudFinestResolvableChordKm — 125 m at the component's
    ///     Max Steps — so a voxel finer than 62.5 m would put structure into the volume that no ray can be
    ///     relied on to sample. At the shipped 48 km region a voxel is 187.5 m, which is three times that
    ///     bound with room to spare.
    ///   * ABOVE, by the memory decision D-9 grants the whole subsystem. Docs/Clouds/CALIBRATION.md §A0
    ///     measured 20.67 MiB occupied before this volume existed and 4.00 MiB per sculpted body; 8.00 MiB
    ///     here leaves 35.33 MiB, which is still the EIGHT hero clouds phase A2 shipped. Variant C's
    ///     512 x 512 x 32 would have been 32.00 MiB and would have COST six of those eight — the sentence
    ///     here read "cut that to six", which is the number lost rather than the number left: 20.67 plus
    ///     32.00 of 64 leaves 11.33 MiB, and that is TWO bodies.
    ///
    /// The vertical axis is the short one and is the LAYER, not a distance: 32 voxels spread over the
    /// shell's own thickness, so a 3.5 km layer gives 109 m of vertical resolution and a thin stratus deck
    /// gets the same 32 samples a cumulonimbus does.
    ///
    /// RAISING THE HORIZONTAL PAIR WAS BUILT, MEASURED AND REFUSED — the tenth refusal of the near-field
    /// programme, and the one that closes the LAST place the complaint could have lived. 512 x 32 x 512 is
    /// 32.00 MiB, four times this, and it is the honest variant to price: the vertical is already the finer
    /// axis (109 m against 187.5 m), so doubling it too would spend 64 MiB to sharpen the axis that is not
    /// the bottleneck. Against decision D-9's 64 MiB and CALIBRATION.md §A0's 20.67 MiB occupied before this
    /// volume, 32 MiB leaves 11.33 MiB — TWO sculpted hero bodies at 4.00 MiB each, against the eight
    /// phase A2 shipped.
    ///
    /// WHAT THE FOUR TIMES BOUGHT, on the shipped congestus at Coverage 0.35 through the whole seam
    /// (Desert/Tests/Engine/CloudField's own surface-roughness instrument, which stops each column at unit
    /// optical depth exactly as the march does):
    ///
    ///     voxel      penetration    silhouette roughness at a lag of
    ///                               80 m     160 m     320 m
    ///     187.5 m       657 m       94.3 m   181.5 m   343.2 m
    ///      93.8 m       578 m       96.0 m   185.0 m   348.6 m
    ///
    /// **+1.7 m of silhouette on 94.3, 1.8 %, for four times the memory** — the same 1.8 % D-31 measured
    /// for the fractal noise and refused, arrived at from the opposite end. The mechanism is Р9's ratio
    /// and it is why this could not have worked: the silhouette is a LINE INTEGRAL over the 657 m the eye
    /// looks through before the cloud is opaque, so structure shorter than that is averaged away before it
    /// reaches the eye. Two voxels is 375 m at this resolution and 188 m at twice it — both far inside the
    /// integral, so both are filtered. The voxel was never the binding constraint.
    ///
    /// AND IT IS NOT ONLY MEMORY. `Clouds: March` GPU SELF time out of the pass's own profiler line —
    /// never a frame-to-frame difference — on `Clouds_Protocol` at 300 frames, the two binaries ALTERNATED
    /// in one session because the machine is shared with other agents, minimum of three: **12.882 ms at
    /// 256 against 14.722 ms at 512, +1.84 ms, +14.3 %**. The spreads are wide and honest about why — 256
    /// ran 12.882 / 16.626 / 22.946 and 512 ran 15.063 / 14.722 / 16.674, with the later samples of each
    /// taken while this session was compiling — which is exactly why the minimum is the statistic. The
    /// march does not read more samples at 512; it reads the same ones out of a texture four times the
    /// size, so this is cache and nothing else.
    ///
    /// THE BAKE PAYS TOO, and it is the one a user waits on: one 48 km region in Debug goes 6.84 s to
    /// about 26 s, four times, on the count of voxels.
    ///
    /// WHAT WOULD CHANGE THE ANSWER: only a shorter integral. A sky whose penetration is a couple of
    /// hundred metres — a genuinely broken cumulus field, or the physical extinction D-32 priced and left
    /// to the owner — would put two voxels OUTSIDE the averaging, and this table would have to be taken
    /// again there. Until then a finer volume is four times the memory for a fiftieth of the roughness it
    /// was bought for.
    ///
    /// AND THE FRAMES SAY IT IN THE SHAPE THAT MATTERS, which the table alone cannot: the change is LIVE
    /// and it is INVISIBLE. Six points on `Clouds_Showcase` from the owner's camera (0, 200, 0), 90 frames,
    /// 1280 x 766, against a measured repeat floor of zero — 17.8 % to 36.7 % of pixels differ, so the
    /// finer volume is unquestionably reaching the screen, while the largest delta anywhere is 15 of 255 at
    /// the zenith and 2 of 255 at the horizon, and the mean is between 0.06 and 0.18 of one grey level. The
    /// error field's coherence is 0.85 to 1.43 — a slowly-varying shift rather than added structure, which
    /// is the instrument saying in its own units that nothing was sharpened. The zenith is still one soft
    /// mass and the mid angle is still smooth lobes, and those are the two points the complaint lives at.
    ///
    /// THE HORIZONTAL SIDE IS NOW A PARAMETER AND THE VERTICAL ONE IS NOT, which is the one thing about
    /// this block that changed for O8 and the reason it could change at all. The shader knows the HEIGHT as
    /// a compile-time number (it pulls the vertical fetch in by half a texel) and it knows nothing of the
    /// width or the depth, because the horizontal mapping is `(world - origin) * invRegionSize` and is in
    /// texture units already. So a view may bake a coarser grid over the same region without a shader
    /// recompile, a second sampler or a second march — which is what makes a 512-pixel asset preview able
    /// to stop paying a whole level's bake. `CloudProceduralFieldParams::VolumeSideVoxels` carries it, and
    /// the constant below is that field's DEFAULT rather than a second statement of it.
    ///
    /// ONE SIDE AND NOT A WIDTH AND A DEPTH. `RegionSizeKm` is one number for both horizontal axes, so a
    /// non-square grid over it would give anisotropic voxels — two numbers that must agree is the defect
    /// class §2.3.1 names, and the pair that stood here was equal in every build this engine has ever had.
    inline constexpr uint32_t kCloudProceduralVolumeSide   = 256u; // x and z, world east and north
    inline constexpr uint32_t kCloudProceduralVolumeHeight = 32u;  // y, up, spanning the layer exactly

    /// The coarsest grid this subsystem will bake, and the number is MEASURED rather than argued — the
    /// first draft of it was 64 on a derivation that turned out to name the wrong mechanism, and both the
    /// frames and Desert/Tests/Engine/CloudProceduralField said so.
    ///
    /// WHAT ACTUALLY BINDS IS THE LUMP FLOOR, NOT THE CELL FLOOR. `CloudProceduralLumpFloorKm` is
    /// `max( 0.5 * ResolvableChordKm, RegionSizeKm / side )` — a HALF-EXTENT — so at the shipped 48 km
    /// region it is 187.5 m at 256, 375 m at 128 and 750 m at 64, while the narrowest lump the shipped
    /// congestus emits is 459 m ACROSS, i.e. a half-extent of 229.5 m. At 256 nothing is clamped; at 128
    /// only the narrowest tail is; at 64 the clamp is three times the body it is applied to and it applies
    /// to nearly every lobe in the sky. The cell floor — four voxels, which lands on the shipped 3 km
    /// lattice at exactly 64 — is the bound the first draft cited, and it is not the one that fires first.
    ///
    /// MEASURED, as the fraction of the sky covered top-down at Coverage 0.35 (the suite prints it):
    ///
    ///     side 256 -> 0.3177 of the sky      (the reference)
    ///     side 128 -> 0.3163                 -0.0014, inside the estimator's own noise
    ///     side  64 -> 0.3613                 +0.0436 — FOUR POINTS OF SKY INVENTED BY THE CONTAINER
    ///
    /// and the frames say the same thing in the shape that matters: at 128 the six-point sweep is the same
    /// clouds in the same places with softer edges (mean 1.1 to 6.2 of 255 against a repeat floor of
    /// exactly zero), while at 64 a cumulus has visibly swollen and a gap between two lobes has closed.
    /// A budget that changes the subject is not a budget, so 64 is REFUSED rather than offered.
    ///
    /// WHAT WOULD CHANGE THE ANSWER: a much smaller region. The quantity that matters is the VOXEL against
    /// the narrowest body, not the side against a number, and `RegionSizeKm / side` is what ties them — a
    /// layer authored at a 16 km region has a 125 m voxel at 128 and could afford a coarser grid than this
    /// floor allows. Nothing in the repository authors one, so the floor is stated as a side; re-measure
    /// the table above before relaxing it for such a layer.
    inline constexpr uint32_t kCloudProceduralVolumeSideMin = 128u;

    inline constexpr uint32_t kCloudProceduralBytesPerVoxel = 4u;

    /// The exact size of the byte block @ref BakeCloudProceduralVolume returns for a grid of @p sideVoxels.
    /// A FUNCTION and not a constant, because the side is a parameter now: a constant would be the size of
    /// one particular volume being read as the size of every volume, which is how a caller ends up
    /// asserting a length that belongs to a different view.
    inline constexpr uint64_t CloudProceduralVoxelBytes( uint32_t sideVoxels )
    {
        return static_cast<uint64_t>( sideVoxels ) * kCloudProceduralVolumeHeight * sideVoxels *
               kCloudProceduralBytesPerVoxel;
    }

    /// A LUMP'S HEIGHT OVER ITS OWN WIDTH — the one ratio that turns a cluster's single size into both of a
    /// lump's radii. The decision behind it, the alternative it was chosen over and the ladder it was read
    /// off are at its use site in CloudProceduralVolume.cpp; what belongs HERE is why it is public at all.
    ///
    /// IT IS EXPORTED BECAUSE IT IS ONE HALF OF A CALIBRATION WHOSE OTHER HALF LIVES IN ANOTHER FILE, and
    /// that pairing was a mine for a whole phase. A taller lump makes a body optically thicker per metre, so
    /// the SAME erosion cut moves the visible surface a shorter distance — and ECS::VolumetricCloudData's
    /// Detail Strength is fixed by a floor on exactly that distance (the chord the march can be relied on to
    /// find, CloudFinestResolvableChordKm). §SIL raised this constant, measured, framed and committed the
    /// result, and only the full test sweep found that it had pushed §DS's floor through the floor — 101 m
    /// against 125. The two numbers had never been named in one place, so nothing could say so.
    ///
    /// Desert/Tests/Engine/CloudField
    /// (`TheLumpsAspectAndTheErosionsStrengthAreOneCalibrationAndNotTwoNumbers`) reads THIS symbol and the
    /// component's own default, bakes the volume the pair produces and asserts the product still clears the
    /// march. Anyone moving either number alone gets a red test that names the other one.
    inline constexpr float kCloudLumpVerticalOverHorizontal = 0.75f;

    /**
     * @brief Everything the placement needs about one KIND of cloud, and nothing about where the camera is.
     *
     * It is `Graphic::CloudTypeShape` plus the two numbers a shape cannot carry because they belong to the
     * layer — the lattice the layer's Weather Tile Size sets, and the wind axis the layer's direction sets.
     * Kept as a separate struct rather than passing the shape and the layer separately so that the
     * generator has ONE input and a test can hold a single value.
     */
    struct CloudProceduralSpecies
    {
        /// The kind of cloud, exactly as the `.decloudtype` asset stores it. Every one of its fourteen
        /// numbers is read here or by the march; none is decorative.
        Graphic::CloudTypeShape Shape{};

        /// The side of the lattice cell one cluster of lumps is drawn in, kilometres, ALREADY carrying this
        /// species' Placement Scale. A cluster is what used to be one cell of the Alligator, and it is now
        /// a pile of overlapping lumps rather than a single cushion.
        float CellKm = 3.0f;

        /// How much longer the cell is along the wind than across it — this species' Placement Anisotropy,
        /// applied to the LATTICE rather than to a noise frequency. A cirrus' fibrous bands are cells drawn
        /// out downwind, which is the same statement the noise made with its basis vectors and is exact
        /// here rather than approximate.
        float Anisotropy = 1.0f;
    };

    /**
     * @brief Everything the bake needs. Pure input: no camera object, no device, no asset manager.
     *
     * DISTANCES ARE KILOMETRES throughout, matching Common/CloudGeometry.glslh and the sculpted volume
     * beside it. The one thing here that is not a length is the seed.
     */
    struct CloudProceduralFieldParams
    {
        /// The horizontal side of the region the volume covers, kilometres. It is also the PERIOD the
        /// volume tiles with, because the bake wraps — see the file note.
        float RegionSizeKm = 48.0f;

        /// How many voxels the region is divided into on EACH horizontal axis. The vertical count is fixed
        /// at @ref kCloudProceduralVolumeHeight, which the shader mirrors.
        ///
        /// IT IS THE BAKE'S COST, ALMOST EXACTLY. The bake is a loop over `side * side` columns of 32 rows,
        /// so halving this quarters the work — measured rather than assumed at the definition of
        /// `ECS::VolumetricCloudData::VolumeResolution`, which is the field an artist actually moves. It is
        /// here, in the pure parameters, because four other pure functions are written in terms of it (the
        /// voxel-against-the-march bound in ValidateCloudProceduralParams, CloudProceduralLumpFloorKm,
        /// CloudProceduralCellExtentKm and the bake itself), and a resolution passed beside the parameters
        /// would let those four disagree with the grid actually written.
        ///
        /// COMPARED BY CloudProceduralParamsEqual, necessarily: two volumes of different resolutions over
        /// the same region are different bytes, and a cache that could not see this would show a preview
        /// its own budget had already been changed away from.
        uint32_t VolumeSideVoxels = kCloudProceduralVolumeSide;

        /// The shell the volume spans vertically: the layer's base altitude and its thickness, kilometres.
        /// The volume's 32 rows are spread over exactly this, so a voxel's height is Thickness/32.
        float LayerBottomKm    = 1.5f;
        float LayerThicknessKm = 3.5f;

        /// The radius over which two lumps FUSE, kilometres — the reciprocal of the smooth minimum's
        /// sharpness, and the same field the sculpted recipe carries.
        ///
        /// IT MUST STAY SMALL AGAINST A LUMP, and the reason is arithmetic rather than taste: the join
        /// inflates the surface by `BlendRadiusKm * ln(sum of weights in range)`, so with hundreds of
        /// overlapping lumps a generous radius does not soften the crease, it FLOODS THE SKY. The fusion
        /// comes from the lumps OVERLAPPING — which is how the shipped sculpted recipe makes one body out
        /// of eight — and the radius only decides how sharp the crease between them is.
        float BlendRadiusKm = 0.06f;

        /// How deep inside the body the Dimensional Profile reaches 1, kilometres. The normalisation of the
        /// distance field, exactly as in the sculpted volume.
        float ProfileDepthKm = 0.35f;

        /// What fraction of the lattice's cells carry a cluster, 0..1. THE SLIDER NOW ADDRESSES A FRACTION
        /// OF SKY DIRECTLY, which is what the quantile map it replaces was introduced to fake: a cell is
        /// alive when its hash falls below this number, so 0 is exactly empty and 1 is exactly full for any
        /// seed, with no distribution to calibrate against.
        float Coverage = 0.24f;

        /// How sharply a cell goes from empty to full, > 0. A cell whose hash lands just under the coverage
        /// threshold grows a SMALL cluster; one well under it grows a full-sized one, and this is the width
        /// of that ramp inverted. It is the same field the layer already exposes and it keeps its meaning:
        /// above 1 the sky is decisively cloud or decisively clear, below 1 the sizes spread out.
        float CoverageContrast = 1.0f;

        /// The realization. Two layers with different seeds have unrelated skies; the same seed and the
        /// same region origin always bake the same bytes.
        uint32_t Seed = 1u;

        // -----------------------------------------------------------------------------------------------
        // THE FOUR NUMBERS THAT DECIDE WHETHER THE SKY READS AS A GRID
        // -----------------------------------------------------------------------------------------------
        //
        // WHAT WAS MEASURED, AND IT IS THE REASON THESE EXIST. Tools/LatticePeak takes the autocorrelation
        // of the baked field's top-down projection and reports the prominence of the bump standing on a
        // multiple of the lattice's own period. On the field that shipped before them the bumps stood at
        // 6.000, 9.000 and 12.000 km against a predicted cell of 3.000 km — the multiples of the lattice,
        // to the voxel — at eight to eighteen times the estimator's own noise. The sky was a grid, and it
        // was a grid because every alive cell carried EXACTLY ONE cluster, of very nearly ONE SIZE,
        // displaced by at most a third of the cell it was born in.
        //
        // Each of the four attacks one of those, and each is defaulted to the value CALIBRATION.md §RW
        // measured rather than to the value that makes it inert.

        /// How many clusters an alive cell carries, on average. The count is drawn per cell so that a cell
        /// holds a whole number of them with this mean — two, then one, then three — and "exactly one per
        /// cell" stops being a property of the field at all.
        ///
        /// THE CLUSTER SHRINKS AS THE COUNT RISES, by this number to the power of kDensityCompensation, so
        /// that the matter in the sky is REDISTRIBUTED rather than added. That is what keeps the Coverage
        /// slider meaning what it says: without the compensation, raising the density would raise the sky's
        /// cover and the mapping decision D-20 re-authorised every scene against would have to be measured
        /// again. The exponent is 0.40 and NOT a half — see kDensityCompensation, which says why the half
        /// is the answer to a different question.
        ///
        /// 1.75 AND NOT 2.5, AND THE MOVE IS A MEASUREMENT — see CALIBRATION.md §RW2. §RW shipped 2.5 on
        /// the argument that the count is what breaks the lattice. Measured one setting at a time at 32
        /// realisations, with everything else at the values that ship, the lattice bump is inside the
        /// estimator's noise from 1.5 upward and only comes back at 1.0:
        ///
        ///     density   1.0     1.5     1.75    2.0     2.5
        ///     LATTICE   0.0264  0.0007  0.0017  0.0027  0.0000
        ///     x noise   5.9     0.2     0.3     0.6     —
        ///
        /// so 2.5 buys nothing the grid can see, and it COSTS the picture: the cluster narrows as this
        /// rises, and at 2.5 every cloud in the frame had shrunk to the size the far ones already were.
        /// The mean horizontal chord through the baked field goes 1.784 / 1.705 / 1.634 / 1.541 km over
        /// those four settings and the frame's contrast goes 0.378 / 0.379 / 0.374 / 0.349 against the
        /// 0.384 of the sky the owner accepted. 1.75 is the lowest setting that is a whole measured step
        /// away from the one that fails and is on the contrast plateau.
        float PlacementDensity = 1.75f;

        /// How far a cluster may wander from its lattice site, in CELLS — 1.0 means it may sit anywhere in
        /// a box one cell wide centred on its site, so it crosses into its neighbours' territory.
        ///
        /// WHAT THE OLD THIRD-OF-A-CELL BOUND ACTUALLY BOUGHT, because the comment it carried claimed more
        /// than it delivered. It was written to keep a cluster inside the cell whose hash made it. The
        /// property that MATTERS is a different one — the set of cells generated must be exactly one
        /// period's worth, or the bake's wrap places each of them twice — and that property does not
        /// depend on where inside the period a cluster sits: a cluster leaving through one face re-enters
        /// through the opposite one, because the wrap is what makes the volume periodic in the first
        /// place. What the bound really bought is that a region SHIFT changes the sky only within a third
        /// of a cell of the region's faces, which is 24 km from the camera. At 1.0 that strip goes from
        /// 1.0 km to 1.5 km at the same distance.
        float PlacementScatter = 1.0f;

        /// How much cluster sizes spread, 0..1. Zero makes every cluster the size its cell's fill says;
        /// one makes the largest about four times the width of the smallest.
        ///
        /// THE SPREAD IS UNIFORM IN AREA AND NOT IN RADIUS, so that the mean area a cluster covers is
        /// exactly what it was at zero. Spreading the radius uniformly instead would have raised the mean
        /// area by a twelfth of the spread squared and moved the Coverage mapping with it.
        float PlacementSizeVariety = 0.75f;

        /// The world size over which the LARGE-scale modulation of coverage repeats, kilometres. It is the
        /// scale of a weather system rather than of a cloud: patches of busy sky and patches of clear sky,
        /// which is the structure a lattice with one number for the whole sky cannot have.
        float PatchTileKm = 21.0f;

        /// How hard that modulation pushes, 0..1. Zero is a uniformly busy sky — which is what the owner
        /// described as "the whole sky is cloud" — and one lets a patch reach nearly empty and nearly
        /// solid. It is SYMMETRIC about the slider's own value, so it redistributes cloud rather than
        /// adding or removing it.
        float PatchStrength = 0.60f;

        /// The horizontal wind direction the lattice's anisotropy is measured against, world XZ. Need not
        /// be normalized; a zero vector means east, which is what CloudSpeciesPlacementBasis also does.
        glm::vec2 WindAxis{ 1.0f, 0.0f };

        // -----------------------------------------------------------------------------------------------
        // THE PAINTED LAYOUT — the artist's own answer to "where are the clouds"
        // -----------------------------------------------------------------------------------------------
        //
        // WHY IT IS HERE AND NOT IN THE MARCH. Unreal reads its three layout textures on every evaluation
        // of the cloud material — once per march step and again at seven further sites on the shadow rays
        // (Docs/Clouds/RESEARCH_LAYOUT_TEXTURES.md §1.2) — because it has no precomputed field at all: the
        // material IS the field. We have one, and placement is decided here, once per lattice cell, a few
        // hundred times per bake. So the painting joins where `Coverage` already joins, and the hottest
        // pass of the frame gains not one instruction.
        //
        // WHAT REPLACES WHAT. The painted pattern and the procedural patch field decide the SAME number —
        // how busy this part of the sky is — and two mechanisms for one number is the second path §1.3 and
        // §4.2 of the contract forbid. So they are not both applied: the painting is the source when one is
        // bound and turned up, the hash is the source otherwise. CloudCellCoverage in the .cpp is the one
        // place that choice is made.

        /// Where the painting sits and how hard it pushes. Every field of it changes the baked volume, so
        /// every field of it is compared by CloudProceduralParamsEqual.
        CloudLayoutPlacement LayoutPlacement{};

        /// The painting the PATTERN is read from, or null when the layer has none bound — which is what
        /// every scene in this repository carries and what must keep rendering the frame it rendered
        /// before. Only its @c Pattern table is read here; a layout bound to this slot that carries only a
        /// mask contributes nothing, and CloudCellCoverage's `HasPattern` is where that is decided.
        ///
        /// A SHARED POINTER AND NOT A COPY, because the tables are up to 5 MiB and these parameters are
        /// held by the renderer across frames and re-used at every region shift; a copy per rebuild would
        /// be a megabyte memcpy for a value that never changes. Shared rather than borrowed for the
        /// lifetime reason spelt out on Runtime::CloudLayoutService::Get.
        ///
        /// IT IS NOT COMPARED BY CloudProceduralParamsEqual — the CONTENT HASH inside it is, which is the
        /// same number stated once rather than twice. Comparing the pointer would call a re-bake every time
        /// the asset was reloaded into a different allocation with identical pixels; comparing the pixels
        /// would be a megabyte memcmp on a path that runs whenever a slider moves.
        std::shared_ptr<const CloudLayoutData> PatternSource;

        /// The painting the add/remove MASK is read from, or null. Only its @c Mask table is read.
        ///
        /// TWO SOURCES AND NOT ONE, which is decision O-4 and Unreal's own arrangement:
        /// `Layout_CloudGlobalPattern` and `Layout_GlobalCloudMask` are separate texture parameters there,
        /// so a sky can keep its placement and swap the region being cleared, or the reverse, without
        /// re-authoring a single file. Pointing both at the same `.dclayout` is the ordinary case and
        /// costs one extra pointer copy; the service hands back the same shared object for both.
        std::shared_ptr<const CloudLayoutData> MaskSource;

        /// The finest chord the march can be relied on to FIND, kilometres — CloudFinestResolvableChordKm
        /// at the component's Max Steps, handed in rather than assumed.
        ///
        /// IT IS AN INPUT AND NOT A CONSTANT because it is one half of a RELATION, and this programme has
        /// paid for that relation twice. The generator clamps every lump so that its smallest diameter
        /// clears this number: a lump the march cannot find is not a thin cloud, it is speckle that appears
        /// and disappears with the ray's jitter. Desert/Tests/Engine/CloudProceduralField asserts the
        /// clamp holds for every shipped type at every quality tier.
        float ResolvableChordKm = 0.125f;

        /// The kinds of cloud in this layer, in the packed order the renderer resolved them. At most
        /// Graphic::kCloudSpeciesSlots, because a species owns one CHANNEL of the volume.
        std::vector<CloudProceduralSpecies> Species;
    };

    /// Rejects parameters the generator cannot honour, with the offending number in the message. Pure, so
    /// the component's validation and the bake refuse for the same reason rather than disagreeing.
    Common::BoolResultStr ValidateCloudProceduralParams( const CloudProceduralFieldParams& params );

    /**
     * @brief The LAYOUT half of the check above, on its own.
     *
     * IT EXISTS BECAUSE THE TWO CALLERS WANT DIFFERENT THINGS FROM A FAILURE, and calling the whole
     * validator for a layout question would give the wrong diagnosis. The bake refuses everything: an
     * invalid Coverage and an over-coarse painting are both reasons not to produce a volume. The RENDERER
     * cannot refuse — a scene must always draw a sky — so when the painting alone is at fault it drops the
     * painting and places the clouds procedurally. Handed the whole validator it would have dropped the
     * artist's painting because somebody mistyped a patch tile, and said so in a message naming the
     * painting.
     *
     * ValidateCloudProceduralParams calls this, so there is one statement of the relations and not two.
     *
     * It is both slots at once. @ref ValidateCloudProceduralLayoutTable is the one the renderer wants,
     * because with two independent sources a bad pattern must not cost the artist their mask.
     */
    Common::BoolResultStr ValidateCloudProceduralLayout( const CloudProceduralFieldParams& params );

    /// Which of the two layout inputs a question is about. Named rather than a bool, because "true means
    /// pattern" at a call site is a coin toss the reader has to resolve by opening this file.
    enum class CloudLayoutTable : uint8_t
    {
        Pattern,
        Mask
    };

    /// "pattern" / "mask", for a message an artist reads. One spelling, so the log and the panel cannot
    /// name the same slot differently.
    const char* CloudLayoutTableName( CloudLayoutTable table );

    /**
     * @brief The layout check for ONE of the two slots, ignoring the other entirely.
     *
     * WHY PER SLOT. The renderer drops what it cannot use and renders the rest, and since O-4 there are two
     * things it could be handed. Checked together, a pattern too coarse for the lattice would take the
     * mask down with it — an artist's region of cleared sky vanishing because a different file was wrong,
     * with the message naming neither. Checked apart, exactly the offending painting is dropped and the
     * log says which.
     *
     * An empty slot passes: nothing to be wrong. The PLACEMENT is not checked here — it belongs to both
     * slots and is @ref ValidateCloudProceduralLayout's first act.
     */
    Common::BoolResultStr ValidateCloudProceduralLayoutTable( const CloudProceduralFieldParams& params,
                                                              CloudLayoutTable                  table );

    /**
     * @brief The narrowest HORIZONTAL half-extent a lump may be given, kilometres.
     *
     * THE RELATION IT STATES, AND IT WAS MISSING. `ValidateCloudProceduralParams` already refuses a volume
     * FINER than the march — a voxel under half `ResolvableChordKm` fills the field with structure no ray
     * can be relied on to sample. The opposite direction was never stated anywhere: the generator's own
     * lump floor was `0.5 * ResolvableChordKm` — 62.5 m of radius, 125 m across — while trilinear filtering
     * cannot express anything narrower than TWO VOXELS, 375 m at the shipped 48 km region. So the generator
     * was authorised to emit lumps three times finer than the volume it writes them into, and what comes
     * back out of the sampler for one of those is not a small cloud but a smear whose size is the filter's.
     *
     * IT IS MEASURED AND NOT ARGUED, AND THE SHIPPED SKY IS NOT WHERE THE DEFECT IS — which is worth
     * saying plainly, because it is the difference between a repair and a regression risk. On the shipped
     * congestus at Coverage 0.35 the narrowest lump `GenerateCloudProceduralBlobs` emits over one 48 km
     * region is 459 m across, so this floor never bites and the baked field is unchanged to every digit the
     * seam's own instruments report. What it guards is the RANGE the placement sliders already permit, and
     * there the old floor let go completely — counted before this existed, at Coverage 1 so that the
     * population is the whole field:
     *
     *     PlacementDensity 8                     38 of  11 904 lumps under two voxels   (0.3 %)
     *     PlacementSizeVariety 1                 76 of   2 646                          (2.9 %)
     *     the smallest cell the cell floor allows
     *       (0.75 km)                        14 367 of  41 658                         (34.5 %)
     *     all three at once                 177 168 of 188 640                         (93.9 %)
     *
     * THE LAST TWO ROWS ARE THE REASON THIS EXISTS AS WELL AS A COMMENT. The cell floor is argued as "four
     * voxels is the narrowest CLUSTER the volume can carry with an inside and two edges" — and a cluster is
     * six lobes, so a cell that clears that bound by construction is full of lobes that do not. A slider
     * whose legal range produces a field the container cannot express is the dead-setting shape §1.3 of the
     * contract names, one level down.
     *
     * WHY THE VERTICAL AXIS IS NOT FLOORED BY THE VOLUME. The horizontal voxel is `RegionSize / Width` and
     * is a constant of the subsystem. The vertical one is `LayerThickness / Height` — the layer's own
     * thickness spread over 32 rows — so it is 12.5 m for a stratus-only layer and 312 m for a layer that
     * also holds a storm. Flooring a lump's height at one of THOSE would push a 400 m deck out of the band
     * its own asset declares in order to fit the volume, which is changing the sky to suit the container.
     * A thin type inside a tall layer is genuinely under-resolved vertically and no clamp can repair it;
     * the vertical keeps the march's floor, and the band clamp above it stays the type's own authority.
     */
    float CloudProceduralLumpFloorKm( const CloudProceduralFieldParams& params );

    /**
     * @brief The lattice cell's two side lengths, kilometres — longer along the wind, shorter across it,
     *        with the AREA held constant so that raising the anisotropy draws a cluster out into a band
     *        instead of making the sky emptier.
     *
     * PUBLIC BECAUSE IT IS THE PERIOD SOMETHING ELSE HAS TO PREDICT. `Tools/LatticePeak` measures the
     * autocorrelation of the baked field and states the lag it expects a lattice peak at; if that number
     * were computed a second time in the tool, a disagreement between the generator and the tool would
     * look like a clean sky. Two statements of one quantity is the defect class DEV_CONTRACT.md §2.3.1
     * names, so there is one statement and everybody calls it.
     */
    glm::vec2 CloudProceduralCellExtentKm( const CloudProceduralFieldParams& params,
                                           const CloudProceduralSpecies&     species );

    /**
     * @brief How much of the sky one cluster of unit radius covers, as the radius of the circle of the same
     *        AREA — for the TOWER alone, with no anvil over it.
     *
     * WHAT IT IS AND WHY IT IS NOT A FIT. It is the area of the union of a cluster's six lobes, projected
     * down, divided by pi and rooted: a pure consequence of the layout constants in
     * CloudProceduralVolume.cpp (six lobes a golden angle apart on a disc of 0.48 cluster radii that
     * narrows going up, each 0.62 radii wide at the base, each scaled by a wobble on [0.85, 1.15] and
     * displaced by up to 0.18 radii). It is computed by quadrature over that layout and NOT fitted to any
     * sky, so it does not move when the coverage, the cell, the density or the genus does.
     *
     * IT DEPENDS ON THE TYPE'S VERTICAL PROFILE AND ON NOTHING ELSE A TYPE AUTHORS, because the profile is
     * the only authored thing that reaches a lobe's WIDTH. It used to depend on `TopTaper`, which was that
     * profile expressed as one knob on a monotone law; the quadrature gave 0.9594 at a taper of 0 and
     * 0.9051 at 1, and the law between them is linear to 0.2 per cent — which is why two numbers ship
     * rather than a table.
     *
     * THOSE TWO NUMBERS SURVIVED THE CURVE, and the reason is measured rather than conservative — see the
     * definition. A curve is mapped onto that calibrated line by its MEAN LOBE WIDTH, taken at the stack's
     * own six heights, and the mapping is the identity for any curve that re-expresses a taper. So the
     * shipped library prices exactly as it did before the format moved, and an authored curve outside the
     * old law's reach EXTRAPOLATES the line rather than being clamped onto its end.
     *
     * Desert/Tests/Engine/CloudPlacementSpectrum re-measures it from the EMITTED lumps at three profiles on
     * every run, so the constants and the layout cannot drift apart in silence.
     */
    float CloudClusterTowerFootprintRadii( const Graphic::CloudVerticalProfile& profile );

    /// The mean of a type's lobe half-widths at the six heights the stack actually samples. Exposed
    /// because the footprint above is defined in terms of it and a test has to be able to say so.
    float CloudProfileMeanHalfWidth( const Graphic::CloudVerticalProfile& profile );

    /**
     * @brief How much wider than the calibrated tower this type's own body reaches, as a factor to divide
     *        the cluster's radius by so that the `Coverage` slider keeps meaning the sky.
     *
     * THE DEFECT IT EXISTS FOR, stated as the two numbers that disagreed. The Coverage mapping — the 0.68
     * alive exponent and the packing gain beside it — is a statement about the AREA one cluster covers, and
     * the generator already holds that area still against the three things that could move it: the density
     * (the count's own compensation), the size spread (a draw uniform in AREA) and the anisotropy (§SIL's
     * geometric mean). **The fourth was the type's own ANVIL, and nothing compensated it.** A cumulonimbus
     * canopy is a solid ellipse of `(1 + 0.8 * AnvilStrength)` cluster radii, concentric with the tower and
     * far wider than it, so a storm covered 2.5 times the sky the mapping had priced — and the slider read
     * 0.856 for a setting of 0.5, the largest lie left in it (CALIBRATION.md §CB).
     *
     * IT IS DERIVED AND NOT FITTED. The canopy's footprint is a closed form — `pi * a * b` of an ellipse
     * whose radii the emission writes — and the tower's is CloudClusterTowerFootprintRadii above. The gain
     * is the ratio of the two equivalent radii, floored at 1 because a canopy NARROWER than the tower it
     * caps is hidden inside it and costs the sky nothing.
     *
     * WHY THE WHOLE CLUSTER SHRINKS AND NOT THE CANOPY. `AnvilStrength` is authored as how far the canopy
     * spreads BEYOND its tower; shrinking the canopy alone would make that number mean something else. A
     * factor on the cluster leaves the storm's silhouette exactly as its asset describes it and moves only
     * how much sky one storm is worth.
     */
    float CloudClusterFootprintGain( const Graphic::CloudTypeShape& shape );

    /**
     * @brief Do these two sets of parameters bake the same volume?
     *
     * WHAT IT IS FOR. The renderer keeps a baked volume and has to know when it is stale. If this answers
     * "same" for two sets that bake differently, the artist moves a slider and NOTHING HAPPENS — which is
     * the dead setting §1.3 of the contract forbids, arrived at from the far side: the setting is wired
     * all the way through and the cache is what eats it. If it answers "different" for two sets that bake
     * identically, every frame re-bakes and the editor stalls for seconds at a time.
     *
     * WRITTEN OUT FIELD BY FIELD RATHER THAN `operator==`, and that is the safe direction: a defaulted
     * comparison would silently start comparing any field somebody adds, which sounds right until the
     * added field is one the bake does not read — and then every frame re-bakes. Comparing the fields the
     * bake actually reads means a new one has to be CONSIDERED rather than inherited.
     *
     * AND IT LIVES HERE, BESIDE THE BAKE, RATHER THAN IN THE RENDERER, because that is what makes the
     * paragraph above checkable. It was in the renderer's own translation unit, where nothing links, and a
     * deliberate sabotage — dropping one of the four placement numbers from the comparison — stayed GREEN
     * across the whole suite. Desert/Tests/Engine/CloudPlacementSpectrum now moves every field of the
     * struct in turn and demands this function notice.
     */
    bool CloudProceduralParamsEqual( const CloudProceduralFieldParams& a, const CloudProceduralFieldParams& b );

    /**
     * @brief Where the region's corner sits for a camera at @p cameraXKm, @p cameraZKm — SNAPPED.
     *
     * The snap is to the coarsest species' lattice cell, so that a cell inside the region before a shift is
     * the same cell after it and hashes to the identical lump. Returned as a pure function of the camera
     * and the parameters so that the renderer, the bake and the test all answer the question once.
     *
     * @return the MINIMUM corner of the region in world kilometres. The region spans
     *         [origin, origin + RegionSizeKm] on both horizontal axes.
     */
    glm::vec2 CloudProceduralRegionOriginKm( const CloudProceduralFieldParams& params, float cameraXKm,
                                             float cameraZKm );

    /// The snap step the function above quantises to, kilometres — the coarsest cell in the layer. Exposed
    /// because the test asserts the invariance across exactly one step of it, and a test that computed its
    /// own step would be testing its own arithmetic.
    float CloudProceduralSnapKm( const CloudProceduralFieldParams& params );

    /**
     * @brief The lumps one species puts in one region. PURE — the same inputs always give the same lumps,
     *        in the same order, byte for byte.
     *
     * Centres are in WORLD kilometres with y an absolute altitude, not relative to the region, because a
     * lump's identity is its place in the world and the region is only the window it is baked through.
     *
     * WHAT DECIDES A LUMP. Every field of the species' shape, and each of them is named here because a
     * stored number nobody reads is the dead data this contract forbids:
     *
     *   Base/Top Altitude      the band the stack of lumps spans
     *   Edge Top Fraction      how short the SHALLOWEST cluster is against the fullest one
     *   Base Ramp Fraction     the vertical radius of the lowest lump, as a fraction of the band
     *   Top Taper              how fast the horizontal radius shrinks going up the stack
     *   Anvil Altitude/Thickness/Strength   one extra, wider, flatter lump above the tower
     *   Placement Scale        already folded into CellKm by the caller
     *   Placement Anisotropy   already folded into Anisotropy by the caller
     *
     * and the four material numbers — Detail Character, Detail/Density/Extinction Factor — are NOT read
     * here: they travel to the march in the parameter block as they always did, because they describe what
     * the cloud is made of rather than where it is.
     *
     * @param slot which channel of the volume this species owns; it decorrelates the hash, so two species
     *        with identical shapes in different slots put their clusters in different places.
     */
    std::vector<CloudModellingBlob> GenerateCloudProceduralBlobs( const CloudProceduralFieldParams& params,
                                                                  uint32_t slot, const glm::vec2& regionOriginKm );

    /**
     * @brief Bakes the whole volume: one channel per species, RGBA8, the layout Graphic::Image3D uploads.
     *
     * ONE CHANNEL PER SPECIES and not one channel per Nubis quantity, and this is the one place where this
     * phase departs from the letter of variant C §3 point 2. That text gives the join's softmax weights to
     * Detail Type and Density Scale — which is right, and which is what the SCULPTED volume does — but it
     * was written before phase T3 put four kinds of cloud in one sky. Four channels can carry four species'
     * profiles or one species' four quantities, and the shipped state has four species with per-species
     * material numbers already reaching the march through the parameter block. Spending the channels on
     * quantities the march already has would have cost three of the four species. The divergence is
     * reported in the phase's report rather than hidden here.
     *
     * The softmax weights are not wasted: WITHIN a species they blend the lumps' own Detail Type and
     * Density Scale, and since a species' lumps share both, that blend is the identity — which is why
     * spending a channel on it would have bought nothing at all.
     *
     * @param params  validated by ValidateCloudProceduralParams; an invalid set is an error, never a
     *                silently substituted default.
     * @param regionOriginKm  the region's minimum corner, from CloudProceduralRegionOriginKm.
     * @return exactly `CloudProceduralVoxelBytes( params.VolumeSideVoxels )` bytes, or an error naming what
     *         was wrong.
     */
    Common::ResultStr<std::vector<unsigned char>>
    BakeCloudProceduralVolume( const CloudProceduralFieldParams& params, const glm::vec2& regionOriginKm );

    /**
     * @brief Told how far the bake has got, and asked whether to carry on.
     *
     * @param  fraction 0 at the start, 1 at the end, monotonically increasing.
     * @return false to abandon the bake, which then returns an error rather than a partial volume.
     *
     * THE SAME SHAPE THE SCULPTED BAKE ALREADY USES (CloudModellingBakeProgressFn), deliberately, because a
     * second spelling of "stop what you are doing" is a second mechanism for one thing.
     *
     * WHY THIS BAKE NEEDED IT, WHICH IS NOT THE REASON THE SCULPTED ONE DID. That one is started by a button
     * and cancelled by a button. This one is started by a SLIDER: an artist dragging Coverage lands twenty
     * edits in a second, and until O8/Г9 an already-doomed bake ran to completion because `std::async` has
     * no way to be told otherwise — the engine's own millisecond log measured 15.42 s from the last edit to
     * the sky that showed it, of which the first 4 800 ms was a volume nobody would ever see. Cancellation
     * is what turns "finish the stale one, then start the wanted one" into "start the wanted one".
     */
    using CloudProceduralBakeProgressFn = std::function<bool( float fraction )>;

    /**
     * @brief The same bake, reporting progress and able to be abandoned.
     *
     * PURITY IS UNCHANGED. @p onProgress may not influence the result: it is called between XZ slices, it is
     * handed a number, and nothing it does is read back into the arithmetic. Baking with a callback and
     * baking without one produce identical bytes, and Desert/Tests/Engine/CloudProceduralField asserts
     * exactly that — a "pure function with a progress hook" is otherwise a claim nobody has checked.
     *
     * The two-argument overload above is this one with an empty callback, so there is ONE bake and not two
     * that must be kept in step.
     */
    Common::ResultStr<std::vector<unsigned char>>
    BakeCloudProceduralVolume( const CloudProceduralFieldParams& params, const glm::vec2& regionOriginKm,
                               const CloudProceduralBakeProgressFn& onProgress );

    /// The DDC deriver of the modelling volume (UE's FCacheBucket + version). Bump the version whenever
    /// BakeCloudProceduralVolume's bytes change for the same inputs: the key cannot see the algorithm.
    inline constexpr Common::DDC::Deriver kCloudModellingDeriver{
         "CloudModelling", ".cmv", { 0x3c9d1f7a52e06b84ULL, 0x0000000000000001ULL } };

    /**
     * @brief Every input the bake reads, serialized in a fixed order — the settings block of the DDC key.
     *
     * THE SAME SET CloudProceduralParamsEqual COMPARES, AND IN THE SAME SENSE, because the two answer one
     * question ("would the bake produce different bytes?"): the paintings by content hash, the placement
     * only when a painting is bound, the shapes value by value (so -0 and +0 are one number). A field
     * the comparison sees and the key does not would serve yesterday's sky from the cache.
     */
    std::string SerializeCloudProceduralBakeInputs( const CloudProceduralFieldParams& params,
                                                    const glm::vec2&                  regionOriginKm );

    /// The DDC key of one modelling volume; @p deriver is a parameter so a test can prove the version counts.
    uint64_t CloudProceduralVolumeCacheKey( const CloudProceduralFieldParams& params, const glm::vec2& regionOriginKm,
                                            const Common::DDC::Deriver& deriver = kCloudModellingDeriver );

    struct CloudProceduralCachedBake
    {
        std::vector<unsigned char> Voxels;
        bool                       FromCache = false;
        uint64_t                   Key       = 0;
        /// Why a fresh bake could not be stored (the file system's own reason), empty when it was — the
        /// volume is still good, so the caller logs it rather than failing the sky.
        std::string CacheWriteError;
    };

    /**
     * @brief BakeCloudProceduralVolume behind the derived-data cache (the MeshDerivedData pattern).
     *
     * A hit returns the stored bytes without baking; an entry of the wrong size is an error naming its path,
     * never a silent re-bake. A miss bakes, then Puts.
     */
    Common::ResultStr<CloudProceduralCachedBake>
    BakeCloudProceduralVolumeCached( const CloudProceduralFieldParams& params, const glm::vec2& regionOriginKm,
                                     const CloudProceduralBakeProgressFn& onProgress );

    /**
     * @brief The Dimensional Profile at one point, gathered over @p blobs — 0 outside the body, 1 at
     *        ProfileDepth inside it.
     *
     * THE SAME THREE FUNCTIONS THE BAKE CALLS, in the same order: the distance, the join's shifted term,
     * the join. What differs is the SET — this gathers every lump it is handed where the bake gathers the
     * ones a spatial bin says can reach the voxel — and Desert/Tests/Engine/CloudProceduralField measures
     * the two against each other at four hundred probes and asserts they agree to within one 255th.
     *
     * It exists because the Cloud Type panel has to draw the silhouette a type produces, and a preview
     * computed from a formula written a second time is a preview that agrees with a picture nobody
     * renders. It is NOT what the bake uses: gathering every lump of a region at every one of two million
     * voxels is quadratic in the region, which is what the bin is for.
     *
     * @param blobs may be in any order; the join is commutative in real arithmetic and the list a caller
     *        gets from GenerateCloudProceduralBlobs is canonically sorted already.
     */
    float EvaluateCloudProceduralProfile( const CloudProceduralFieldParams&      params,
                                          const std::vector<CloudModellingBlob>& blobs, const glm::vec3& pointKm );

    /// How many lumps the whole region holds, summed over the species — the quantity the bake's cost is
    /// linear in, exposed so the renderer can log it beside the milliseconds rather than guessing.
    size_t CountCloudProceduralBlobs( const CloudProceduralFieldParams& params, const glm::vec2& regionOriginKm );

    /**
     * @brief How busy the sky is at one place — the ONE number a painting decides, 0..1.
     *
     * THIS IS THE FUNCTION THE BAKE CALLS, exposed rather than re-derived. The cloud layout panel draws a
     * map of what a painting will do to a sky, and a map computed from the rule written a second time is a
     * map that agrees with a sky nobody bakes. That is the same argument
     * EvaluateCloudProceduralProfile carries for the cloud TYPE panel's silhouette, and the same defect
     * class DEV_CONTRACT.md §2.3.1 names.
     *
     * WHAT IT FOLDS IN, in one place and once: the Coverage slider, then EITHER the painted pattern (when
     * a layout is bound and Layout Pattern Strength is up) OR the procedural weather patch — never both,
     * because two mechanisms setting one number is the second path §1.3 and §4.2 forbid — and then the
     * painted mask, which is additive and asymmetric because adding cloud where you paint is what a mask
     * is for. The result is CLAMPED to 0..1, and that clamp is not a formality: a mask at full strength
     * can drive a region past both ends of it, and everything the pattern would have said inside that
     * region is then eaten. CALIBRATION.md §PT measured exactly that as a byte-identical pair of frames.
     *
     * @param slot which species slot's channel of the painting to read; it also decorrelates the weather
     *        patch, so two species in different slots have their busy regions in different places.
     */
    float CloudProceduralCellCoverage( const CloudProceduralFieldParams& params, uint32_t slot,
                                       const glm::vec2& centreKm );

    /**
     * @brief A TOP-DOWN MAP of the coverage a painting produces, plus the two numbers that map cannot show
     *        on its own.
     *
     * WHY A MAP OF COVERAGE AND NOT A RENDERED SKY. An artist paints a sky, not a texture, so a preview
     * that shows the texture is honest and useless — CALIBRATION.md §PT's own proof is a TOP-DOWN frame
     * for exactly that reason. A rendered sky would mean a full bake and a march; this is the quantity in
     * between, and it is the quantity the painting actually decides. It is sampled on the PLACEMENT CELL
     * because that is the resolution the sky can express, which is what makes a stroke finer than a cell
     * visibly fall apart in the picture instead of looking crisp in it.
     *
     * @param slot    species slot to map, 0..Species.size()-1.
     * @param spanKm  world size of the square to map, centred on the world origin. One region span shows
     *                exactly one period of the painting; two shows that it tiles.
     * @param maxSide the caller's ceiling on the returned side, so a fine lattice over a wide span cannot
     *                ask for a million evaluations behind a slider.
     *
     * PURE: no GPU, no files, no global state. Errors rather than guesses when the slot does not exist or
     * the span is not a positive length.
     */
    struct CloudLayoutPreview
    {
        /// Cells across the mapped square, both axes.
        uint32_t Side = 0u;

        /// The square's world size, kilometres — the span asked for, unchanged.
        float SpanKm = 0.0f;

        /// The world size of one sample, kilometres. It is the placement cell unless @p maxSide clipped
        /// the map, and a caller that reports a legibility bound must use THIS rather than the cell it
        /// asked for, or it will quote a resolution the picture does not have.
        float SamplePitchKm = 0.0f;

        /// The MAPPED SLOT'S placement cell, kilometres, on its shorter side — the bound a painted stroke
        /// has to clear to read as a shape rather than as a row of clumps. The slot's own and not the
        /// layer's finest: legibility fails first for the species with the LARGEST cells, where
        /// ValidateCloudProceduralLayout's texel bound fails first for the species with the smallest, and
        /// two bounds that run in opposite directions must not share a number.
        float CellKm = 0.0f;

        /// `Side * Side` coverages in 0..1, row-major, x increasing east and y increasing north.
        std::vector<float> Coverage;

        /// How many of those cells the two ENDS of Layout Pattern Strength disagree about by more than one
        /// 255th — the quantity that is ZERO when the mask has saturated the clamp and the pattern slider
        /// can no longer do anything at all. Measured at 0 and at 1 rather than around the current value,
        /// because that is the pair CALIBRATION.md §PT shot and found byte-identical.
        uint32_t CellsPatternMoves = 0u;
        uint32_t Cells             = 0u;

        /// How many cells the clamp pinned at exactly 0 or exactly 1 at the parameters as given. It is the
        /// EXPLANATION for the number above: a sky most of which is pinned is a sky in which a
        /// redistribution has nowhere to go.
        uint32_t CellsClamped = 0u;

        /// The mean of Coverage over the map — what the Coverage slider actually delivers here, as opposed
        /// to what it asks for.
        float MeanCoverage = 0.0f;
    };

    Common::ResultStr<CloudLayoutPreview> BuildCloudLayoutPreview( const CloudProceduralFieldParams& params,
                                                                   uint32_t slot, float spanKm, uint32_t maxSide );
} // namespace Desert::Assets
