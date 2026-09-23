#pragma once

// WHAT A TEXTURE IS FOR — the one thing about a texture that is not in its pixels.
//
// ── WHY MEASURING WAS NOT ENOUGH, THOUGH IT IS KEPT ──────────────────────────────────────────────
//
// The cook can already establish, by encoding and decoding its own output, whether a block format
// reproduces a given image. That is a fact about pixels and it is a good one; `TextureImporter.cpp`
// records the eight measurements it was derived from. What it CANNOT establish is which of two images
// with the same statistics is a normal map, because that is not a property of the numbers. The gates
// live at 45 dB and a worst texel of 32, and today's corpus leaves those thresholds sitting in gaps
// (38.7 <-> 51.6 dB, 15 <-> 84). New content lands in gaps, and the failure there is SILENT: a normal
// map that happens to score 46 dB is compressed as BC7 and ruins lighting somewhere nobody is looking.
//
// So this is the second, INDEPENDENT source of the same answer. It is the pattern UE uses — its
// `TextureCompressionSettings` is an authored property and not a cook inference — and the reason is the
// same one. Two sources that can disagree are stronger than either alone, and the DISAGREEMENT is the
// signal: `TextureImporter` says so out loud rather than silently compressing or silently refusing.
//
// ── THE DECOMPOSITION, AND WHAT IS DELIBERATELY NOT IN IT ────────────────────────────────────────
//
// Five enumerators, and each one exists because something DOWNSTREAM branches on it — the three rules
// `Docs/Textures/T1_BCN_MEASUREMENT.md` binds the cook to, plus "nobody said". There is no `HDR`
// enumerator and there must not be one: whether a source decodes as extended range is something the
// cook already knows (`stbi_is_hdr_from_memory`), so an authored HDR would be a second answer to a
// question that has one, and the first time the two disagreed there would be nothing to decide between
// them. UE has `TC_HDR` because ITS field also selects the source's range; ours does not, so this is a
// place to take the pattern and not the letter.
//
// Equally there is no `sRGB` bit here. Colour space is a property with its own consumer (the sampler's
// view format) and its own migration, and folding it into this enum would make one field answer two
// questions — which is the defect `AssetHandle` spent a day being cured of.
//
// ── WHERE IT IS AUTHORED ─────────────────────────────────────────────────────────────────────────
//
// In a `.detex` beside the source image, NOT in the cooked `.tex`. The cooked container RECORDS the
// intent it was cooked with — that is what makes a refusal reproducible from the file — but it cannot
// be the authority, because `Cooked/` is derived data whose documented remedy is deletion, and because
// this repository has already paid for the other shape once: `TextureImporter` used to read the asset
// HANDLE back out of an existing `.tex`, and that "back-compat" branch preserved the defect rather than
// the compatibility (its own comment, still in the file). Authored data read out of the cook's previous
// output is authored data that a `rm -rf Cooked/` destroys.

#include <Engine/Core/Formats/ImageFormat.hpp>

#include <cstdint>
#include <string_view>

namespace Desert::Core::Formats
{
    /// WHAT THIS TEXTURE IS FOR. Authored; the cook never invents one.
    ///
    /// The numbers are part of the file format (`TextureBinary`'s header carries one), so an enumerator
    /// may be APPENDED and none may be renumbered. `Unspecified` is 0 for the reason the container's
    /// note gives: every `.tex` ever written has zero in that word already, so an old file and a new
    /// unauthored one decode to the same thing and are byte-for-byte identical.
    enum class TextureIntent : uint32_t
    {
        /// NOBODY SAID. The cook falls back to measuring, exactly as it did before this field existed,
        /// and says at the log that the format it chose rests on a measurement alone. It is a real
        /// state and not a hole: most textures in a project never need an opinion, and pretending the
        /// author supplied one would make the cross-check compare a guess with itself.
        Unspecified = 0,

        /// Something a human looks at as colour: albedo, UI art, a checker. BC7 is what T1 measures it
        /// at and what the gates then confirm.
        Colour = 1,

        /// A TANGENT-SPACE NORMAL MAP. The rule T1 binds the cook to is BC5 and NEVER BC7 — 52.51 dB
        /// against 46.48 on this project's own normal map, at a hundredth of the encode time — and it
        /// is a rule about the CONTENT, which is why it needs this field and cannot be measured.
        NormalMap = 2,

