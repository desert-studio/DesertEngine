#pragma once

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <functional>
#include <istream>
#include <vector>

namespace Desert::Assets
{
    /**
     * @file
     * @brief The cloud MODELLING volume: the recipe a hero cloud's body is sculpted from, the container it
     *        is stored in (`.dcmv`), and the pure functions that turn one into the other.
     *
     * WHAT IT IS FOR. Docs/Clouds/ANALYSIS_APPROACH.md §4 gives the cloud field two producers behind one
     * seam. The procedural one is infinite and needs no content, and it has a measured limit that three
     * separate tasks found independently: the Alligator's lobes DO NOT MERGE by construction — the field
     * is `best - second`, which lays a zero between every pair of cells — so a procedural sky reads as a
     * deck of separate cushions and can never be one fused convective mass. This volume is the other
     * producer, and a fused mass is exactly what it is for.
     *
     * WHY A SEPARATE FORMAT AND NOT MORE OF `.dcnv`, since a `.dcnv` is also four channels of RGBA8 in a
     * little-endian container. Four reasons, and each of them is a field that would have had to become
     * "meaningful for one kind of volume only":
     *
     *   1. `.dcnv`'s header IS a noise recipe — a seed, four lattice periods, a curl strength. A sculpted
     *      body has none of them, and a struct whose fields mean something for half its files is the
     *      shape of defect this programme has paid for most often.
     *   2. `.dcnv` is CUBIC by construction: one `Resolution` scalar, validated as a power of two, with
     *      the payload length checked as `R^3`. This volume is 128 x 64 x 128 — three independent extents,
     *      because a cloud is wider than it is tall and a cubic volume spends half its bytes on air.
     *   3. The channel meanings are stored and CHECKED on read in both formats, and they are disjoint
     *      sets. Two magics refuse each other's files by name; one magic could only refuse them by
     *      guessing from a field further in.
     *   4. `.dcnv`'s validator enforces "at least eight voxels per lattice cell", which is a statement
     *      about noise and is meaningless about a sculpted body.
     *
     * IT IS THE SAME ASSET SYSTEM ALL THE SAME (contract §2.2): `AssetTypeID::CloudModellingVolume`,
     * `AssetBase`, `AssetManager`, the Content Browser, the drag-and-drop payload and the hot reload are
     * the ones every other asset uses, and the byte primitives are the ones `.dcnv` uses
     * (Engine/Assets/ContainerBytes.hpp). What is new is a format, not a second way to own files.
     *
     * WHY NOT DDS/KTX/BCn — unchanged from `.dcnv` and repeated because it is the first question asked:
     * there is not one of them anywhere in this tree, `ImageFormat` knows RGBA8F / RGBA16F / RGBA32F /
     * BGRA8F and the two depth formats, and on Apple Silicon the BC family is not exposed for 3D textures
     * at all (ANALYSIS_APPROACH.md §2.2).
     *
     * THE FILE CARRIES ITS OWN RECIPE, exactly as a `.dcnv` does, and for the same reason: the blobs, the
     * blend radius and the two normalisation distances are all in the header, so any volume can be
     * re-baked, compared or edited from what is written in it. That is also what makes the sculpting tool
     * of phase A1 an editor for this struct rather than a new file format.
     */

    /// What a channel of a modelling volume CONTAINS. Stored per channel in the header, so a volume is
    /// self-describing and a mislabelled or re-ordered one is a diagnosable error rather than a cloud that
    /// simply looks wrong.
    ///
    /// The first three are the deck's own set and its own order (Nubis Cubed, SIGGRAPH 2023, p.85 and
    /// p.96: "Dimensional Profile / Detail Type / Density Scale"). The fourth is ours and is named on the
    /// enumerator.
    enum class CloudModellingChannel : uint32_t
    {
        DimensionalProfile = 0, ///< how deep inside the body, 0 at the surface and 1 in the core
        DetailType         = 1, ///< 0 wispy, 1 billowy — which erosion the up-rez applies here
        DensityScale       = 2, ///< per-voxel multiplier on how much matter is in this part of the body
        /// The body DILATED by the bake's margin: 1 throughout the body, falling to 0 a stated distance
        /// outside it. It is the conservative shell — above zero wherever the profile is, and extending
        /// past it, never the other way round — and the CUTOUT is what reads it: an instance that
        /// suppresses the procedural field suppresses it here and only here, so a hero cloud does not cut
        /// a rectangular hole in the deck around it. See Common/CloudField.glslh.
        CutoutEnvelope = 3,
    };

    const char* CloudModellingChannelName( CloudModellingChannel channel );

