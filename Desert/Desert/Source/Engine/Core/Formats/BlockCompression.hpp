#pragma once

// THE BLOCK ENCODER — what it encodes, what it deliberately refuses, and how it is measured.
//
// `Docs/Textures/T3_FORMAT_PLAN.md` §6 puts this last of four steps, and the reason is hardware rather
// than taste: `blitDst = 0` for every BC format on this device, so the GPU cannot build a block-format
// mip chain and the chain had to be in the file before a block format could exist at all. It is, since
// v1 of the container. This file is the step that finally puts blocks in it.
//
// ── FOUR FORMATS, AND WHERE THE CHOICE BETWEEN THEM IS MADE ─────────────────────────────────────
//
// `Docs/Textures/T1_BCN_MEASUREMENT.md` names four and its rules are binding: BC5 for normals and
// NEVER BC7 on them (52.51 dB against 46.48, and BC5 encodes 106x faster on the same image); BC4 for
// single-channel masks; nothing block-compressed for noise (29.29 dB, the one substitution that was
// visible in a frame). Every one of those three rules needs to know WHAT A TEXTURE IS FOR, and until
// `Core/Formats/TextureIntent.hpp` there was no field that said so — this file carried two formats for
// exactly that reason, and the missing half was never the encoder.
//
// THE POLICY IS STILL NOT HERE. It is `BlockPolicyForIntent`, one pure function of the authored intent
// and the source format, and this file knows only how to turn pixels into blocks. That split is what
// lets the rules be tested without an importer and the encoders be measured without an opinion.
//
// ── QUALITY IS MEASURED IN THIS REPOSITORY, NOT QUOTED FROM A REFERENCE ENCODER ──────────────────
//
// These are OUR encoders and they are simpler than the reference ones. BC7 here writes MODE 6 only
// (one subset, RGBA endpoints, 4-bit indices) out of the eight modes the format has, and BC6H writes
// MODE 11 only (one subset, 10-bit absolute endpoints) out of fourteen. A partitioned mode helps a
// block that straddles an edge between two materially different colours, and neither of these will
// find that. What they DO is chosen for what this project stores: smooth colour and smooth radiance.
//
// BC4 IS THE EXCEPTION AND IS IMPLEMENTED WHOLE: the format has no modes, both of its palette shapes
// are here, and BC5 is literally two of its blocks. That is not this file being more careful about one
// format — it is the format being small enough that "a subset of it" would have been more code than
// all of it.
//
// The numbers are in `Desert/Tests/Engine/BlockCompression`, measured over this tree's own images,
// and the suite FAILS if they regress. Claims about "BC7 quality" taken from a table elsewhere describe
// a different encoder.
//
// ── THE DECODERS ARE PART OF THE DELIVERABLE, NOT A TEST HELPER ──────────────────────────────────
//
// An encoder verified only by "the device drew something" is verified by nothing: this machine returns
// BC7 texels bit-exact WITH THE FEATURE DISABLED (measured, MoltenVK 1.1.357, four mode-6 blocks, no
// validation message either way), so the device is not a witness here. The decoders below implement the
// same arithmetic the specification requires of a GPU, and every encode in the suite is measured by
// decoding it back — which is the only check that can fail on this hardware.

