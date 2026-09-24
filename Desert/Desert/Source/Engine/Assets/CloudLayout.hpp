#pragma once

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace Desert::Assets
{
    /**
     * @file
     * @brief The PAINTED CLOUD LAYOUT (`.dclayout`): where the artist says clouds are, and where they are
     *        not, as data rather than as a hash.
     *
     * WHY IT EXISTS. The owner asked for two things in one breath — "a cloud material editor like Unreal's,
     * so I can load noise textures for the cloud shape" and "is there a parameter to do this by HAND". The
     * first is refused and stays refused (decision D-5: no material graph; our graph cannot compile into
     * the march loop, cannot sample 3D textures and cannot contain a loop). The second is this file. The
     * two are not the same request: Unreal's layout textures are DATA, and data is the one part of that
     * material we can take without taking the graph — the same formula this programme has already applied
     * three times (`.decloudtype`, `.dcnv`, `.dcmv`): Unreal's semantics, our formats.
     *
     * WHAT UNREAL DOES, AND WHICH HALF OF IT IS HERE. Docs/Clouds/RESEARCH_LAYOUT_TEXTURES.md is the full
     * account; the three facts this file is built on are:
     *
     *   1. `Layout_CloudGlobalPattern` — ONE CHANNEL PER CLOUD TYPE, each an independent two-dimensional
     *      field of "does this kind of cloud live here". Not a partition, not a set of blend weights: the
     *      channels overlap freely and the conflict is settled by a `max` at the very end. That is the
     *      arrangement our own four species slots already have, so the mapping is exact.
     *   2. `Layout_GlobalCloudMask` — regional ADD and REMOVE, and it is ADDITIVE: it is summed into the
     *      assembled shape ("Add additive mask" is the author's own node comment) and subtracts only by
     *      carrying a negative weight. There is no multiply and no remap anywhere on that path.
     *
     *      AND THE TWO ARE SEPARATE TEXTURE PARAMETERS AT EPIC, which is what decision O-4 brought over.
     *      They were one RGBA picture here until then: the pattern's four channels plus the mask needed
     *      five planes and an image has four, so the mask had to BE the fourth pattern channel — a plane
     *      with two meanings, and a layout that used all five was refused at the door
     *      (MakeCloudLayoutCanvasFromLayout used to say so). Below, the pattern and the mask are two
     *      tables with two pictures, imported and exported one at a time, and the FILE is unchanged: it
     *      always held both and always allowed either to be absent. The Cloud Raymarch material takes
     *      them as two inputs, so one painting can supply the pattern and a different one the mask.
     *   3. `Layout_CloudHeightProfile` is NOT here, and its absence is a decision with a bearing input.
     *      Unreal needs a `f(altitude, pattern value)` table because its placement field is two-dimensional
     *      and the table is the only vertical structure it has. Ours is geometry: a lump has three radii and
     *      an altitude, and `fill` in CloudProceduralVolume.cpp — how deep inside the coverage threshold a
     *      cell fell — already drives the stack's vertical band through the type's Edge Top Fraction. That
     *      IS `f(altitude, pattern value)`, expressed once. A painted table would be a SECOND way to state
     *      the same thing (contract §2.3.1) and would fight the §SIL2 calibration, where the lump's aspect
     *      and the erosion's strength are one quantity guarded by a test that names both numbers.
     *
     * THE PAYLOAD IS READ AT THE BAKE AND NEVER IN THE MARCH, and that is the whole cost argument. Unreal
     * samples its three layout textures on every material evaluation — once per march step and again at
     * seven further sites on the shadow rays (VolumetricCloud.usf:858, 991, 1012, 1092, 1106, 1181, 1317,
     * 1920, 2186) — because it has no precomputed field at all. We do: the field is a baked periodic volume
     * and the march performs ONE fetch of it. So the painted layer enters where placement is already
     * decided, once per LATTICE CELL, and adds not one instruction to the hottest pass of the frame.
     *
     * THE ONE REAL CONSTRAINT IS PERIODICITY, and it is why the placement below carries an integer rather
     * than a length. The modelling volume must be exactly periodic over the region: everything past the
     * region is REPEAT sampling of it, and the wrap seam is measured at 0.950/255 against 1.239/255 between
     * ordinary neighbours (CALIBRATION.md §RW). A world-anchored painting whose period did not divide the
     * region would put a hard discontinuity across every region face — a defect an order of magnitude
     * larger than the seam that exists. Hence `RepeatsPerRegion`, a whole number: divisibility is a
     * property of the TYPE and not something somebody has to remember to check.
     */

    /// How many channels a pattern table carries, and it is not a preference: a channel is one SPECIES
    /// SLOT of the layer, and a layer holds four (Graphic::kCloudSpeciesSlots, CLOUD_SPECIES_SLOTS in
    /// Common/CloudField.glslh). Unreal fixes the same four to R, G, B and A.
    ///
    /// NOTE WHAT A CHANNEL MEANS HERE AND WHAT IT MEANS AT EPIC. Theirs is a NAMED GENUS — R stratocumulus,
    /// G altostratus, B cirrostratus, A nimbostratus — because their four types are compiled in for ever.
    /// Ours is the layer's SLOT, whatever type the artist dropped into it, and the library on disk is not
    /// limited by this number at all. That is the more general arrangement and it costs nothing.
    inline constexpr uint32_t kCloudLayoutChannels = 4u;

    /// Bounds on the table's side. Below the floor a painting cannot describe a cell of the placement
    /// lattice at all; the ceiling is the memory decision D-9 grants the subsystem — 2048 squared would be
    /// 16.0 MiB of pattern, which is twice the modelling volume it feeds.
    inline constexpr uint32_t kCloudLayoutMinResolution = 4u;
    inline constexpr uint32_t kCloudLayoutMaxResolution = 1024u;

    /**
     * @brief A decoded layout: up to two tables, the means the bake needs, and the number the staleness
     *        check compares.
     *
     * EITHER TABLE MAY BE ABSENT, and absent is spelt as an EMPTY VECTOR rather than as a flag. One
     * representation of "there is no mask", so a mask that is present-but-empty cannot exist and no reader
     * has to decide which of two fields to believe.
     */
    struct CloudLayoutData
    {
        /// Side of both tables in texels. Square, because the layout tiles the world on a square period and
        /// a non-square table would stretch the painting by an aspect nobody authored.
        uint32_t Resolution = 0u;

        /// RGBA8, `4 * Resolution * Resolution` bytes, x fastest. Channel k is species slot k's own field
        /// of "is there cloud of this kind here", 0 = none, 255 = as much as the slider allows. Empty when
        /// the layout carries no pattern.
        std::vector<unsigned char> Pattern;

        /// R8, `Resolution * Resolution` bytes. 128 is NEUTRAL, above it adds cloud and below it removes —
        /// the signed convention Unreal expresses with a negative weight instead, chosen here because a
        /// painter's mid-grey is the value an artist can actually flood-fill. Empty when there is no mask.
        std::vector<unsigned char> Mask;

        /// The mean of each pattern channel over the whole table, 0..1, computed ONCE when the file is
        /// encoded and carried in the header.
        ///
        /// THIS IS THE FIELD THAT KEEPS DECISION D-20 TRUE, and it is worth stating why a mean is stored at
        /// all rather than recomputed. The pattern modulates a cell's coverage about the slider, and it is
        /// applied ZERO-MEAN so that binding a layout REDISTRIBUTES cloud instead of adding it — exactly
        /// the discipline the procedural patch field it replaces already keeps ("symmetric about the
        /// slider", CloudProceduralVolume.cpp). Without it, a bright painting would raise the sky's cover
        /// and the `Coverage` -> fraction-of-sky mapping that every shipped scene was re-authorised against
        /// would have to be measured again for every painting anybody ever makes.
        ///
        /// Stored rather than derived because the bake visits a few hundred CELLS and would otherwise sweep
        /// a million texels to learn one number, and because a mean computed twice is two numbers that can
        /// disagree.
        float PatternMean[kCloudLayoutChannels] = { 0.0f, 0.0f, 0.0f, 0.0f };

        /// CRC-32 of the payload, the same one the container verifies. It is what
        /// Assets::CloudProceduralFieldParams carries so that swapping the painting, or editing it in place,
        /// makes the cached volume stale — see the note on CloudProceduralFieldParams::LayoutContentHash for
        /// why the PARAMS carry a number and the BAKE takes the bytes.
        uint32_t ContentHash = 0u;

        /// The layout's identity, the GUID its envelope header states (container 2). Carried through every
        /// edit so that re-saving a painting keeps the handle scenes and materials reference it by; null
        /// only for a layout that has never been written, which the encoder then mints one for.
        Common::Content::AssetGuid Guid;

        bool HasPattern() const
        {
            return !Pattern.empty();
        }

        bool HasMask() const
        {
            return !Mask.empty();
        }

        /// True when this layout can change a single cell — i.e. when it carries at least one table. A
        /// decoded file always does; ValidateCloudLayoutData refuses one that carries neither, because a
        /// layout that cannot move anything is a slot an artist fills and never sees.
        bool IsUsable() const
        {
            return Resolution > 0u && ( HasPattern() || HasMask() );
        }
    };

    /**
     * @brief Where the painting sits in the world and how hard it pushes — the layer's half of the
     *        arrangement, as opposed to the file's half above.
     *
     * It is a plain value type and it lives in the bake's parameters, because every one of these five
     * numbers changes the baked volume and therefore has to make the cache stale.
     */
    struct CloudLayoutPlacement
    {
        /// How many times the painting repeats across the region, a WHOLE number.
        ///
        /// WHY AN INTEGER AND NOT A PERIOD IN KILOMETRES, which is how Unreal spells the same control
        /// (`Layout_CloudGlobalScale`, 256 km). Their layout has nothing to stay in step with; ours has to
        /// divide the region exactly or the volume stops being periodic and the far field grows a seam. A
        /// float period would make divisibility a thing somebody validates — and a validator that can be
        /// forgotten is the defect class §2.3.1 names. As an integer the relation cannot be expressed
        /// wrongly. At the shipped 48 km region, 1 gives a 48 km period and 16 gives 3 km, which is one
        /// lattice cell.
        uint32_t RepeatsPerRegion = 1u;

        /// Quarter turns of the painting about the world's vertical axis, 0..3.
        ///
        /// QUARTER TURNS AND NOT AN ANGLE, for the same reason and it is the same relation: a square
        /// lattice maps onto itself under a quarter turn and under nothing else, so a free angle breaks the
        /// periodicity that the integer above was chosen to protect. Unreal rotates freely
        /// (`Layout_GlobalTexturePlacement.a`) because it has no baked volume to keep in step. The
        /// divergence is deliberate and the bearing input is the measured wrap seam.
        uint32_t QuarterTurns = 0u;

        /// Where the painting's origin sits in the world, kilometres. Continuous, and it does NOT threaten
        /// the periodicity: sliding a periodic function along its own axis leaves it periodic.
        glm::vec2 OffsetKm{ 0.0f, 0.0f };

        /// How hard the painted pattern rules the coverage, 0..1.
        ///
        /// AT ZERO THE PROCEDURAL PATCH FIELD TAKES OVER, and that is not a fallback bolted on — it is the
        /// single-source rule. A cell's coverage has ONE modulator: the painting when there is one and it
        /// is turned up, the hash otherwise. Both applied at once would be two mechanisms deciding one
        /// number, which §1.3 and §4.2 of the contract forbid and which is exactly what the research found
        /// Unreal does not have to worry about, having no procedural patch at all.
        float PatternStrength = 1.0f;

        /// How hard the painted mask adds and removes, 0..1. Additive and ASYMMETRIC — that is its job, and
        /// it is Unreal's semantics unchanged. It is safe for D-20 in a way the pattern is not, because a
        /// layout with no mask table contributes exactly nothing and an artist who paints one is asking for
        /// the sky to change.
        float MaskStrength = 1.0f;
    };

    bool CloudLayoutPlacementEqual( const CloudLayoutPlacement& a, const CloudLayoutPlacement& b );

    /// Rejects a placement the bake cannot honour, naming the offending number. Pure, so the Cloud Layout
    /// panel greys out for the same reason the bake refuses rather than the two disagreeing.
    Common::BoolResultStr ValidateCloudLayoutPlacement( const CloudLayoutPlacement& placement );

    /// Rejects a decoded layout that cannot be sampled: no resolution, both tables absent, a table whose
    /// length disagrees with the resolution, a mean outside 0..1.
    Common::BoolResultStr ValidateCloudLayoutData( const CloudLayoutData& data );

    /**
     * @brief World kilometres to a position in the painting, in TABLE UNITS that wrap at 1.
     *
     * THE ONE RELATION THIS FUNCTION EXISTS TO KEEP: for any point, any offset, any rotation and any
     * repeat count, `CloudLayoutUv(p) == CloudLayoutUv(p + (regionSizeKm, 0))` and likewise on the other
     * axis, modulo 1. That is what makes the baked volume periodic with a painting bound, and
     * Desert/Tests/Engine/CloudPlacementSpectrum asserts it directly rather than trusting the argument
     * above — a quarter turn replaced by any other angle, or a fractional repeat count, turns it red.
     */
    glm::vec2 CloudLayoutUv( const CloudLayoutPlacement& placement, float regionSizeKm, const glm::vec2& worldKm );

    /**
     * @brief Bilinear, wrapping sample of one pattern channel, 0..1.
     *
     * WRAPPING AND NOT CLAMPED, because the painting tiles the world by construction and a clamped edge
     * would smear the last row of texels across everything beyond one period — the same mistake the
     * modelling volume's own note refuses for the same reason.
     *
     * @param slot species slot, 0..kCloudLayoutChannels-1. Out of range returns 0, which reads as "this
     *        kind of cloud is not painted here" and is the honest answer for a slot the painting does not
     *        describe.
     */
    float SampleCloudLayoutPattern( const CloudLayoutData& data, uint32_t slot, const glm::vec2& uv );

    /**
     * @brief Bilinear, wrapping sample of the mask, returned SIGNED in -1..1.
     *
     * The stored byte is 0..255 with 128 neutral, and this is where that convention is turned into the
     * number the coverage expression adds. One place performs the conversion, so a mask that reads as
     * inverted is a bug in one line rather than in every caller.
     */
    float SampleCloudLayoutMask( const CloudLayoutData& data, const glm::vec2& uv );

    /// The layout's own version, stated in the envelope header under kCloudLayoutSubsystemTag. Bumped when
    /// a FIELD of the payload moves, independently of anything about the meaning of the pixels. Version 1
    /// was a bare "DCLY" container with no header GUID; Tools/SceneMigrator wraps it into version 2.
    inline constexpr uint32_t kCloudLayoutContainerVersion = 2u;

    /// The subsystem tag the envelope header carries the layout version under.
    inline constexpr uint32_t kCloudLayoutSubsystemTag = Common::Content::FourCC( "DCLY" );

    /// Byte length of the fixed header that opens the envelope's Payload section, before the pixels.
    /// Exposed because the round-trip test asserts the whole payload length, and a header that grew
    /// without this moving would pass a test that meant nothing.
    inline constexpr size_t kCloudLayoutHeaderSize = 40u;

    /// The four bytes a version-1 file started with, before the envelope. Kept ONLY so the decoder can
    /// refuse such a file by name and Tools/SceneMigrator can recognise what it wraps; nothing reads it.
    inline constexpr char kCloudLayoutVersion1Magic[4] = { 'D', 'C', 'L', 'Y' };

    /// The extension the Content Browser, the file dialog and the drag-and-drop payload all agree on.
    inline constexpr const char* kCloudLayoutExtension = ".dclayout";

    /**
     * @brief Serialises a layout into its AF1 binary envelope (kind CloudLayout, the layout's GUID, one
     *        Payload section: the 40-byte header, then the tables).
     *
     * The GUID travels in `data.Guid`; a null one is a layout never written before and gets a fresh GUID
     * here, so an edit that carries its GUID through keeps the asset's handle.
     * The four channel means are RECOMPUTED here from the pattern rather than trusted from the argument,
     * which is the one place they can be made to agree with the pixels. A mean that travelled in from a
     * caller could describe a different painting than the one being written, and the symptom would be a
     * sky whose cover drifts from its slider for no reason anybody could see.
     */
    Common::ResultStr<std::vector<unsigned char>> EncodeCloudLayout( const CloudLayoutData& data );

    /**
     * @brief Parses a container back into a layout, or says why it could not.
     *
     * REFUSES RATHER THAN GUESSES, each refusal naming the number that was wrong: a bare version-1 file
     * (named, pointing at Tools/SceneMigrator), an envelope that does not read or is not a CloudLayout, an
     * unknown container version, a resolution outside its bounds, a payload length that disagrees with the
     * resolution, a truncated file, a checksum that does not match. A layout that decoded to zeros would
     * render as a sky with no cloud in it and nothing in the log — which is the shape of defect this
     * programme keeps paying for.
     */
    Common::ResultStr<CloudLayoutData> DecodeCloudLayout( const std::vector<unsigned char>& bytes );

    /**
     * @brief How wide the THINNEST parts of one painted channel are, in texels.
     *
     * WHY THIS EXISTS, AND IT IS A HOLE A VALIDATOR STRUCTURALLY CANNOT COVER.
     * `ValidateCloudProceduralLayout` compares one layout TEXEL against the placement cell, and that is
     * the right bound for "can the painting tell two neighbouring cells apart". It is NOT the bound for
     * LEGIBILITY, which is the painting's thinnest STROKE against the cell: at four repeats the letter of
     * CALIBRATION.md §PT has a 12 km world period and a 1.2 km stroke, so every texel still resolves a
     * cell and the glyph still collapses into evenly spaced clumps. The knob's ceiling of sixteen is
     * honest about periods and silent about pictures, and §PT reported that silence rather than guarding
     * it, because guessing a stroke width is an opinion about somebody's painting.
     *
     * SO IT IS MEASURED RATHER THAN GUESSED. A stroke width is a property of the pixels and nothing else,
     * and once measured it can be REPORTED — which is what the authoring panel does with it. Nothing here
     * refuses anything; the bake is untouched.
     *
     * HOW A TEXEL'S STROKE WIDTH IS DEFINED. A texel counts as painted when it is above the midpoint
     * between the channel's own least and greatest value — its own midpoint rather than a fixed 0.5, so a
     * soft painting that never reaches white is measured on the shape the artist drew rather than on the
     * ink they used. Its stroke width is the SHORTER of the two runs of painted texels it lies in,
     * horizontal and vertical. That is exact for a straight bar of any orientation-aligned width and
     * generous for a diagonal one (a diagonal bar's axis-aligned runs are longer than its true width by
     * up to sqrt(2)), which is the safe direction: it never claims a stroke is thinner than it is.
     *
     * THE RUNS WRAP, because the painting tiles the world. A band that leaves the right edge and returns
     * at the left is one stroke and not two, and measuring it as two would report half its real width.
     */
    struct CloudLayoutStrokeStats
    {
        /// How many texels of this channel counted as painted. ZERO means the channel is flat — nothing
        /// was drawn on it — and every field below is then zero as well, which a caller must read as "no
        /// strokes" rather than as "infinitely thin ones".
        uint64_t PaintedTexels = 0u;

        /// The width at or below which the thinnest TENTH of the painted texels lie, in texels. Reported
        /// as a percentile rather than as a minimum because a minimum is the corner of one curve: any
        /// rounded shape has a few texels one wide, and a figure would be judged by its worst four pixels.
        float ThinnestTenthTexels = 0.0f;

        /// The median painted texel's stroke width, in texels — what the bulk of the drawing measures.
        float MedianTexels = 0.0f;

        /// Fraction of the painted texels whose stroke is narrower than the limit handed to the measure,
        /// 0..1. This is the number an artist acts on: "this much of what you drew is finer than the sky
        /// can express".
        float FractionBelowLimit = 0.0f;
    };

    /**
     * @brief Measures one pattern channel's strokes, and how much of it falls under @p limitTexels.
     *
     * @param slot species slot, 0..kCloudLayoutChannels-1. A layout with no pattern, or a slot out of
     *        range, measures as an empty result — no strokes, rather than an error, because "you have not
     *        painted this slot" is an ordinary state and not a fault.
     * @param limitTexels the width the caller cares about, in texels of THIS table — normally the
     *        placement cell converted through the painting's own world period. Non-positive means nothing
     *        is below it, and FractionBelowLimit is then 0.
     *
     * PURE and allocation-bounded: two byte-wide run tables and a histogram the side of the image.
     */
    CloudLayoutStrokeStats MeasureCloudLayoutStrokes( const CloudLayoutData& data, uint32_t slot,
                                                      float limitTexels );

    // ---------------------------------------------------------------------------------------------------
    // THE CANVAS — the authoring surface both ways in converge on, and the reason it is HERE
    // ---------------------------------------------------------------------------------------------------
    //
    // WHAT THE BRUSH PRODUCES IS PIXELS, not a layout. Everything below fills the canvas's two planes, and
    // the panel and the tool then hand that canvas to `MakeCloudLayoutFromCanvas`. So there is exactly ONE
    // statement in this tree of what a picture means, and painting and importing cannot come to differ
    // about a channel mapping, a mask or a mean.
    //
    // AND IT LIVES IN THE ENGINE RATHER THAN IN THE PANEL because a brush the editor owns is a brush no
    // test can run and no command can reproduce. The panel drives it from a mouse; the command-line baker
    // drives the identical function from a figure; and Desert/Tests/Engine/CloudPlacementSpectrum measures
    // its strokes with the very ruler the panel quotes at the artist. A figure whose provenance is
    // "somebody clicked" is what §PT already refused once.

    /**
     * @brief One painting surface: the pattern's four planes and the mask's one, as two buffers.
     *
     * TWO BUFFERS AND NOT ONE RGBA IMAGE, and this is decision O-4. The pattern needs four planes and the
     * mask a fifth; an RGBA image has four, so the mask used to BE the pattern's alpha and a flag said
     * which of the two that plane meant. That cost three things and all three are gone with it: a layout
     * using all five planes could not be opened for painting at all, alpha had two meanings the artist had
     * to keep straight, and the two tables could not be brought in or taken out separately — which is how
     * Unreal has had them all along, as two texture parameters.
     *
     * THE MASK IS ABSENT AS AN EMPTY VECTOR, exactly as in CloudLayoutData, rather than as a flag beside a
     * buffer of neutral bytes. One representation of "there is no mask", so no reader has to decide which
     * of two fields to believe.
     */
    struct CloudLayoutCanvas
    {
        /// Side in texels, shared by both planes. Square, for the reason CloudLayoutData::Resolution is.
        uint32_t Side = 0u;

        /// RGBA8, `4 * Side * Side`, x fastest — channel k is species slot k. This is the exact buffer
        /// stbi_load returns and stbi_write_png takes, so the pattern needs no conversion in either
        /// direction. EMPTY when this painting carries no pattern, which is what a mask brought in on its
        /// own leaves and what a mask-only `.dclayout` opens as.
        std::vector<unsigned char> Pattern;

        /// R8, `Side * Side`. 128 is neutral, above adds cloud and below removes it. EMPTY when this
        /// painting carries no mask, which is the state a blank canvas starts in.
        std::vector<unsigned char> Mask;

        /// Same authored state? DEFAULTED so the compiler generates it from every member — see the long
        /// note at Graphic::CloudVerticalProfile::operator==. Read by the cloud documents' GetDiskState.
        [[nodiscard]] bool operator==( const CloudLayoutCanvas& ) const = default;
        bool HasMask() const
        {
            return !Mask.empty();
        }
    };

    /**
     * @brief A blank canvas: no cloud painted anywhere, and NO MASK.
     *
     * WHY NO MASK RATHER THAN A NEUTRAL ONE. A mask of uniform neutral changes nothing, so carrying one
     * would be a table written to every file for no effect — and the layer would pay a wrap-sampled fetch
     * per cell to add zero. Absent is the honest state, and @ref SetCloudLayoutCanvasMask is how an artist
     * asks for one; it starts at neutral, so ticking the box never fills the sky by itself.
     */
    Common::ResultStr<CloudLayoutCanvas> MakeCloudLayoutCanvas( uint32_t side );

    /**
     * @brief Recovers the canvas a layout was painted on.
     *
     * IT CANNOT REFUSE FOR THE OLD REASON ANY MORE, and that is the point of O-4. This function used to
     * reject any layout whose mask differed from its fourth pattern channel — five planes will not fit in
     * an RGBA image — so a `.dclayout` with a pattern on all four slots AND a mask could be bound and
     * rendered but never opened. With the mask on its own plane there is nothing left to collide, and the
     * only failure is a layout that was not valid to begin with.
     */
    Common::ResultStr<CloudLayoutCanvas> MakeCloudLayoutCanvasFromLayout( const CloudLayoutData& data );

    /**
     * @brief The canvas as a layout: the file's tables, its means and its content hash.
     *
     * ROUND-TRIPPED THROUGH THE CONTAINER rather than assembled by hand, so the means and the hash a
     * caller receives are the ones the FILE will carry. Two paths to a CloudLayoutData — one through the
     * encoder and one around it — is how the mean in memory comes to differ from the mean on disk, and the
     * symptom is a sky whose cover drifts from its slider with nothing anywhere to say why.
     *
     * A canvas with a flat pattern and no mask is still a layout, and so is one with a mask and no
     * pattern; a canvas of side 0 is not, and neither is one carrying neither table — the second of those
     * is refused inside the encoder, where that rule already lives.
     */
    Common::ResultStr<CloudLayoutData> MakeCloudLayoutFromCanvas( const CloudLayoutCanvas& canvas );

    /**
     * @brief Gives the canvas a mask, at neutral, or takes its mask away.
     *
     * Adding one twice keeps what is painted — it is not a clear — because the artist ticking a box they
     * had already ticked must not lose a drawing. Removing it drops the plane, which is what makes the
     * saved file carry no mask table at all.
     */
    Common::BoolResultStr SetCloudLayoutCanvasMask( CloudLayoutCanvas& canvas, bool present );

    /// One square 8-bit picture: what a table looks like outside this engine, and the only shape either
    /// direction of the import/export door deals in. RGBA8 because that is what `stbi_load(..., 4)` returns
    /// and `stbi_write_png` takes, so neither end needs a conversion pass.
    struct CloudLayoutImage
    {
        uint32_t                   Side = 0u;
        std::vector<unsigned char> Pixels; ///< `4 * Side * Side`, x fastest
    };

    /**
     * @brief Brings a PATTERN picture in — Unreal's `Layout_CloudGlobalPattern` slot, as a file.
     *
     * @param channelForSlot which SOURCE channel feeds each species slot, 0..3 each. It exists because a
     *                       painting is usually greyscale: an artist draws one shape and wants it on slot
     *                       1, and without this they would have to author an RGBA image to say so.
     *
     * Non-square and out-of-range sources are REFUSED by name rather than resampled: resampling is an
     * opinion about the artist's painting, and one taken silently is the worst kind. A picture whose side
     * disagrees with a mask ALREADY on this canvas is refused too, with both numbers — the two tables of
     * one layout share a resolution, and quietly resizing either would change a sky nobody asked to change.
     */
    Common::BoolResultStr
    SetCloudLayoutCanvasPatternFromImage( CloudLayoutCanvas& canvas, const std::vector<unsigned char>& pixels,
                                          uint32_t width, uint32_t height,
                                          const uint32_t channelForSlot[kCloudLayoutChannels] );

    /**
     * @brief Brings a MASK picture in — Unreal's `Layout_GlobalCloudMask` slot, as a file.
     *
     * @param sourceChannel which channel of the picture carries the mask, 0..3. A greyscale PNG loads with
     *                      R == G == B, so 0 is right for everything an artist is likely to draw; the
     *                      parameter exists so a mask packed into somebody's alpha is not a re-export.
     *
     * THE PICTURE'S BYTES ARE THE MASK'S BYTES, 128 neutral, unchanged. There is no remap and no inversion
     * here, so what an artist flood-fills with mid-grey is exactly the value that does nothing.
     */
    Common::BoolResultStr SetCloudLayoutCanvasMaskFromImage( CloudLayoutCanvas&                canvas,
                                                             const std::vector<unsigned char>& pixels,
                                                             uint32_t width, uint32_t height,
                                                             uint32_t sourceChannel );

    /// The pattern's four planes as an RGBA picture, ready for `stbi_write_png`. Slot k is channel k, so
    /// what comes out is what @ref SetCloudLayoutCanvasPatternFromImage takes back in under the identity
    /// mapping — the round trip a test can assert rather than a resemblance.
    Common::ResultStr<CloudLayoutImage> EncodeCloudLayoutCanvasPatternToImage( const CloudLayoutCanvas& canvas );

    /// The mask as an RGBA picture: its byte in R, G and B so it opens as the grey an artist painted, and
    /// an opaque alpha so no tool treats the dark half of it as transparency. Refuses a canvas with no
    /// mask, because writing a neutral file would claim a table this painting does not have.
    Common::ResultStr<CloudLayoutImage> EncodeCloudLayoutCanvasMaskToImage( const CloudLayoutCanvas& canvas );

    /**
     * @brief The brush itself: three numbers, each of which changes the pixels.
     *
     * THERE IS NO "FLOW" AND ITS ABSENCE IS A DECISION. A flow that builds up over repeated dabs makes a
     * stroke's width depend on how SLOWLY the mouse moved through it, and the one thing this brush has to
     * guarantee is that a stroke is as wide as it was asked to be — that is the number the placement cell
     * is compared against. Ink already expresses everything a partial flow could: a stroke that deposits
     * less cloud. So the stroke is laid ONCE, at its strongest, and its width is a property of the brush
     * rather than of the drag.
     */
    struct CloudLayoutBrush
    {
        /// Half the stroke's width at full hardness, in texels of the canvas.
        float RadiusTexels = 24.0f;

        /// 0..1. At 1 the dab is a disc with no ramp at all; at 0 it ramps linearly from the centre to
        /// nothing at the rim. It is what makes a coastline soft and a letter crisp, and it moves the width
        /// the stroke MEASURES at — see CloudLayoutBrushWidthTexels, which states that relation once.
        float Hardness = 1.0f;

        /// 0..1, the value the stroke moves the channel TOWARD. On a pattern channel 1 is "as much of this
        /// species as the slider allows" and 0 is "none"; on the mask 1 adds cloud, 0 removes it and 0.5 is
        /// the neutral an eraser returns to. One number for painting and for erasing, because an eraser
        /// that is not just "ink at the rest value" is a second code path doing one thing.
        float Ink = 1.0f;
    };

    /**
     * @brief How wide a stroke of @p brush comes out under `MeasureCloudLayoutStrokes`, in texels.
     *
     * THIS IS THE RELATION THE WHOLE PANEL RESTS ON, so it is stated once here rather than assumed in two
     * places. The measure calls a texel painted when it is above the channel's own midpoint; the dab's
     * profile is flat out to `Hardness * Radius` and ramps linearly to nothing at `Radius`; so the profile
     * crosses the midpoint at `Radius * (1 + Hardness) / 2` from the axis and the stroke measures
     * `Radius * (1 + Hardness)` across. At full hardness that is exactly the diameter, which is why the
     * panel's advice — "paint at least one placement cell wide" — is a statement about the radius slider
     * and not a hope about it.
     *
     * Independent of Ink on purpose: a fainter stroke is not a thinner one, and the measure's own
     * per-channel midpoint is what makes that true.
     */
    float CloudLayoutBrushWidthTexels( const CloudLayoutBrush& brush );

    /**
     * @brief One continuous drag, held open between mouse-down and mouse-up.
     *
     * WHY A STROKE HAS STATE AT ALL. A texel takes the STRONGEST coverage the stroke laid on it, once —
     * not one deposit per frame. Without that, a drag that paused would burn through a soft edge, the same
     * figure would come out different on a slow machine, and the width the panel promises would be a
     * function of the frame rate. The two tables below are what make "laid once" true: @ref Base is the
     * channel as it stood when the drag began, and @ref Coverage is the strongest dab seen so far.
     *
     * It carries the side and the channel so that they are validated ONCE, at BeginCloudLayoutStroke, and
     * the per-segment call that runs on every mouse move cannot fail at all.
     */
    struct CloudLayoutStroke
    {
        uint32_t Side    = 0u;
        uint32_t Channel = 0u;

        /// How many channels the buffer being painted interleaves — 4 for the canvas's pattern, 1 for its
        /// mask. Carried on the stroke so that the per-segment call, which runs on every mouse move, takes
        /// the same stride the opening call validated. A stride passed twice is a stride that can be
        /// passed differently the second time, and the symptom would be a brush writing every fourth texel.
        uint32_t ChannelCount = 0u;

        /// `Side * Side`, the channel's value before this drag.
        std::vector<unsigned char> Base;

        /// `Side * Side`, the strongest dab alpha this drag has laid, 0..255.
        std::vector<unsigned char> Coverage;

        bool IsOpen() const
        {
            return Side > 0u && ChannelCount > 0u && Base.size() == static_cast<size_t>( Side ) * Side;
        }
    };

    /**
     * @brief Opens a stroke on one channel of an interleaved plane buffer, snapshotting it.
     *
     * @param plane        the canvas's Pattern (4 channels) or its Mask (1). Passed as bytes rather than as
     *                     the canvas so that a test — and the stroke measure beside it — can paint a buffer
     *                     without building an asset around it.
     * @param channel      which channel of @p plane, `0 .. channelCount - 1`.
     * @param channelCount how many channels @p plane interleaves.
     *
     * Refuses a buffer whose length disagrees with its side and stride, a side outside the layout's bounds
     * and a channel the stride does not have — each by name, because the alternative is a brush that paints
     * into the wrong plane and an artist who cannot see why.
     */
    Common::BoolResultStr BeginCloudLayoutStroke( CloudLayoutStroke&                stroke,
                                                  const std::vector<unsigned char>& plane, uint32_t side,
                                                  uint32_t channel, uint32_t channelCount );

    /**
     * @brief Extends an open stroke by one straight segment and writes the result into @p canvas.
     *
     * THE SEGMENT IS A CAPSULE, NOT A ROW OF DABS. Every texel's coverage comes from its distance to the
     * SEGMENT rather than to the nearest of a series of stamps, so there is no spacing constant, no
     * scalloped edge, and no way for a fast mouse to leave gaps a slow one would not. It also removes the
     * one number a dab-spacing brush has that nobody can choose correctly.
     *
     * AND THE FOOTPRINT WRAPS. The painting tiles the world, so a stroke laid against the left edge must
     * come out of the right one — otherwise every figure that touches an edge is cut in half at the region
     * face, which is the one discontinuity the whole `.dclayout` design exists to avoid.
     *
     * @return how many texels changed value. Zero means there is nothing to re-upload, which is what keeps
     *         a stationary cursor from restaging a megabyte every frame.
     */
    uint64_t ExtendCloudLayoutStroke( CloudLayoutStroke& stroke, std::vector<unsigned char>& plane,
                                      const glm::vec2& fromTexels, const glm::vec2& toTexels,
                                      const CloudLayoutBrush& brush );

    /**
     * @brief Paints a whole polyline as ONE stroke — a drag, replayed from a script.
     *
     * It is what `Tools/CloudLayoutBaker` draws its brush figures with, and it exists so that a frame
     * showing "a letter painted with the brush" is showing THIS brush rather than a second one that
     * resembles it. Fewer than two points paint nothing and say so.
     */
    Common::BoolResultStr PaintCloudLayoutPolyline( std::vector<unsigned char>& plane, uint32_t side,
                                                    uint32_t channel, uint32_t channelCount,
                                                    const std::vector<glm::vec2>& pointsTexels,
                                                    const CloudLayoutBrush&       brush );

    /**
     * @brief Floods one channel of a plane — the "start again" of the panel and the ground a figure is
     *        drawn on.
     */
    Common::BoolResultStr FillCloudLayoutPlaneChannel( std::vector<unsigned char>& plane, uint32_t side,
                                                       uint32_t channel, uint32_t channelCount,
                                                       unsigned char value );

    /// The mask's neutral byte, exposed because the panel has to offer it as an eraser's ink and the tool
    /// has to lay it down as a background. One constant, so "neutral" cannot be spelt 127 in one place and
    /// 128 in another.
    inline constexpr unsigned char kCloudLayoutMaskNeutral = 128u;
} // namespace Desert::Assets