    /**
     * @brief WHICH SOLID a lump is. Three, and the set is closed until something measures a fourth.
     *
     * A0 shipped one — the ellipsoid — on the reasoning that three radii buy the whole vocabulary for two
     * floats. That reasoning is still right about SHAPE and turned out to be incomplete about two other
     * things, which is why this enum exists rather than a fourth radius:
     *
     *   * A CAPSULE IS NOT AN ELLIPSOID AND CANNOT BE APPROXIMATED BY ONE. Its surface is a swept sphere,
     *     so its cross-section is CONSTANT along the segment; an ellipsoid's tapers from the middle to a
     *     point at both ends. The catalogue of ANALYSIS_APPROACH.md section 6 asks for the flat spreading
     *     BASE of a cumulus and for the elongated growths off its shoulders, and both are the constant
     *     cross-section case — sculpted from ellipsoids they come out lens-shaped and pinched, and the
     *     only fix is a row of overlapping ellipsoids, which costs one lump per unit of length.
     *   * A SPHERE IS AN ELLIPSOID, AND IT IS A CHEAPER ONE. Substituting equal radii into the ellipsoid's
     *     bounded form reduces it algebraically to `|p| - R` — but only after two vector divides, a second
     *     `length` and a division that the compiler cannot elide because it cannot know the radii are
     *     equal. Evaluating the reduced form directly is the same answer for less work, and it is EXACT
     *     rather than the ellipsoid form's tight underestimate. It earns its branch by measurement, not by
     *     vocabulary; the number is in Docs/Clouds/CALIBRATION.md section A1.
     *
     * The set is CLOSED here deliberately. Every entry is a branch in a loop that runs 8.4 million times
     * and a case in the format's validator, so a fourth is a cost that has to be argued for by naming a
     * shape in the catalogue that these three cannot express.
     */
    enum class CloudModellingPrimitive : uint32_t
    {
        /// Three independent semi-axes. The general case: a flattened disc, a prolate lobe, a lens.
        Ellipsoid = 0,
        /// One radius. `RadiiKm` must be equal on all three axes.
        Sphere = 1,
        /// A segment of the lump's local Y swept by a sphere. `RadiiKm.x` and `RadiiKm.z` are the
        /// cross-section radius and must be equal; `RadiiKm.y` is the TOTAL half-height, caps included,
        /// and must be at least the radius — a capsule shorter than its own diameter is a sphere, and is
        /// refused as one rather than silently degenerating into one.
        Capsule = 2,
    };

    const char* CloudModellingPrimitiveName( CloudModellingPrimitive primitive );

    /**
     * @brief One smooth lump the body is made of.
     *
     * `RadiiKm` MEANS THE SAME THING FOR EVERY PRIMITIVE: the half-extent of the lump's local bounding
     * box. Each primitive then constrains it — an ellipsoid not at all, a sphere to three equal numbers, a
     * capsule to a round cross-section — and ValidateCloudModellingRecipe enforces the constraint. That
     * uniformity is what lets the recipe's "the body must not reach its own box" arithmetic stay one
     * formula instead of three, and it is why no component of `RadiiKm` is ever ignored: a stored field
     * that nothing reads is the shape of dead data this contract forbids.
     *
     * The two material numbers are per-blob and NOT per-voxel-authored: the union below turns them into a
     * smooth per-voxel field for free (see GenerateCloudModellingVolume), which is the second of the three
     * properties that made the exponential smooth-min the right join.
     */
    struct CloudModellingBlob
    {
        /// Centre, kilometres, relative to the CENTRE of the volume's box. Relative rather than absolute
        /// so that a recipe is a description of a cloud rather than of a place — the entity's transform is
        /// what puts it in the sky.
        glm::vec3 CentreKm{ 0.0f };

        /// Half-extent of the lump's local bounding box, kilometres — read as semi-axes by an ellipsoid, as
        /// one radius by a sphere, as radius-and-half-height by a capsule. All three strictly positive; a
        /// zero axis is a degenerate solid whose distance function divides by zero, so it is refused rather
        /// than clamped.
        glm::vec3 RadiiKm{ 0.25f };

        /// The lump's orientation, DEGREES, applied about its own centre before the distance is measured.
        /// Euler angles in the engine's own convention — `glm::quat( glm::radians( RotationDeg ) )`, the
        /// same construction TransformComponent::GetTransform uses — so a lump's rotation reads the way
        /// every other rotation in the editor reads.
        ///
        /// DEGREES AND NOT RADIANS, and the unit is in the NAME: this is a number an artist types and a
        /// baker prints, not one that feeds a quaternion every frame, and a field whose unit is only
        /// stated in a comment is how two halves of a subsystem come to disagree.
        ///
        /// It is what makes the capsule useful rather than decorative. A capsule's axis is its local Y, so
        /// without a rotation every capsule is vertical — and the shape the catalogue actually asks a
        /// capsule for, the spreading base of a cumulus, is horizontal.
        ///
        /// A rotation is RIGID, so it moves the surface without distorting the distance to it: the field
        /// stays a true normalised distance field, which is the third of the three properties the join was
        /// chosen for and the one the Dimensional Profile is derived from.
        glm::vec3 RotationDeg{ 0.0f };