#include <Common/Core/ResultStr.hpp>
#include <Engine/Core/Formats/ImageFormat.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Desert::Core::Formats
{
    /// THE LARGEST VALUE BC6H_UFLOAT CAN HOLD: its endpoints are half-float bit patterns, so its ceiling
    /// is the largest finite half. The encoder clamps anything above it rather than writing an infinity,
    /// and that clamp is an ENERGY LOSS on real skies — rural_asphalt_road_2k.hdr's sun peaks at 131072 —
    /// so the one producer that feeds radiance into this encoder (`WriteBakedEnvironmentCube`) counts
    /// what it is about to lose against this same number and says so.
    inline constexpr float kBC6HLargestValue = 65504.0f;

    /// What encoding RGBA32F texels to BC6H would do to them, counted BEFORE it is done: the encoder
    /// writes a non-finite channel as 0 and clamps anything above `kBC6HLargestValue`, and neither is
    /// visible in its output. Channels are R, G, B; alpha is not stored by BC6H and is not counted.
    struct BC6HCeilingCensus
    {
        uint64_t NonFiniteChannels = 0;
        uint64_t ClampedTexels     = 0;   ///< texels with at least one channel above the ceiling
        float    Peak              = 0.0f;
        double   Sum               = 0.0; ///< of every finite, non-negative channel
        double   LostAboveCeiling  = 0.0; ///< of (channel - ceiling) where it is positive

        /// The share of the counted energy the clamp removes; 0 for an empty or black image.
        [[nodiscard]] double LostFraction() const { return Sum > 0.0 ? LostAboveCeiling / Sum : 0.0; }
    };

    /// @p rgba holds @p texelCount tightly packed RGBA32F texels.
    [[nodiscard]] BC6HCeilingCensus CensusBC6HCeiling( const float* rgba, std::size_t texelCount );

    /// THE BLOCK FORMAT A SOURCE FORMAT ENCODES INTO WHEN NOBODY SAID OTHERWISE, or `ImageFormat::Count`
    /// for "this one does not".
    ///
    /// It is a function of the SOURCE's numeric range and nothing else — RGBA8 is LDR colour and becomes
    /// BC7, RGBA32F is radiance and becomes BC6H — and that is the whole of what the cook can derive
    /// without being told. It is the DEFAULT, not the policy: `TextureIntent.hpp`'s
    /// `BlockPolicyForIntent` is what an author's answer goes through, and this is what the cook falls
    /// back to when there is no author's answer to go through it.
    [[nodiscard]] ImageFormat BlockFormatFor( ImageFormat sourceFormat );

    /// THE UNCOMPRESSED FORMAT A BLOCK FORMAT IS ENCODED FROM AND DECODES BACK TO, or
    /// `ImageFormat::Count` when the argument is not a block format at all.
    ///
    /// IT IS NOT THE INVERSE OF `BlockFormatFor` AND MUST NOT BE WRITTEN AS ONE. Three block formats
    /// are encoded from RGBA8, so the forward relation stopped being a bijection the day the authored
    /// field arrived; this direction is still single-valued, which is why it is the one both entry
    /// points below check their arguments with.
    [[nodiscard]] ImageFormat SourceFormatFor( ImageFormat blockFormat );

    /// Encode one tightly-packed image. @p source holds exactly
    /// `CalculateImageSize(width, height, sourceFormat)` bytes; what comes back holds exactly
    /// `CalculateImageSize(width, height, blockFormat)` — i.e. the level rounded up to whole 4x4 blocks,
    /// which for a level under four texels is a WHOLE BLOCK and not a fraction of one.
    ///
    /// A level whose extent is not a multiple of four is legal and is what the small end of every mip
    /// chain looks like. The texels that fall outside the image are filled by CLAMPING to the edge
    /// rather than left as zero: a black quarter-block at the edge of a 6x6 level would be encoded as
    /// part of the same endpoint fit and would drag the real texels' colours with it.
    [[nodiscard]] Common::ResultStr<std::vector<unsigned char>>
    BlockCompressImage( uint32_t width, uint32_t height, ImageFormat sourceFormat, ImageFormat blockFormat,
                        const unsigned char* source, std::size_t sourceBytes );

    /// Decode blocks back to @p destFormat — the inverse of the above, and the thing that MEASURES it.
    /// Implements the specification's own reconstruction (the integer interpolation, the unquantize and
    /// the BC6H finish-unquantize), so a disagreement between this and a GPU is a defect in one of them
    /// rather than a difference of opinion.
    [[nodiscard]] Common::ResultStr<std::vector<unsigned char>>
    BlockDecompressImage( uint32_t width, uint32_t height, ImageFormat blockFormat, ImageFormat destFormat,
                          const unsigned char* source, std::size_t sourceBytes );
} // namespace Desert::Core::Formats
