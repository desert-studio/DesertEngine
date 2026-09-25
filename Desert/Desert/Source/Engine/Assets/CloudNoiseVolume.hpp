#pragma once

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Assets
{
    /**
     * @file
     * @brief The cloud noise volume: its recipe, its container format, and the pure functions that turn
     *        one into the other.
     *
     * WHY THIS IS A FILE FORMAT AND NOT A BAKE. Until now the volume was the output of a compute pass keyed
     * on a seed, which meant an artist could neither see it, keep it, nor bring their own. The deck's own
     * pipeline ships the volume as DATA (Nubis Cubed, SIGGRAPH 2023, p.96 — "Voxel Cloud Noise, 4 Channel,
     * 128 x 128 x 128 Voxels", distributed as TGA slices and a VDB), and UE's cloud material takes it as
     * `Noise_Texture3D` on the material instance. A volume that is not a file cannot be either.
     *
     * WHY OUR OWN CONTAINER AND NOT DDS/KTX/BCn. There is not one of them anywhere in this tree — the
     * engine's `ImageFormat` knows RGBA8F / RGBA16F / RGBA32F / BGRA8F and the two depth formats and
     * nothing else — and on Apple Silicon the BC family is not exposed for 3D textures at all
     * (Docs/Clouds/ANALYSIS_APPROACH.md section 2.2). Adopting one of them would mean adopting a library to
     * read it, for a payload that is a tightly packed array of bytes.
     *
     * THE FILE CARRIES ITS OWN RECIPE, and that is the property that makes it an artist's asset rather than
     * an opaque blob: the header stores the seed, the four lattice periods, the curl strength and the
     * generator version, so any volume can be reproduced, compared, or re-rolled from what is written in
     * it. A file whose parameters live only in whoever baked it is a file nobody dares change.
     */

    /// What a channel of a volume CONTAINS. Stored per channel in the header, so a volume is
    /// self-describing and a mislabelled or re-ordered one is a diagnosable error rather than a picture
    /// that simply looks wrong.
    ///
    /// The four values are the deck's own set and its own order (p.96): "The first two channels are low and
    /// high frequency Curly-Alligator noise and the last two are Low and High frequency Alligator Noise."
    enum class CloudNoiseChannel : uint32_t
    {
        CurlyAlligatorLowFrequency  = 0, ///< wispy, coarse — web-like filaments sheared by a curl field
        CurlyAlligatorHighFrequency = 1, ///< wispy, fine
        AlligatorLowFrequency       = 2, ///< billowy, coarse — the lobes a convective cloud is made of
        AlligatorHighFrequency      = 3, ///< billowy, fine
    };

    const char* CloudNoiseChannelName( CloudNoiseChannel channel );

    /**
     * @brief Everything the generator needs to produce a volume, and nothing it does not.
     *
     * Every field is authored in the editor panel, written into the container and read back out of it.
     * There is no field here that only one of those three touches.
     */
    struct CloudNoiseVolumeParams
    {
        /// Voxels per axis. 128 is the deck's number (p.96) and, in RGBA8, exactly 8 MiB — inside the
        /// 64 MiB per-subsystem budget with room for the two volumes a scene might want at once.
        uint32_t Resolution = 128;

        /// Changes which noise, not what kind of noise. Re-rolling it gives a different sky with the same
        /// statistics, which is the one thing a seed is for.
        uint32_t Seed = 1337;

        /// How far the curl flow shears the wispy channels, in lattice cells. A third of a cell is enough
        /// to hook an edge over; past about half a cell the wisps stop belonging to the cloud they came
        /// from and the field reads as smeared rather than as blown.
        float CurlStrength = 0.33f;

        /// Lattice cells across the unit cube, per channel. THESE MUST DIVIDE THE CUBE A WHOLE NUMBER OF
        /// TIMES or REPEAT sampling puts a seam across the sky at every tile boundary — hence the integral
        /// check in ValidateCloudNoiseVolumeParams rather than a comment hoping for it.
        ///
        /// 2 / 4 / 3 / 6 ARE MEASURED, NOT CHOSEN. The first default was twice as fine (4 / 8 / 6 / 12) and
        /// the frame said no: the wispy pair is what Common/CloudField.glslh thresholds as its COVERAGE
        /// field, and at that frequency one coverage cell is smaller than the quarter-resolution trace can
        /// resolve, so the zenith came out as speckled shreds with a measured contrast of 0.100 against the
        /// 0.483 of the sky it replaced. Halving the frequency lifts it to 0.321 with the same noise and
        /// the same seed. The finer volume is still a legitimate asset and ships beside this one as
        /// CloudNoise_FineWisp.dcnv — this is the DEFAULT because it is the one that reads as weather at
        /// the resolution the march actually runs at.
        float WispyPeriodLowFrequency   = 2.0f;
        float WispyPeriodHighFrequency  = 4.0f;
        float BillowPeriodLowFrequency  = 3.0f;

        /// Same authored state? DEFAULTED so the compiler generates it from every member — see the long
        /// note at Graphic::CloudVerticalProfile::operator==. Read by the cloud documents' GetDiskState.
        [[nodiscard]] bool operator==( const CloudNoiseVolumeParams& ) const = default;
        float BillowPeriodHighFrequency = 6.0f;
    };

    /// The generator's own version, written into every file. It is bumped when the NOISE MATHS changes, so
    /// a volume baked before a change is recognisable as such instead of silently disagreeing with the
    /// code that now claims to have produced it.
    ///
    /// 1 — Alligator / Curly-Alligator (Nubis Cubed p.95-96), replacing four channels of Perlin-Worley,
    ///     inverted Worley and curl-sheared Perlin.
    inline constexpr uint32_t kCloudNoiseGeneratorVersion = 1u;

    /// The container layout's own version, independent of the maths. Bumped when a FIELD moves.
    ///
    /// 1 — magic, versions, resolution, format, four channel meanings, the recipe, length and CRC.
    /// 2 — adds @ref CloudNoiseVolumeOrigin, because a volume can now arrive from OUTSIDE the generator and
    ///     a file that cannot say so would have to lie about its recipe. See the enum.
    /// 3 — the version-2 bytes after magic and version, inside the AF1 binary envelope (kind
    ///     CloudNoiseVolume, the volume's GUID, this version under kCloudNoiseSubsystemTag). A bare container
    ///     of any version is refused by name; Tools/SceneMigrator wraps it, once, in the file.
    inline constexpr uint32_t kCloudNoiseContainerVersion = 3u;

    /// The tag the envelope header states kCloudNoiseContainerVersion under.
    inline constexpr uint32_t kCloudNoiseSubsystemTag = Common::Content::FourCC( "DCNV" );

    /**
     * @brief Where a volume's voxels came from — and therefore whether the recipe beside them means
     *        anything.
     *
     * THIS EXISTS BECAUSE IMPORT BROKE AN INVARIANT THIS FILE STATES AT THE TOP: "the file carries its own
     * recipe". A volume read from a tiled slice sheet has no seed, no periods and no curl strength, because
     * a picture cannot carry them. Writing plausible-looking defaults into its header would have made the
     * file claim a recipe that does not reproduce it — and the trap is not theoretical: the editor panel
     * shows those fields and offers a Bake button, so one click would silently replace an artist's imported
     * voxels with entirely different generated ones, with nothing on screen having warned them.
     *
     * So the file states which of the two it is, and every reader is forced to ask.
     */
    enum class CloudNoiseVolumeOrigin : uint32_t
    {
        /// Baked by `GenerateCloudNoiseVolume` from the recipe in this header. Re-baking that recipe
        /// reproduces these exact bytes — that is the property the generator's purity test defends.
        Generated = 0,

        /// Read from a tiled slice sheet. The recipe fields are ZERO and mean nothing; only Resolution is
        /// real, because the pixels genuinely state it. Re-baking would produce a DIFFERENT volume.
        Imported = 1,
    };

    const char* CloudNoiseVolumeOriginName( CloudNoiseVolumeOrigin origin );

    /// A decoded volume: what it contains, how it was made, and the voxels themselves.
    struct CloudNoiseVolumeData
    {
        /// The volume's identity, the GUID its envelope header states (container 3). Null on a volume never
        /// written; EncodeCloudNoiseVolume mints one for it, so carrying it through an edit keeps the handle.
        Common::Content::AssetGuid Guid;

        CloudNoiseVolumeParams Params;
        uint32_t               GeneratorVersion = kCloudNoiseGeneratorVersion;

        /// Defaults to Generated because that is what constructing one in code and filling in a recipe
        /// means. The sheet importer is the only thing in the tree that sets Imported.
        CloudNoiseVolumeOrigin Origin = CloudNoiseVolumeOrigin::Generated;

        /// RGBA8, four bytes per voxel, tightly packed with x varying fastest and z slowest — the layout
        /// `vkCmdCopyBufferToImage` expects for a whole-volume copy with no row padding, which is how
        /// Graphic::Image3D uploads its `Data`. Stated here because the generator and the reader both have
        /// to agree on it and neither can see the other.
        std::vector<unsigned char> Voxels;

        uint64_t VoxelCount() const
        {
            const uint64_t r = Params.Resolution;
            return r * r * r;
        }
    };

    /**
     * @brief Rejects a RESOLUTION the container cannot hold, with the offending number in the message.
     *
     * Split out of ValidateCloudNoiseVolumeParams when volumes gained an origin, because the two halves of
     * that check answer different questions and only one of them survives an import. A resolution is a
     * property of the VOXELS — an imported volume has one, and it must be as legal as any other, or the
     * payload length check downstream is checking a size nothing bounded. The recipe is a property of the
     * GENERATOR, and an imported volume simply does not have one.
     */
    Common::BoolResultStr ValidateCloudNoiseVolumeResolution( uint32_t resolution );

    /**
     * @brief Rejects a parameter set the generator cannot honour, with the offending number in the message.
     *
     * A pure function so the panel can grey out its Bake button for the same reason the loader refuses the
     * file, rather than the two disagreeing about what is legal.
     *
     * ONLY MEANINGFUL FOR A GENERATED VOLUME. Applying it to an imported one would reject every import on
     * the grounds that zero is not a whole number of lattice cells — a true statement about a field that
     * carries no meaning. `DecodeCloudNoiseVolume` therefore asks this of Generated volumes and asks only
     * @ref ValidateCloudNoiseVolumeResolution of Imported ones.
     */
    Common::BoolResultStr ValidateCloudNoiseVolumeParams( const CloudNoiseVolumeParams& params );

    /// The recipe an IMPORTED volume carries: empty, at every field the generator would have filled. A
    /// function rather than a literal in two places, because the encoder writes it and the decoder asserts
    /// it, and those are exactly the two sides that must not drift apart.
    CloudNoiseVolumeParams EmptyImportedRecipe( uint32_t resolution );

    /**
     * @brief Serialises a volume into its payload: the container fields after magic and version, then the
     *        voxels.
     *
     * Total: any @p data that Validate accepts encodes. The result is exactly
     * `kCloudNoiseHeaderSize + 4 * Resolution^3` bytes. The GUID is not in it - the envelope carries that.
     */
    std::vector<unsigned char> EncodeCloudNoisePayload( const CloudNoiseVolumeData& data );

    /**
     * @brief Parses a payload back into a volume (GUID left null), or says why it could not.
     *
     * REFUSES RATHER THAN GUESSES, and each refusal names the number that was wrong: a resolution that does
     * not match the payload length, a truncated payload, a checksum that disagrees, an unknown format or
     * channel order. A silent fallback here would be a sky that renders from whatever bytes happened to be
     * in the file, which is the single hardest class of defect to trace back.
     */
    Common::ResultStr<CloudNoiseVolumeData> DecodeCloudNoisePayload( const std::vector<unsigned char>& payload );

    /**
     * @brief Serialises a volume into its AF1 binary envelope (kind CloudNoiseVolume, the volume's GUID,
     *        one PAYL section).
     *
     * The GUID travels in `data.Guid`; a null one is a volume never written before and gets a fresh GUID
     * here.
     */
    Common::ResultStr<std::vector<unsigned char>> EncodeCloudNoiseVolume( const CloudNoiseVolumeData& data );

    /**
     * @brief Reads a `.dcnv` file: the envelope, then its payload. The GUID comes back in `Guid`.
     *
     * A BARE 'DCNV' CONTAINER (versions 1 and 2) IS REFUSED BY NAME, never read: it has no GUID, so reading
     * it would hand the volume a path-derived handle nothing else agrees with. The migrator wraps it.
     */
    Common::ResultStr<CloudNoiseVolumeData> DecodeCloudNoiseVolume( const std::vector<unsigned char>& file );

    /// Byte length of the payload header (the version-2 container header less its magic and version).
    /// Exposed because the round-trip test asserts the payload size, and a header that grew without the
    /// constant moving would pass a test that meant nothing.
    inline constexpr size_t kCloudNoiseHeaderSize = 68u;

    /// The four bytes a BARE container (versions 1 and 2, before the envelope) started with. Kept so the
    /// decoder can refuse such a file by name and the migrator can recognise what it wraps.
    inline constexpr char kCloudNoiseMagic[4] = { 'D', 'C', 'N', 'V' };

    /// The extension the Content Browser, the file dialog and the drag-and-drop payload all agree on.
    inline constexpr const char* kCloudNoiseVolumeExtension = ".dcnv";

    /// The volume an EMPTY SLOT resolves to. It is a real asset shipped with the project rather than
    /// something the engine synthesises, and that is a measurement rather than a preference: generating
    /// 128^3 costs 9.6 s optimised and 82 s unoptimised on the machine this was written on, so a runtime
    /// fallback would stall every launch of a scene with clouds in it. Shipping it also means the artist can
    /// SEE the default in the Content Browser and duplicate it, which is what the panel expects them to do.
    inline constexpr const char* kCloudNoiseDefaultVolumeName = "CloudNoise_Default.dcnv";
} // namespace Desert::Assets