        /// WHICH SOLID. See CloudModellingPrimitive.
        CloudModellingPrimitive Primitive = CloudModellingPrimitive::Ellipsoid;

        /// How hard this lump pulls in the union, > 0. It multiplies the lump's term in the smooth
        /// minimum's sum, and the algebra of what that does is exact and worth stating, because it is what
        /// makes the knob predictable instead of a feel:
        ///
        ///   `-r * ln( SUM w_k exp(-d_k/r) )` is the same function as the unweighted join over distances
        ///   `d_k - r*ln(w_k)`.
        ///
        /// So A WEIGHT IS A DILATION OF THE LUMP, and its size is `BlendRadiusKm * ln(Weight)` — at the
        /// shipped 50 m blend radius, a weight of 2 grows the lump by 35 m and a weight of 0.5 shrinks it
        /// by the same. It moves the CREASE between neighbours without moving either centre, which is the
        /// thing an artist reaches for when one lobe should read as growing out of another rather than
        /// meeting it.
        ///
        /// 1 is the identity and reduces the sum to A0's exactly. The range is [1/8, 8], which is
        /// +/- 2.08 blend radii of dilation — past that the lump has stopped being where its centre says
        /// it is, and moving it is the honest edit.
        float Weight = 1.0f;

        /// 0 = wispy, 1 = billowy. The character of the erosion the up-rez cuts into this lump's part of
        /// the body — the same axis Graphic::CloudTypeShape::DetailCharacter is on, so a hero cloud and a
        /// procedural species mean the same thing by it.
        float DetailType = 1.0f;

        /// 0..1, how much matter this lump carries relative to the instance's own Density Factor. A wispy
        /// tail is thinner than the core it grew from, and this is where that is said.

        /// Same authored state? DEFAULTED so the compiler generates it from every member — see the long
        /// note at Graphic::CloudVerticalProfile::operator==. Read by the cloud documents' GetDiskState.
        [[nodiscard]] bool operator==( const CloudModellingBlob& ) const = default;
        float DensityScale = 1.0f;
    };

    /**
     * @brief Everything the generator needs to produce a modelling volume, and nothing it does not.
     *
     * Every field is written into the container and read back out of it; the sculpting tool of phase A1
     * edits exactly this struct.
     */
    struct CloudModellingVolumeRecipe
    {
        /// The world extent the body was sculpted at, kilometres, before the entity's own scale. At the
        /// default (2 x 1 x 2 km over 128 x 64 x 128 voxels) a voxel is 15.6 m on every axis.
        ///
        /// FIFTEEN METRES IS NOT A DEGRADATION, IT IS A TRANSFER OF WORK. Guerrilla store 8 m per voxel
        /// and the deck says in the same breath that 8 m of data yields 0.5 m of VISIBLE detail, because
        /// the detail is carried by the up-rez noise and not by the voxel (p.122). The volume carries the
        /// SILHOUETTE; Common/CloudField.glslh's erosion carries the edge. Both halves already exist.
        glm::vec3 SizeKm{ 2.0f, 1.0f, 2.0f };

        /// The radius over which two lumps FUSE, kilometres. It is the reciprocal of the exponential
        /// smooth-min's sharpness, expressed as a length because a length is what an artist can see: two
        /// lobes a quarter of this apart read as one body, two lobes ten times this apart read as two.
        ///
        /// It is also what makes the join non-optional to get right: the union INFLATES the surface by
        /// about `BlendRadiusKm * ln(blobCount)`, which is why ValidateCloudModellingRecipe accounts for
        /// it rather than checking the blobs' own extents.
        float BlendRadiusKm = 0.04f;

        /// How deep inside the body the Dimensional Profile reaches 1, kilometres. It is the
        /// normalisation of the distance field, and it is the one number that decides how much of the
        /// cloud the erosion is allowed to eat: the erosion is weighted by `1 - Profile`, so a small value
        /// makes a body that is solid almost everywhere and a large one makes a body that is edge all the
        /// way through.
        float ProfileDepthKm = 0.30f;