        /// ONE meaningful channel: an opacity cutout, a metallic or roughness map, an AO map. BC4.
        /// The channels beyond the first are not preserved, which is exactly why this may not be
        /// inferred: an image whose G and B happen to equal its R is indistinguishable from a colour
        /// map by measurement, and only the author knows which one it is.
        Mask = 3,

        /// PACKED OR NON-VISUAL CHANNELS — an ORM map whose three channels are three unrelated
        /// quantities, a noise field, a lookup table. NEVER block-compressed: a block format fits a
        /// SHARED endpoint line through a 4x4 neighbourhood, which is the one assumption packed
        /// channels break. T1 measured 29.29 dB on this project's noise texture and it was the single
        /// substitution visible in a rendered frame.
        Data = 4,

        /// Not an intent. Every real one goes above this line and the count below is derived from it.
        Count
    };

    /// Derived, never written down.
    inline constexpr uint32_t kTextureIntentCount = static_cast<uint32_t>( TextureIntent::Count );

    /// THE ONE SPELLING OF EACH INTENT'S NAME, index-matched to the enumerator. It is what a `.detex`
    /// carries and what every refusal message quotes, so a person editing the authored file and a
    /// person reading the log see the same word.
    ///
    /// A NAME AND NOT THE NUMBER, in the authored file. The number is a file-format detail of the
    /// cooked container; the authored file is written and read by people, and a hand-edited `3` that
    /// means `Mask` today would mean something else the day an enumerator is inserted. The container
    /// may carry the number because the container is rewritten whenever the enum moves.
    inline constexpr std::string_view kTextureIntentNames[] = {
         "Unspecified", "Colour", "NormalMap", "Mask", "Data",
    };
    static_assert( sizeof( kTextureIntentNames ) / sizeof( kTextureIntentNames[0] ) == kTextureIntentCount,
                   "every TextureIntent needs a name, and the sentinel needs none" );

    /// Is @p value one this build knows? Asked by the container's decoder about a word it read out of a
    /// file, so it takes the raw integer rather than the enum.
    [[nodiscard]] constexpr bool IsKnownTextureIntent( const uint32_t value )
    {
        return value < kTextureIntentCount;
    }

    [[nodiscard]] constexpr std::string_view TextureIntentName( const TextureIntent intent )
    {
        return IsKnownTextureIntent( static_cast<uint32_t>( intent ) )
                    ? kTextureIntentNames[static_cast<uint32_t>( intent )]
                    : std::string_view( "<not an intent>" );
    }

    /// Parse an authored name. Returns `TextureIntent::Count` for a word this build does not know,
    /// which the reader turns into a refusal that LISTS the vocabulary — a typo in an authored file has
    /// to be a sentence somebody can act on, not a silent fall back to the default. Case-sensitive on
    /// purpose: there is one spelling and the table above is it.
    [[nodiscard]] constexpr TextureIntent TextureIntentFromName( const std::string_view name )
    {
        for ( uint32_t i = 0; i < kTextureIntentCount; ++i )
        {
            if ( kTextureIntentNames[i] == name )
                return static_cast<TextureIntent>( i );
        }
        return TextureIntent::Count;
    }

    /// WHY A TEXTURE IS OR IS NOT BLOCK-COMPRESSED. Three outcomes and not a bool, because "the author
    /// said no" and "this build has nothing to encode it with" are different facts that need different
    /// sentences — the first is a decision that stands, the second is a gap that will close.
    enum class BlockPolicyVerdict
    {
        /// Encode into `Format` and then measure it.
        Encode,
        /// The intent forbids a block format. `Data`, and it is the whole point of that enumerator.
        RefusedByIntent,
        /// The intent names a block format this build cannot produce. No such intent exists today; the
        /// enumerator stays because the shape of the answer must survive an encoder being removed as
        /// well as added, and because a policy that could only ever say yes-or-refused would have to
        /// LIE about a missing encoder by calling it a refusal.
        NoEncoderYet,
    };