        /// How far OUTSIDE the body the cutout envelope still reads above zero, kilometres. It is the
        /// distance at which a procedural cloud is allowed to come back after a hero cloud has pushed it
        /// away, and it is a bake parameter rather than a runtime one because it is a dilation of THIS
        /// body and the bake is where the body's distance field exists.
        float EnvelopeMarginKm = 0.12f;

        /// The lumps, in any order. The join is commutative AND associative, so this is a set that happens
        /// to be stored in a sequence — nothing downstream may depend on the order, and
        /// Desert/Tests/Engine/CloudAuthored asserts a shuffled recipe bakes byte for byte the same
        /// volume.

        /// Same authored state? DEFAULTED so the compiler generates it from every member — see the long
        /// note at Graphic::CloudVerticalProfile::operator==. Read by the cloud documents' GetDiskState.
        [[nodiscard]] bool              operator==( const CloudModellingVolumeRecipe& ) const = default;
        std::vector<CloudModellingBlob> Blobs;
    };

    /// The generator's own version, written into every file. Bumped when the SCULPTING MATHS changes, so a
    /// volume baked before a change is recognisable as such instead of silently disagreeing with the code
    /// that now claims to have produced it.
    ///
    /// 1 — ellipsoid signed distance joined by exponential smooth-min, with the union's softmax weights
    ///     carrying Detail Type and Density Scale (ANALYSIS_APPROACH.md §3, PLAN_AUTHORED_CLOUDS.md §3).
    /// 2 — three primitives instead of one, each lump rigidly rotated about its own centre, and a
    ///     per-lump weight in the sum. A recipe whose lumps are all unrotated unit-weight ellipsoids is
    ///     the same arithmetic in the same order as 1 and bakes the same bytes; Desert/Tests/Engine/
    ///     CloudAuthored asserts that rather than asserting it in a comment.
    inline constexpr uint32_t kCloudModellingGeneratorVersion = 2u;

    /// The container layout's own version, independent of the maths. Bumped when a FIELD moves.
    ///
    /// 2 — a lump grew a primitive, a rotation and a weight, so the blob record is 52 bytes and not 32.
    ///     There is NO reader for version 1 and that is deliberate (contract §4): the old path is deleted
    ///     by the change that replaces it, and the one v1 file that ever existed —
    ///     kCloudModellingDefaultVolumeName — is re-baked by this same change. `Tools/CloudVolumeBaker
    ///     --in old.dcmv --out new.dcmv` is what a re-bake is FOR, and it cannot help across this bump,
    ///     which is why the recipe also lives in code as CloudModellingDefaultRecipe().
    /// 3 — the version-2 bytes after magic and version, inside the AF1 binary envelope (kind
    ///     CloudModellingVolume, the volume's GUID, this version under kCloudModellingSubsystemTag). A bare
    ///     container of any version is refused by name; Tools/SceneMigrator wraps it, once, in the file.
    inline constexpr uint32_t kCloudModellingContainerVersion = 3u;

    /// The tag the envelope header states kCloudModellingContainerVersion under.
    inline constexpr uint32_t kCloudModellingSubsystemTag = Common::Content::FourCC( "DCMV" );

    /// The volume's shape, FIXED BY THE FORMAT. Mirrored by CLOUD_MODELLING_VOLUME_WIDTH and its two
    /// siblings in Editor/Resources/Shaders/Common/CloudAuthored.glslh, which needs them as compile-time
    /// numbers to pull a fetch in by half a texel; Desert/Tests/Engine/CloudAuthored asserts the two
    /// statements agree.
    ///
    /// The vertical axis is HEIGHT and it is the short one. 128 x 64 x 128 in RGBA8 is 4.00 MiB, and the
    /// budget arithmetic is in Docs/Clouds/CALIBRATION.md §A0.
    inline constexpr uint32_t kCloudModellingVolumeWidth  = 128u; // x, world east
    inline constexpr uint32_t kCloudModellingVolumeHeight = 64u;  // y, up
    inline constexpr uint32_t kCloudModellingVolumeDepth  = 128u; // z, world north

    inline constexpr uint32_t kCloudModellingBytesPerVoxel = 4u;

    inline constexpr uint64_t kCloudModellingVoxelBytes =
         static_cast<uint64_t>( kCloudModellingVolumeWidth ) * kCloudModellingVolumeHeight *
         kCloudModellingVolumeDepth * kCloudModellingBytesPerVoxel;

    /// A decoded volume: the recipe that produced it, and the voxels themselves.
    struct CloudModellingVolumeData
    {
        /// The volume's identity, the GUID its envelope header states (container 3). Null on a volume never
        /// written; EncodeCloudModellingVolume mints one for it, so carrying it through an edit keeps the
        /// handle.
        Common::Content::AssetGuid Guid;

        CloudModellingVolumeRecipe Recipe;
        uint32_t                   GeneratorVersion = kCloudModellingGeneratorVersion;

        /// RGBA8, four bytes per voxel, tightly packed with x varying fastest and z slowest — the layout
        /// `vkCmdCopyBufferToImage` expects for a whole-volume copy with no row padding, which is how
        /// Graphic::Image3D uploads its `Data`. Stated here because the generator and the reader both have
        /// to agree on it and neither can see the other.
        std::vector<unsigned char> Voxels;
    };

    // -----------------------------------------------------------------------------------------------
    // THE SCULPTING MATHS, EXPOSED — because a SECOND producer bakes with it now
    // -----------------------------------------------------------------------------------------------
    //
    // These five declarations were private to the bake until phase Э5 gave the PROCEDURAL producer a
    // modelling volume of its own (Engine/Assets/CloudProceduralVolume.hpp). That producer places its lumps
    // by a hash instead of by an artist, and it walks them in a different ORDER — one lump splatted into
    // its own neighbourhood, rather than every lump gathered at one voxel — because a region holds hundreds
    // of lumps where a sculpted body holds at most 64.
    //
    // A DIFFERENT LOOP MUST NOT MEAN A DIFFERENT FORMULA. The distance, the join's per-lump term and the
    // join itself are the text below, called by both, so that "the two producers agree about what a lump
    // is" is true by construction rather than by vigilance — which is exactly the two-statements-of-one-
    // fact defect class contract §2.3.1 names. Desert/Tests/Engine/CloudProceduralField still asserts it on
    // the numbers, because that class survives good intentions.

    /// One lump with everything that does not depend on the sample point already done: the rotation
    /// inverted (a rotation matrix is orthonormal, so the inverse is the transpose — exact and free of the
    /// drift a general inverse would add), and the two material numbers carried along.
    struct CloudModellingPreparedBlob
    {
        glm::vec3               CentreKm{ 0.0f };
        glm::vec3               RadiiKm{ 0.0f };
        glm::mat3               IntoLocal{ 1.0f };
        CloudModellingPrimitive Primitive    = CloudModellingPrimitive::Ellipsoid;
        float                   Weight       = 1.0f;
        float                   DetailType   = 1.0f;
        float                   DensityScale = 1.0f;
    };

    CloudModellingPreparedBlob PrepareCloudModellingBlob( const CloudModellingBlob& blob );

    /// The signed distance from @p pointKm to that lump, kilometres, negative inside. Exact for a sphere
    /// and a capsule, a tight underestimate for an ellipsoid — and an underestimate is the safe direction,
    /// because it makes the body slightly smaller than the bound Validate checked, never larger.
    float CloudModellingBlobDistanceKm( const CloudModellingPreparedBlob& blob, const glm::vec3& pointKm );

    /// The lump's world-axis-aligned half-extent once it is rotated. `|R| * r` is the exact AABB of a
    /// rotated box and every primitive is contained in its own box, so this bounds all three.
    glm::vec3 CloudModellingBlobHalfExtentKm( const CloudModellingBlob& blob );

    /// Sorts lumps into THE canonical order, in place. The join is commutative and associative in real
    /// arithmetic and neither in floating point, so a bake whose bytes must not depend on the order its
    /// inputs arrived in sorts first. The key covers every authored number: two lumps differing only in
    /// rotation are different lumps, and a key that could not tell them apart would leave their order to
    /// `std::sort`'s internals — the very non-determinism the sort removes.
    void SortCloudModellingBlobs( std::vector<CloudModellingBlob>& blobs );

    /// One lump's term in the smooth minimum's sum, with the SHIFT already applied.
    ///
    /// The shift is what keeps this finite: `exp(-d/r)` overflows a float once `d/r` passes about 88, which
    /// at a 60 m blend radius is 5.3 km — nearer than the corner of many regions. Subtracting the smallest
    /// distance first is algebraically the identity and moves the largest exponent to exactly 1.
    inline float CloudModellingJoinTerm( float weight, float distanceKm, float nearestKm, float invBlendRadius )
    {
        return weight * std::exp( -( distanceKm - nearestKm ) * invBlendRadius );
    }

    /// The exponential smooth minimum, shifted back: `nearest - r*ln(sum)`.
    inline float CloudModellingJoinKm( float nearestKm, float weightSum, float blendRadiusKm )
    {
        return nearestKm - std::log( weightSum ) * blendRadiusKm;
    }