    /// WHAT THE AUTHORED INTENT ASKS FOR, given the format the source decoded to.
    ///
    /// THE POLICY LIVES HERE AND NOWHERE ELSE. `BlockCompression.hpp` deliberately does not carry it —
    /// that file knows how to turn pixels into blocks and has no business knowing which textures may
    /// become which — and the cook does not carry it either, because a rule spelled at the call site is
    /// a rule the suite has to reach through an importer to test. This is a pure function of two
    /// arguments and the suite asks it directly.
    ///
    /// `Unspecified` NEVER REACHES HERE. It is not a request for a format, it is the absence of one,
    /// and the cook's fall-back-to-measurement branch is a different piece of code with a different log
    /// line. Asking this function about it would force an answer that looks authored.
    struct BlockPolicy
    {
        BlockPolicyVerdict Verdict = BlockPolicyVerdict::RefusedByIntent;
        /// Valid only when `Verdict == Encode`; `ImageFormat::Count` otherwise.
        ImageFormat Format = ImageFormat::Count;
        /// One clause, quoted into the cook's log beside the texture's name. Present for every verdict.
        std::string_view Because;
    };

    [[nodiscard]] constexpr BlockPolicy BlockPolicyForIntent( const TextureIntent intent,
                                                              const ImageFormat   sourceFormat )
    {
        // AN EXTENDED-RANGE SOURCE IS NOT OFFERED A BLOCK FORMAT BY THIS POLICY, whatever the intent
        // says. The one BC6H producer in this engine is the environment bake, which weighs its own
        // substitution against a rendered sky; a second one reached through an authored field would be
        // an encode nothing has ever compared a frame against. The bulk cook does not reach an `.hdr`
        // at all (`TextureSourceFormats.hpp` excludes the group), so this is the belt to that braces.
        if ( sourceFormat != ImageFormat::RGBA8F )
        {
            return BlockPolicy{ BlockPolicyVerdict::RefusedByIntent, ImageFormat::Count,
                                "the source is not 8-bit colour, and the only extended-range encoder in "
                                "this engine is the environment bake's" };
        }

        switch ( intent )
        {
            case TextureIntent::Colour:
                return BlockPolicy{ BlockPolicyVerdict::Encode, ImageFormat::BC7_UNORM,
                                    "colour is what BC7 is for" };
            case TextureIntent::NormalMap:
                return BlockPolicy{ BlockPolicyVerdict::Encode, ImageFormat::BC5_UNORM,
                                    "T1 binds a normal map to BC5 and forbids BC7 on one" };
            case TextureIntent::Mask:
                return BlockPolicy{ BlockPolicyVerdict::Encode, ImageFormat::BC4_UNORM,
                                    "a mask is one channel, and BC4 stores one channel in half a BC7 "
                                    "block" };
            case TextureIntent::Data:
                return BlockPolicy{ BlockPolicyVerdict::RefusedByIntent, ImageFormat::Count,
                                    "packed or non-visual channels do not share an endpoint line, which "
                                    "is the one thing every block format assumes" };
            case TextureIntent::Unspecified:
            case TextureIntent::Count:
                break; // neither is a request for a format -- see the note above
        }

        return BlockPolicy{ BlockPolicyVerdict::RefusedByIntent, ImageFormat::Count,
                            "no intent was authored, so nothing was asked for" };
    }

    namespace Detail
    {
        /// The same shape of guard `ImageFormat`'s `LookupsAreTotal` uses, and for the same reason: an
        /// intent added without a case falls through to the "nothing was asked for" answer, which is a
        /// PLAUSIBLE one and would ship. Constant-evaluating every enumerator makes the omission a
        /// build failure instead.
        constexpr bool EveryIntentIsAnswered()
        {
            for ( uint32_t i = 0; i < kTextureIntentCount; ++i )
            {
                const TextureIntent intent = static_cast<TextureIntent>( i );
                if ( TextureIntentName( intent ).empty() )
                    return false;
                if ( TextureIntentFromName( TextureIntentName( intent ) ) != intent )
                    return false;
                if ( intent == TextureIntent::Unspecified )
                    continue;

                const BlockPolicy policy = BlockPolicyForIntent( intent, ImageFormat::RGBA8F );
                if ( policy.Because.empty() )
                    return false;
                // The relation, not the value: a verdict of Encode must carry a real format and every
                // other verdict must carry none. A policy that said "encode into Count" would be read
                // as a format by the one caller that matters.
                if ( ( policy.Verdict == BlockPolicyVerdict::Encode ) == ( policy.Format == ImageFormat::Count ) )
                    return false;
            }
            return true;
        }
    } // namespace Detail

    static_assert( Detail::EveryIntentIsAnswered(),
                   "Every TextureIntent needs a name that round-trips and a BlockPolicyForIntent case "
                   "whose verdict and format agree." );
} // namespace Desert::Core::Formats