    /**
     * @brief Lays several baked bodies end to end into the bytes of ONE volume — the atlas the march
     *        samples every hero cloud of a frame through.
     *
     * WHY ALONG THE DEPTH AXIS AND NOT THE OTHER TWO. The layout above has x varying fastest and z
     * slowest, so stacking on z makes the atlas the bodies' bytes CONCATENATED and nothing else: one
     * `memcpy` per body, and a test can assert the whole result with `==` against a concatenation it built
     * itself. Stacking on y would interleave the bodies 128 slices deep and turn one copy into 1 024, for
     * a picture that is identical — the cost is real and the benefit is zero.
     *
     * WHAT KEEPS THE BODIES FROM BLEEDING INTO EACH OTHER is not here and not the bake's empty shell: it
     * is CloudAuthoredAtlasUvw, which never carries a coordinate past the first or last TEXEL CENTRE of a
     * slab, so the trilinear filter's two depth taps are always two texels of the same body.
     *
     * @param bodies  the voxel arrays, in slab order. Each must be exactly kCloudModellingVoxelBytes long,
     *                which every array Decode or Generate produces is.
     * @return the atlas bytes — `bodies.size()` times kCloudModellingVoxelBytes — or an error naming which
     *         body was the wrong length. An empty list is an ERROR and not an empty atlas: a zero-depth
     *         image cannot be created, and the caller's answer to "no bodies" is to bind the fallback
     *         rather than to build nothing.
     */
    Common::ResultStr<std::vector<unsigned char>>
    AssembleCloudModellingAtlas( const std::vector<const std::vector<unsigned char>*>& bodies );

    /**
     * @brief Rejects a recipe the generator cannot honour, with the offending number in the message.
     *
     * A pure function, so the sculpting tool can grey out its Bake button for the same reason the loader
     * refuses the file, rather than the two disagreeing about what is legal.
     *
     * THE INTERESTING CHECK IS THE LAST ONE: the body, INFLATED BY THE JOIN, must fit inside the box with
     * the envelope's margin to spare. A body that touches its own boundary is a cloud with a flat face
     * where the volume was cut, and — because every sampler in this engine is REPEAT — its top would blend
     * with its own bottom through the trilinear filter. The inflation term is `BlendRadiusKm * ln(N)`,
     * which is the exact amount an exponential smooth-min of N equal distances pushes the zero set
     * outward, so the bound is arithmetic rather than a guess.
     */
    Common::BoolResultStr ValidateCloudModellingRecipe( const CloudModellingVolumeRecipe& recipe );

    /**
     * @brief Bakes a recipe into voxels.
     *
     * Pure and GPU-free: in a recipe, out 4 MiB of RGBA8 in the layout above. Roughly one second per
     * volume in a debug build, which is why the result is SHIPPED AS A FILE rather than generated at load
     * — the same decision, for the same reason, that `.dcnv` records on kCloudNoiseDefaultVolumeName.
     *
     * The construction, and why each half of it is this and not something else:
     *
     *   * EACH LUMP is an ellipsoid signed distance. Inigo Quilez's bounded form `k0*(k0-1)/k1`, which is
     *     EXACT for a sphere and a tight underestimate otherwise — an underestimate is the safe direction,
     *     because it makes the body slightly smaller than the bound Validate checked.
     *   * THE JOIN is the exponential smooth minimum, `-ln(sum(exp(-d_k/r)))*r`. Three properties, and all
     *     three are load-bearing (PLAN_AUTHORED_CLOUDS.md §3): it is COMMUTATIVE AND ASSOCIATIVE, so the
     *     order lumps were sculpted in cannot move the result and the tool need not store one; its
     *     SOFTMAX WEIGHTS are a partition of unity over the lumps, so Detail Type and Density Scale fall
     *     out of the join instead of being invented separately; and the result IS a normalised distance
     *     field, which is the exact quantity Guerrilla derive their Dimensional Profile from (deck p.85),
     *     obtained analytically rather than by a distance transform.
     *   * THE SUM IS SHIFTED by the smallest distance before it is exponentiated. Without that shift
     *     `exp(-d/r)` overflows for any point more than about 700 blend radii outside the body, which at
     *     the shipped 40 m radius is 28 km — closer than the corners of many boxes — and the answer comes
     *     back as an infinity that quantises to a solid voxel. Shifting is algebraically the identity.
     *
     * @return the voxels, or an error naming what went wrong. A recipe Validate accepts always bakes;
     *         the one error this can return that Validate cannot is the empirical one — a body that
     *         reaches the volume's boundary after all — and it names the face it touched.
     */
    Common::ResultStr<std::vector<unsigned char>>
    GenerateCloudModellingVolume( const CloudModellingVolumeRecipe& recipe );

    /**
     * @brief Told how far the bake has got, and asked whether to carry on.
     *
     * @param  fraction 0 at the start, 1 at the end, monotonically increasing.
     * @return false to abandon the bake, which then returns an error rather than a partial volume.
     *
     * THE RETURN IS NOT A CONVENIENCE. A debug bake of 1 048 576 voxels takes tens of seconds, and the
     * panel runs it on a worker; without a way to stop it, closing the panel or quitting the editor has to
     * block on a thread that has no reason to finish. Cancellation is what makes the panel's destructor
     * bounded, so it is part of the contract of the function rather than a feature of the caller.
     */
    using CloudModellingBakeProgressFn = std::function<bool( float fraction )>;

    /**
     * @brief The same bake, reporting progress and able to be abandoned.
     *
     * PURITY IS UNCHANGED and this is the point worth being careful about. @p onProgress may not influence
     * the result: it is called between slabs, it is handed a number, and nothing it does is read back into
     * the arithmetic. Baking with a callback and baking without one produce identical bytes, and
     * Desert/Tests/Engine/CloudModellingRecipe asserts exactly that — a "pure function with a progress
     * hook" is otherwise a claim that nobody has checked.
     *
     * The single-argument overload above is this one with an empty callback, so there is ONE bake and not
     * two that must be kept in step.
     */
    Common::ResultStr<std::vector<unsigned char>>
    GenerateCloudModellingVolume( const CloudModellingVolumeRecipe&   recipe,
                                  const CloudModellingBakeProgressFn& onProgress );

    /// Which axis a preview slice is taken ACROSS — the axis whose coordinate the slice holds fixed.
    enum class CloudModellingAxis : uint32_t
    {
        X = 0, ///< a slice at constant x; the image is depth (z) across by height (y) down
        Y = 1, ///< a slice at constant y, the horizontal cut through the body; width (x) by depth (z)
        Z = 2, ///< a slice at constant z; width (x) across by height (y) down
    };

    const char* CloudModellingAxisName( CloudModellingAxis axis );

    /// How many voxels deep the volume is along @p axis — the exclusive upper bound on a slice index.
    uint32_t CloudModellingAxisExtent( CloudModellingAxis axis );

    /// One plane of the volume, RGBA8, tightly packed, row 0 first.
    struct CloudModellingSlice
    {
        uint32_t                   Width  = 0;
        uint32_t                   Height = 0;
        std::vector<unsigned char> Pixels;
    };

    /**
     * @brief Bakes ONE plane of the volume, for a preview that keeps up with a slider.
     *
     * WHY THIS EXISTS AND IS NOT A CROP OF THE FULL BAKE. A three-dimensional field cannot be shown
     * directly, so the panel shows slices — the same answer Editor/.../CloudNoiseVolumePanel reached for
     * `.dcnv`. But the full bake is tens of seconds in a debug build, and a preview that costs a full bake
     * is a preview nobody looks at while sculpting. A plane is 1/64 or 1/128 of the volume, which is the
     * difference between a slider that responds and one that does not.
     *
     * IT IS THE SAME CODE, NOT THE SAME FORMULA WRITTEN TWICE. The per-voxel evaluation is one function
     * that both loops call, so "the preview shows what will be baked" is true by construction rather than
     * by vigilance. Desert/Tests/Engine/CloudModellingRecipe still asserts it on the bytes, because the
     * two-statements-of-one-fact defect class (contract §2.3.1) is precisely the one that survives good
     * intentions.
     *
     * NOTE that the boundary check the full bake ends with is NOT performed here: a single plane cannot
     * observe the six faces, and a preview that refused to draw because some OTHER plane touches the box
     * would hide the very picture the artist needs in order to fix it. Save is where that refusal belongs,
     * and Save is where it happens.
     *
     * @param index which plane, 0 .. CloudModellingAxisExtent(axis) - 1.
     * @return the plane, or an error — a recipe Validate rejects, or an index outside the volume.
     */
    Common::ResultStr<CloudModellingSlice> GenerateCloudModellingSlice( const CloudModellingVolumeRecipe& recipe,
                                                                        CloudModellingAxis axis, uint32_t index );

    /**
     * @brief Serialises a volume into its payload: the container fields after magic and version, the lumps,
     *        then the voxels.
     *
     * Total: any @p data whose recipe Validate accepts encodes. The result is exactly
     * `kCloudModellingHeaderSize + kCloudModellingBlobBytes * blobCount + kCloudModellingVoxelBytes` bytes.
     * The GUID is not in it - the envelope carries that.
     */
    std::vector<unsigned char> EncodeCloudModellingPayload( const CloudModellingVolumeData& data );

    /**
     * @brief Parses a payload back into a volume (GUID left null), or says why it could not.
     *
     * REFUSES RATHER THAN GUESSES, and each refusal names the number that was wrong: extents that are not
     * the format's, a truncated payload, a checksum that disagrees, a recipe Validate rejects. A silent
     * fallback here would be a hero cloud rendered from whatever bytes happened to be in the file, which is
     * the single hardest class of defect to trace back.
     */
    Common::ResultStr<CloudModellingVolumeData>
    DecodeCloudModellingPayload( const std::vector<unsigned char>& bytes );

    /**
     * @brief Serialises a volume into its AF1 binary envelope (kind CloudModellingVolume, the volume's GUID,
     *        one PAYL section).
     *
     * The GUID travels in `data.Guid`; a null one is a volume never written before and gets a fresh GUID
     * here.
     */
    Common::ResultStr<std::vector<unsigned char>>
    EncodeCloudModellingVolume( const CloudModellingVolumeData& data );

    /**
     * @brief Reads a `.dcmv` file: the envelope, then its payload. The GUID comes back in `Guid`.
     *
     * A BARE 'DCMV' CONTAINER (versions 1 and 2) IS REFUSED BY NAME, never read: it has no GUID, so reading
     * it would hand the volume a path-derived handle nothing else agrees with. The migrator wraps it.
     */
    Common::ResultStr<CloudModellingVolumeData>
    DecodeCloudModellingVolume( const std::vector<unsigned char>& file );

    /**
     * @brief The GUID a `.dcmv` envelope header on @p in states, reading only the header.
     *
     * Refuses by name a stream that is not a modelling-volume envelope (a bare container, another kind,
     * a truncated header). The asset adopts the GUID at creation; a re-bake over an existing file keeps it.
     */
    Common::ResultStr<Common::Content::AssetGuid> ReadCloudModellingVolumeGuid( std::istream& in );

    /// Byte length of the payload's FIXED header (the version-2 container header less its magic and
    /// version) — the part before the blob array. Exposed because the round-trip test asserts the payload
    /// size, and a header that grew without this constant moving would pass a test that meant nothing.
    inline constexpr size_t kCloudModellingHeaderSize = 80u;

    /// Bytes per blob in the file: twelve floats and one `uint32`, in the order the encoder writes them.
    inline constexpr size_t kCloudModellingBlobBytes = 52u;

    /// The four bytes a BARE container (versions 1 and 2, before the envelope) started with, kept so the
    /// decoder can refuse such a file by name and the migrator can recognise what it wraps.
    /// `DCMV` — Desert Cloud Modelling Volume, beside `DCNV`, Desert Cloud Noise Volume.
    inline constexpr char kCloudModellingMagic[4] = { 'D', 'C', 'M', 'V' };

    /// The extension the Content Browser, the file dialog and the drag-and-drop payload all agree on.
    inline constexpr const char* kCloudModellingVolumeExtension = ".dcmv";

    /**
     * @brief The recipe the shipped example volume is baked from, and the one a new asset starts from.
     *
     * A CUMULUS CONGESTUS AS A FUSED MASS: a flattened base, two shoulders, a body and a tower that grow
     * out of one another, plus a wispy tail. It is content expressed in code for exactly as long as phase
     * A0 lasts — the sculpting tool of A1 authors this struct in a panel — and it is here rather than in
     * the tool because `Tools/CloudVolumeBaker` is a mechanism and a specific cloud is content
     * (`desert-engine-dev`, "hardcoding content into a mechanism"). The precedent is
     * Assets::CloudTypeDefaultShape, which is the same shape of answer for the same reason: a scene that
     * nobody has sculpted for still has to have something to look at.
     *
     * ITS PURPOSE IS TO BE SOMETHING THE PROCEDURAL FIELD CANNOT BE. Every lump here overlaps its
     * neighbour by more than the blend radius, so the join fuses them into one connected body with a
     * single surface — which is precisely what `best - second` forbids by construction.
     */
    const CloudModellingVolumeRecipe& CloudModellingDefaultRecipe();

    /// The file name the shipped example is written under, under `Resources/Assets/Clouds/Volumes/`.
    inline constexpr const char* kCloudModellingDefaultVolumeName = "HeroCloud_Congestus.dcmv";

    /// Where the library lives, RELATIVE to the project's assets root — the string a scene stores in a
    /// hero cloud's slot. It must agree with Common::Constants::Path::CLOUD_VOLUME_PATH, which is the same
    /// directory expressed against the project root; Desert/Tests/Engine/CloudAuthored asserts that the
    /// second ends with the first rather than trusting the two to be edited together.
    inline constexpr const char* kCloudModellingAssetsRelativeDir = "Clouds/Volumes/";
} // namespace Desert::Assets
