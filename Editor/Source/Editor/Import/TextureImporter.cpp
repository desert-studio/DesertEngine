#include <variant>
#include "TextureImporter.hpp"

#include <mutex>
#include "CookPaths.hpp"
#include "CookedJsonWrite.hpp"

#include <Common/Core/Serialization/GlmReflection.hpp>

#include "TextureIntentFile.hpp"
#include <Engine/Assets/TextureSourceAsset.hpp>
#include <Common/Content/DerivedDataCache.hpp>
#include <Common/Utilities/PakFile.hpp>
#include <Editor/Import/TextureSourceFormats.hpp>

#include <Engine/Assets/Serialization/TextureBinary.hpp>
#include <Engine/Core/Formats/BlockCompression.hpp>
#include <Engine/Core/Formats/TextureIntent.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <cstring>

#include <stb_image/stb_image.h>

#include <algorithm>
#include <cctype>
#include <cmath>

#include <Common/Core/Constants.hpp>

namespace Desert::Editor
{
    namespace
    {
        namespace Fmt = Desert::Core::Formats;

        /// THE COOK'S BLOCK-ENCODING POLICY, VERSIONED. It goes into the container's `EncoderHash` and
        /// into the freshness comparison, so changing a threshold below RE-COOKS every texture instead
        /// of leaving the old ones encoded by the old rule with nothing to say so. Before this the
        /// freshness check compared the source hash and the two identity fields only, which was right
        /// while the cook had no settings of its own; it has some now.
        inline constexpr uint32_t kBlockEncoderVersion = Assets::kTextureBlockEncoderVersion;

        // ── TWO SOURCES FOR ONE ANSWER, AND WHAT HAPPENS WHEN THEY DISAGREE ──────────────────────
        //
        // `Docs/Textures/T1_BCN_MEASUREMENT.md` is binding and says three things about what may be
        // block-compressed: BC5 for normals and NEVER BC7; BC4 for single-channel masks; nothing block
        // at all for noise. All three need to know WHAT A TEXTURE IS FOR, which is not in the pixels.
        // The cook now has two independent answers to that question and uses both:
        //
        //   THE AUTHORED ONE decides WHICH format is attempted, or that none is. It is a `.detex`
        //   beside the source (`TextureIntentFile.hpp`) run through `BlockPolicyForIntent`.
        //
        //   THE MEASURED ONE decides whether the attempt is KEPT. The cook encodes, DECODES ITS OWN
        //   OUTPUT and compares over the channels the format promised, against the two gates below.
        //
        // NEITHER IS ALLOWED TO OVERRULE THE OTHER SILENTLY. Where they disagree the cook says so by
        // name, with both numbers, and the CONSERVATIVE side wins — an author's refusal stands over a
        // passing measurement, and a failing measurement stands over an author's request. The
        // disagreement is the whole value of having two sources; a cook that resolved it quietly would
        // be a cook with one source and a second opinion it throws away.
        //
        // THE MEASUREMENT IS TAKEN EVEN WHEN THE AUTHOR REFUSED, which costs a BC7 encode of a texture
        // that will not be stored as one. That is deliberate and it is the price of the second
        // direction: without it the cross-check could only ever fire when an author asked for
        // something, and "the author marked this Data and it compresses perfectly" — the likeliest
        // authoring mistake there is — would be invisible. It is the same encode this cook already
        // performed on every LDR texture before the field existed, so it is not new work in total.
        //
        // Measured in this tree (2026-09-23, the encoder in `Core/Formats/BlockCompression.cpp`, over the
        // WHOLE MIP CHAIN and with no `.detex` anywhere — i.e. the measurement acting alone):
        //
        // THE TABLE USED TO SAY "over the base level" AND THAT WAS WRONG ABOUT ITS OWN NUMBERS.
        // `MeasureLdrChain` walks every level and always has; the base-level figures for the same images
        // are the ones `Desert/Tests/Engine/BlockCompression`'s corpus prints, and they are DIFFERENT
        // (T_Checker 51.04 against 54.15, texture_normal 46.35 against 46.46). No conclusion below
        // moves — 45 dB and 32 sit in the gap on either measurement — but a label that names the wrong
        // measurement is how a threshold gets re-justified against numbers it was not fitted to.
        //
        //   texture_metallic 2048      57.78 dB   max|d|   3   kept
        //   texture_roughness 2048     54.72 dB   max|d|  11   kept
        //   T_Checker 1024             54.15 dB   max|d|   1   kept
        //   shaded 2048                53.39 dB   max|d|  10   kept
        //   texture_diffuse 2048       52.46 dB   max|d|  11   kept
        //   texture_pbr 2048           47.97 dB   max|d|  84   REFUSED by the delta gate
        //   texture_normal 2048        46.46 dB   max|d| 131   REFUSED by the delta gate
        //   1k_Dissolve_Noise 1024     37.83 dB   max|d|  10   REFUSED by the PSNR gate
        //
        // TWO GATES BECAUSE ONE WOULD MISS ONE OF THEM, and the table above is the argument: the noise
        // texture is the only one with a bad AVERAGE and it has a perfectly ordinary worst texel, while
        // the normal map has a respectable average and a worst texel half the range wide. A mean-only
        // gate ships the normal map; a worst-only gate ships the noise. T1 reached the same two
        // conclusions from a reference encoder (46.48 dB on the same normal map, 29.29 dB on noise),
        // which is what makes these thresholds a reproduction of its rule rather than a new opinion.
        //
        // The thresholds sit in the gaps, not at the measurements: 45 dB lies between noise (37.8) and
        // the lowest keeper (52.5 — and below the two the other gate rejects anyway), and 32 lies
        // between the worst keeper (11) and the packed-PBR map (84).
        //
        // AND SITTING IN A GAP IS EXACTLY WHY THE AUTHORED FIELD HAD TO EXIST. Both numbers are fitted
        // to eight images. A normal map that lands at 46 dB with a worst texel of 20 clears both and
        // is stored as BC7, which T1 forbids outright; nothing in this table can see that, because the
        // thing that separates a normal map from a colour map is not a number in it. The thresholds
        // protect the OUTPUT and the intent classifies the INPUT, and neither substitutes for the
        // other.
        inline constexpr double   kBlockPsnrFloorDb     = 45.0;
        inline constexpr uint32_t kBlockMaxDeltaCeiling = 32;

        struct Fidelity
        {
            double   Psnr             = 0.0;
            uint32_t MaxAbsoluteDelta = 0;
        };

        /// Decode @p blockPixels back and compare with the chain they were made from, OVER EVERY LEVEL
        /// AND OVER THE CHANNELS THE FORMAT PROMISED. The whole chain and not just the base, because
        /// the chain is what ships and because the smallest levels are where a block format's
        /// arithmetic differs at all — a 2x2 level is one block, and sixteen bytes describing four
        /// texels is the easy end, so measuring only level 0 would be measuring only the hard end and
        /// calling it the average.
        ///
        /// THE CHANNEL COUNT IS NOT A DETAIL, it is what made BC4 and BC5 measurable at all. This
        /// function used to walk the two buffers BYTE FOR BYTE, which is correct for BC7 — four
        /// channels in, four channels out — and produces near zero decibels for a PERFECT BC4 encode,
        /// because the green, blue and alpha it was never asked to keep come back as the zeros a
        /// sampler returns. A byte-wise comparison would therefore have refused every narrow format on
        /// every image, and the authored intent would have looked implemented while being unable to
        /// take effect once. `PreservedChannelCount` is the promise; this is where it is collected.
        Fidelity MeasureLdrChain( const Assets::Serialization::TextureAssetData&          source,
                                  const std::vector<Assets::Serialization::TextureLevel>& blockLevels,
                                  const std::vector<unsigned char>&                       blockPixels,
                                  const Fmt::ImageFormat                                  blockFormat )
        {
            Fidelity fidelity;
            double   squaredError = 0.0;
            uint64_t samples      = 0;

            // IT GRADES AN 8-BIT SOURCE AND NOTHING ELSE, and it says so rather than assuming it. Every
            // byte below is compared as an unsigned char and the PSNR is scaled by 255 squared, so
            // handing it a float chain would produce a number that looks like decibels and measures
            // nothing. The worst possible answer is returned, which is the one that cannot be mistaken
            // for a passing grade.
            if ( source.Format != Fmt::ImageFormat::RGBA8F )
            {
                fidelity.MaxAbsoluteDelta = 255;
                return fidelity;
            }

            // The source of an LDR cook is RGBA8; the decode comes back in the same format, so both
            // buffers are four bytes a texel and only the number of channels COMPARED differs.
            //
            // THROUGH THE BLOCK TABLE AND NOT THROUGH A 4. The source format's block is 1x1, so its
            // `Bytes` IS its bytes per texel — and asking the table is what keeps this line out of the
            // register of places that spell their own texel size, which is the defect
            // `kSkyEnvBytesPerPixel` was.
            const uint32_t sourceChannels = Fmt::GetTexelBlock( source.Format ).Bytes;
            const uint32_t channels       = std::min( Fmt::PreservedChannelCount( blockFormat ), sourceChannels );

            for ( std::size_t row = 0; row < blockLevels.size(); ++row )
            {
                const auto& level = blockLevels[row];
                auto decoded = Fmt::BlockDecompressImage( level.Width, level.Height, blockFormat, source.Format,
                                                          blockPixels.data() + level.ByteOffset,
                                                          static_cast<std::size_t>( level.ByteSize ) );
                if ( !decoded.IsSuccess() )
                {
                    fidelity.MaxAbsoluteDelta = 255; // unreadable output is the worst possible answer
                    return fidelity;
                }

                const auto&       original = source.Levels[row];
                const std::size_t texels   = static_cast<std::size_t>( level.Width ) * level.Height;
                for ( std::size_t texel = 0; texel < texels; ++texel )
                {
                    const std::size_t at = texel * sourceChannels;
                    if ( at + channels > decoded.GetValue().size() || at + channels > original.ByteSize )
                        break;
                    for ( uint32_t c = 0; c < channels; ++c )
                    {
                        const int delta = static_cast<int>( source.Pixels[original.ByteOffset + at + c] ) -
                                          static_cast<int>( decoded.GetValue()[at + c] );
                        squaredError += static_cast<double>( delta ) * delta;
                        fidelity.MaxAbsoluteDelta =
                             std::max( fidelity.MaxAbsoluteDelta, static_cast<uint32_t>( std::abs( delta ) ) );
                        ++samples;
                    }
                }
            }

            if ( samples == 0 )
                return fidelity;
            const double meanSquaredError = squaredError / static_cast<double>( samples );
            fidelity.Psnr = meanSquaredError <= 0.0 ? 1000.0 : 10.0 * std::log10( 65025.0 / meanSquaredError );
            return fidelity;
        }

        /// ENCODE ONE CHAIN INTO @p blockFormat AND GRADE IT, or say why it could not be encoded. One
        /// function because the three steps — encode, lay out, measure — only mean anything together,
        /// and because the cook now performs them for up to two formats on the same image and a second
        /// spelling of the sequence is a second place for the layout to drift.
        struct Attempt
        {
            bool                                             Encoded = false;
            std::string                                      Problem;
            Fidelity                                         Grade;
            std::vector<Assets::Serialization::TextureLevel> Levels;
            std::vector<unsigned char>                       Pixels;

            /// Does this encode clear BOTH gates? Asked in one place so the two thresholds cannot be
            /// compared with different operators at two call sites.
            [[nodiscard]] bool ClearsTheGates() const
            {
                return Encoded && Grade.Psnr >= kBlockPsnrFloorDb &&
                       Grade.MaxAbsoluteDelta <= kBlockMaxDeltaCeiling;
            }
        };

        Attempt EncodeAndGrade( const Assets::Serialization::TextureAssetData& data,
                                const Fmt::ImageFormat                         blockFormat )
        {
            Attempt attempt;

            const uint32_t levelCount = data.LevelCount();
            auto           blocks =
                 Assets::Serialization::BlockCompressChain( data.Width, data.Height, levelCount, data.LayerCount,
                                                            data.Format, blockFormat, data.Levels, data.Pixels );
            if ( !blocks.IsSuccess() )
            {
                attempt.Problem = blocks.GetError();
                return attempt;
            }

            auto table =
                 Assets::Serialization::BuildLevelTable( data.Width, data.Height, levelCount, data.LayerCount,
                                                         blockFormat, blocks.GetValue(), attempt.Pixels );
            if ( !table.IsSuccess() )
            {
                attempt.Problem = table.GetError();
                attempt.Pixels.clear();
                return attempt;
            }

            attempt.Levels  = table.ExtractValue();
            attempt.Grade   = MeasureLdrChain( data, attempt.Levels, attempt.Pixels, blockFormat );
            attempt.Encoded = true;
            return attempt;
        }

        /// THE COOK'S SIGNATURE FOR ONE TEXTURE: the policy version and the authored intent, in the one
        /// 64-bit word the container reserves for "was this made the way I am asking for it now".
        ///
        /// THE INTENT IS IN HERE AND THAT IS WHAT MAKES THE AUTHORED FILE TAKE EFFECT. Freshness is
        /// decided from the SOURCE IMAGE's bytes, and editing a `.detex` does not move one of them —
        /// so without this an artist could mark a texture as a normal map and the cook would answer
        /// "up to date" for ever, which is the same shape of invisible staleness the mtime comparison
        /// was removed for.
        ///
        /// `Unspecified` IS ZERO IN THE HIGH HALF, so the signature of an unauthored texture is exactly
        /// `kBlockEncoderVersion` — the number this field already held. Every `.tex` in the repository
        /// therefore still matches its own signature and nothing re-cooks on the day this lands.
        constexpr uint64_t CookSignature( const uint32_t version, const Fmt::TextureIntent intent )
        {
            return static_cast<uint64_t>( version ) | ( static_cast<uint64_t>( intent ) << 32 );
        }
    } // namespace

    TextureCookResult TextureImporter::Cook( const std::filesystem::path& path )
    {
        // Bulk cooking runs mesh imports in PARALLEL; two meshes often share textures, and two threads
        // writing the same cooked .tex would corrupt it. Texture cooking is cheap next to the Assimp
        // parse, so one global lock here is the simplest safe answer.
        static std::mutex           s_CookMutex;
        std::lock_guard<std::mutex> cookLock( s_CookMutex );

        auto abs = std::filesystem::weakly_canonical( path ).string();

        if ( m_Cache.contains( abs ) )
        {
            return { m_Cache[abs], TextureCookOutcome::Fresh };
        }

        // THE ASSET IS THE SOURCE OF TRUTH (AF3). A raw image handed in is IMPORTED first — its bytes go
        // inside `<stem>.detex` — and from then on only the asset is read: its header holds the handle, its
        // IMPT the provenance and settings, its SRCE the bytes the platform data is derived from.
        std::filesystem::path assetPath = path;
        if ( assetPath.extension() != Assets::kTextureAssetExtension )
        {
            auto imported = ImportSourceAsset( path );
            if ( !imported.IsSuccess() )
            {
                LOG_ERROR( "[TextureImporter] '{0}' could not be imported into a texture asset ({1}); no cooked "
                           "texture was written and the null handle is returned.",
                           abs, imported.GetError() );
                return { Common::AssetHandle::Null(), TextureCookOutcome::Failed };
            }
            assetPath = imported.GetValue();
        }
        const auto assetRead = Assets::ReadTextureSourceAssetFile( assetPath );
        if ( !assetRead.IsSuccess() )
        {
            LOG_ERROR( "[TextureImporter] '{0}' could not be read ({1}); no cooked texture was written and "
                       "the null handle is returned.",
                       assetPath.string(), assetRead.GetError() );
            return { Common::AssetHandle::Null(), TextureCookOutcome::Failed };
        }
        const Assets::TextureSourceAsset& asset     = assetRead.GetValue();
        const Common::UUID                handle    = asset.Handle();
        const std::string                 sourceKey = asset.Import.SourceFile;
        const std::string                 sourceBytesStorage( reinterpret_cast<const char*>( asset.Source.data() ),
                                                              asset.Source.size() );
        const uint64_t                    sourceHash = asset.Import.SourceHash;
        TextureIntentRead                 authored;
        authored.Intent = asset.Import.Settings.Intent;
        authored.Where  = authored.Intent == Fmt::TextureIntent::Unspecified ? TextureIntentSource::NotAuthored
                                                                             : TextureIntentSource::Authored;
        const Assets::TextureBuildSettings buildSettings{ asset.Import.Settings, kBlockEncoderVersion };
        const uint64_t                     ddcKey = Assets::TextureDerivedDataKey( sourceHash, buildSettings );
        const uint64_t                     cookSignature = CookSignature( kBlockEncoderVersion, authored.Intent );

        // A SOURCE FORMAT IS AN INPUT, NOT A STORAGE FORMAT — `Docs/Textures/T2_CONTAINER_DECISION.md`.
        // This is the ONE place in the project that decodes one, and everything downstream reads the
        // container this function writes.
        //
        // HDR SOURCES KEEP THEIR RANGE. The cooked file used to record `Format: RGBA8F` for every
        // source including `.hdr` — a field that was simultaneously wrong and unread, because the
        // loader sniffed the source file itself and decided again. The container's format field is the
        // answer now, so it has to be the true one.
        // THE DERIVED DATA CACHE (AF5). The platform data — mip chain and BC levels — is keyed by the source
        // bytes, the settings and the deriver's GUID, never by this asset's path. A hit skips the decode and
        // the encode entirely; the identity fields (handle, provenance) are per asset and live only in the
        // `.detex`. FRESH == THE DDC HOLDS THE KEY (AF3c). There is no per-asset cooked file any more: the runtime
        // reads the asset's header and asks the DDC (Assets::LoadTexturePlatformData), so an entry under
        // this key IS the texture's platform data, whichever asset with the same source put it there.
        if ( Common::DDC::Get( Assets::kTextureDeriver, ddcKey ).has_value() )
        {
            m_Cache[abs] = handle;
            Assets::ContentRegistry::NoteFile( assetPath );
            return { handle, TextureCookOutcome::Fresh };
        }
        const bool isHDR = stbi_is_hdr_from_memory( reinterpret_cast<const stbi_uc*>( sourceBytesStorage.data() ),
                                                    static_cast<int>( sourceBytesStorage.size() ) ) != 0;

        int                                w = 0, h = 0, ch = 0;
        std::vector<unsigned char>         base;
        Desert::Core::Formats::ImageFormat format = Desert::Core::Formats::ImageFormat::RGBA8F;

        if ( isHDR )
        {
            float* pixels =
                 stbi_loadf_from_memory( reinterpret_cast<const stbi_uc*>( sourceBytesStorage.data() ),
                                         static_cast<int>( sourceBytesStorage.size() ), &w, &h, &ch, 4 );
            if ( !pixels )
            {
                const char* reason = stbi_failure_reason();
                LOG_ERROR( "[TextureImporter] stbi_loadf failed for '{0}' ({1}); no cooked texture was "
                           "written and the null handle is returned.",
                           abs, reason ? reason : "no reason reported" );
                return { Common::AssetHandle::Null(), TextureCookOutcome::Failed };
            }
            format = Desert::Core::Formats::ImageFormat::RGBA32F;
            base.resize( static_cast<size_t>( w ) * h * 4u * sizeof( float ) );
            std::memcpy( base.data(), pixels, base.size() );
            stbi_image_free( pixels );
        }
        else
        {
            stbi_uc* pixels =
                 stbi_load_from_memory( reinterpret_cast<const stbi_uc*>( sourceBytesStorage.data() ),
                                        static_cast<int>( sourceBytesStorage.size() ), &w, &h, &ch, 4 );
            if ( !pixels )
            {
                // No `.tex` is written and the null handle is returned: a failed decode used to fall
                // through and freeze the UNINITIALIZED w/h into the cooked file, silently. The failure
                // is not cached either, so fixing the image and importing again works without
                // restarting the editor.
                const char* reason = stbi_failure_reason();
                LOG_ERROR( "[TextureImporter] stbi_load failed for '{0}' ({1}); no cooked texture was "
                           "written and the null handle is returned.",
                           abs, reason ? reason : "no reason reported" );
                return { Common::AssetHandle::Null(), TextureCookOutcome::Failed };
            }
            base.resize( static_cast<size_t>( w ) * h * 4u );
            std::memcpy( base.data(), pixels, base.size() );
            stbi_image_free( pixels );
        }

        // A source outside every content root has no project-relative name to store, so the key IS the
        // absolute spelling (StableKeyForPath's documented behaviour) and the cooked file is bound to this
        // machine. Say so once, at cook time, instead of letting the artist discover it on a colleague's
        // machine as an empty material slot.
        if ( !Common::AssetHandle::IsProjectRelativeKey( sourceKey ) )
        {
            LOG_WARN( "[TextureImporter] '{0}' lies outside every content root, so its texture asset "
                      "stores the absolute path and will not resolve on another machine.",
                      abs );
        }

        // THE ENTRY IS PURE DERIVED DATA (AF3e): no handle, no source path. It is keyed by content, so every
        // asset with these bytes and settings reads it, and an identity stamped here would be whichever
        // asset derived it first. Identity lives in the `.detex` header alone.
        Assets::Serialization::TextureAssetData data;
        data.SourceContentHash = sourceHash;
        data.Width             = static_cast<uint32_t>( w );
        data.Height            = static_cast<uint32_t>( h );
        data.Format            = format;

        // THE MIP CHAIN IS BUILT HERE, ON THE CPU, ONCE PER COOK. It used to be built on the GPU on
        // every load with `vkCmdBlitImage`, which is impossible for the block-compressed formats this
        // container exists to carry (`blitDst=0`) — so the chain has to be in the file before the
        // format can change, and that ordering is `Docs/World/PROGRAMME.md` §5.
        auto chain =
             Assets::Serialization::BuildMipChain( data.Width, data.Height, data.Format, base, data.Pixels );
        if ( !chain.IsSuccess() )
        {
            LOG_ERROR( "[TextureImporter] '{0}' was decoded but its mip chain could not be built: {1}. "
                       "The null handle is returned and nothing is cached.",
                       abs, chain.GetError() );
            return { Common::AssetHandle::Null(), TextureCookOutcome::Failed };
        }
        data.Levels      = chain.ExtractValue();
        data.Intent      = authored.Intent;
        data.EncoderHash = cookSignature;

        // ── THE TWO SOURCES MEET HERE ────────────────────────────────────────────────────────────
        //
        // See the note on `kBlockPsnrFloorDb` for the whole argument. In short: the AUTHORED intent
        // chooses the format, the MEASUREMENT decides whether that choice survives, and every
        // disagreement between them is a sentence in the log rather than a quiet resolution.
        //
        // ONLY AN 8-BIT COLOUR SOURCE IS OFFERED A BLOCK FORMAT AT ALL, and the guard is written against
        // the SOURCE FORMAT rather than against "the cook found some block format for it". HDR 2D
        // sources are rare here (the one in the tree is the sky panorama, which the environment only
        // ever reads at level 0 as the INPUT of its bake), and BC6H's measured win is the baked environment cube,
        // where it is applied — `EnvironmentBake.cpp`. A second BC6H call site here would be an encode with no
        // rendered frame to weigh it against, and `MeasureLdrChain` would grade it by walking two float buffers as
        // bytes, which is not a measurement of anything. `BlockPolicyForIntent` refuses an
        // extended-range source by name as well; that is the braces to this belt, and both are here
        // because the authored field made the old spelling of this guard (`blockFormat == BC7_UNORM`)
        // stop meaning what it said — three block formats are reachable from RGBA8 now.
        if ( data.Format == Fmt::ImageFormat::RGBA8F )
        {
            const Fmt::BlockPolicy policy = Fmt::BlockPolicyForIntent( authored.Intent, data.Format );

            // THE MEASUREMENT'S OWN VERDICT, TAKEN WHATEVER THE AUTHOR SAID. This is the second,
            // independent opinion; it is about the format the cook would reach for with nobody to ask,
            // which is what makes it comparable with the author's answer rather than derived from it.
            const Fmt::ImageFormat probeFormat = Fmt::BlockFormatFor( data.Format );
            Attempt                probe;
            if ( probeFormat != Fmt::ImageFormat::Count )
            {
                probe = EncodeAndGrade( data, probeFormat );
                if ( !probe.Encoded )
                {
                    // NOT FATAL, AND SAID OUT LOUD. An uncompressed cook is a correct cook; a cook that
                    // failed silently and produced a smaller file would not be.
                    LOG_WARN( "[TextureImporter] '{0}' could not be measured against {1}: {2}", abs,
                              static_cast<uint32_t>( probeFormat ), probe.Problem );
                }
            }

            // The encode that would actually be STORED. It is the probe itself whenever the author
            // asked for the same format the cook would have guessed, which is the common case and the
            // reason a `Colour` texture costs no more to cook than an unmarked one.
            Attempt chosen;
            bool    chosenIsProbe = false;
            if ( policy.Verdict == Fmt::BlockPolicyVerdict::Encode )
            {
                if ( policy.Format == probeFormat )
                {
                    chosenIsProbe = true;
                }
                else
                {
                    chosen = EncodeAndGrade( data, policy.Format );
                    if ( !chosen.Encoded )
                    {
                        LOG_WARN( "[TextureImporter] '{0}' is marked {1} and could not be encoded as the "
                                  "format that intent asks for: {2}",
                                  abs, Fmt::TextureIntentName( authored.Intent ), chosen.Problem );
                    }
                }
            }
            const Attempt& stored = chosenIsProbe ? probe : chosen;

            if ( authored.Where == TextureIntentSource::Malformed )
            {
                // AN INSTRUCTION WE COULD NOT READ IS NOT PERMISSION. Somebody wrote a `.detex` and the
                // cook does not know what it says; falling through to the measurement would be guessing
                // past a person, which is the one thing the authored field exists to stop. So the
                // texture is stored uncompressed — always correct, never silent — and the ERROR naming
                // the file was already logged above.
                LOG_WARN( "[TextureImporter] '{0}' is kept uncompressed because its authored intent could "
                          "not be read; the cook does not guess past a '{1}' that exists.",
                          abs, assetPath.filename().string() );
            }
            else if ( authored.Where != TextureIntentSource::Authored )
            {
                // NOBODY SAID. Exactly the behaviour this cook had before the field existed, and the
                // log line says on what authority — an unmarked texture is not a cross-checked one, and
                // the set of textures nobody has ever classified has to be readable from the cook's own
                // output rather than inferred from the absence of files.
                if ( probe.ClearsTheGates() )
                {
                    LOG_INFO( "[TextureImporter] '{0}' is stored as BC7 on a measurement alone ({1:.2f} dB, "
                              "worst texel off by {2}, {3} bytes instead of {4}): no intent is authored "
                              "for it, so there is nothing to cross-check the choice against. Put a "
                              R"('{{"Intent": "..."}}' in '{5}' to say what it is for.)",
                              abs, probe.Grade.Psnr, probe.Grade.MaxAbsoluteDelta, probe.Pixels.size(),
                              data.Pixels.size(), assetPath.filename().string() );
                    data.Format = probeFormat;
                    data.Levels = probe.Levels;
                    data.Pixels = std::move( probe.Pixels );
                }
                else if ( probe.Encoded )
                {
                    // THE REFUSAL NAMES THE TEXTURE AND BOTH NUMBERS. A texture that quietly did not
                    // get compressed is indistinguishable from one the cook forgot about, and the whole
                    // point of measuring is that somebody can read what was measured.
                    LOG_INFO( "[TextureImporter] '{0}' is kept uncompressed: BC7 gives {1:.2f} dB (floor "
                              "{2:.2f}) with a worst texel off by {3} (ceiling {4}).",
                              abs, probe.Grade.Psnr, kBlockPsnrFloorDb, probe.Grade.MaxAbsoluteDelta,
                              kBlockMaxDeltaCeiling );
                }
            }
            else if ( policy.Verdict != Fmt::BlockPolicyVerdict::Encode )
            {
                // THE AUTHOR REFUSED A BLOCK FORMAT. It stands, and the interesting case is when the
                // measurement would have allowed one: that is a genuine disagreement between the two
                // sources and it is the direction a cook with only a measurement can never report.
                if ( probe.ClearsTheGates() )
                {
                    LOG_WARN( "[TextureImporter] '{0}': the two sources DISAGREE. It is authored as {1}, "
                              "which forbids a block format ({2}), and the measurement says BC7 would "
                              "have given {3:.2f} dB with a worst texel off by {4} — inside both gates. "
                              "The authored refusal stands and the texture is stored uncompressed; if "
                              "the intent is wrong, '{5}' is where to change it.",
                              abs, Fmt::TextureIntentName( authored.Intent ), policy.Because, probe.Grade.Psnr,
                              probe.Grade.MaxAbsoluteDelta, assetPath.filename().string() );
                }
                else
                {
                    LOG_INFO( "[TextureImporter] '{0}' is kept uncompressed: it is authored as {1} ({2}), "
                              "and the measurement agrees — BC7 gives {3:.2f} dB with a worst texel off "
                              "by {4}.",
                              abs, Fmt::TextureIntentName( authored.Intent ), policy.Because, probe.Grade.Psnr,
                              probe.Grade.MaxAbsoluteDelta );
                }
            }
            else if ( stored.ClearsTheGates() )
            {
                LOG_INFO( "[TextureImporter] '{0}' is authored as {1} and stored as format {2} ({3}): "
                          "{4:.2f} dB, worst texel off by {5}, {6} bytes instead of {7}.",
                          abs, Fmt::TextureIntentName( authored.Intent ), static_cast<uint32_t>( policy.Format ),
                          policy.Because, stored.Grade.Psnr, stored.Grade.MaxAbsoluteDelta, stored.Pixels.size(),
                          data.Pixels.size() );
                data.Format = policy.Format;
                data.Levels = stored.Levels;
                // `stored` is a reference to one of two locals and one of them is still needed for the
                // sentence above, so the copy is taken rather than moved out of a const reference.
                data.Pixels = stored.Pixels;
            }
            else if ( stored.Encoded )
            {
                // THE OTHER DIRECTION OF DISAGREEMENT. The author asked for a format and the format
                // does not reproduce this image; compressing anyway is the silent ruin the whole field
                // exists to prevent, and refusing without saying so would hide an authoring mistake.
                LOG_WARN( "[TextureImporter] '{0}': the two sources DISAGREE. It is authored as {1}, "
                          "which asks for format {2} ({3}), and that encode measures {4:.2f} dB (floor "
                          "{5:.2f}) with a worst texel off by {6} (ceiling {7}). The measurement stands "
                          "and the texture is stored uncompressed.",
                          abs, Fmt::TextureIntentName( authored.Intent ), static_cast<uint32_t>( policy.Format ),
                          policy.Because, stored.Grade.Psnr, kBlockPsnrFloorDb, stored.Grade.MaxAbsoluteDelta,
                          kBlockMaxDeltaCeiling );
            }
        }

        // A HANDLE IS ONLY RETURNED FOR PLATFORM DATA THAT IS STORED. The DDC entry is the one place the
        // runtime finds it; a build that could not be Put would be a texture nobody can load.
        const std::string encoded = Assets::Serialization::EncodeTextureBinary( data );
        if ( const auto stored = Common::DDC::Put( Assets::kTextureDeriver, ddcKey, encoded ); !stored )
        {
            LOG_ERROR( "[TextureImporter] '{0}' was built but its platform data could not be stored in the DDC "
                       "({1}): {2}. The null handle is returned and nothing is cached.",
                       assetPath.string(), Common::DDC::PathFor( Assets::kTextureDeriver, ddcKey ).string(),
                       stored.GetError() );
            return { Common::AssetHandle::Null(), TextureCookOutcome::Unwritten };
        }

        m_Cache[abs] = handle;
        Assets::ContentRegistry::NoteFile( assetPath );
        return { handle, TextureCookOutcome::Cooked };
    }

    Common::ResultStr<std::filesystem::path>
    TextureImporter::ImportSourceAsset( const std::filesystem::path& source )
    {
        namespace fs             = std::filesystem;
        const fs::path assetPath = TextureIntentPath( source ); // `<stem>.detex` beside the file
        const auto     rawRead   = Common::Utils::FileSystem::ReadFileContent( source );
        if ( !rawRead.IsSuccess() )
            return Common::MakeError<fs::path>( rawRead.GetError() );
        const std::string&     raw = rawRead.GetValue();
        std::vector<std::byte> bytes( reinterpret_cast<const std::byte*>( raw.data() ),
                                      reinterpret_cast<const std::byte*>( raw.data() ) + raw.size() );
        const uint64_t         hash      = Common::Utils::PakContentHash( bytes.data(), bytes.size() );
        const std::string      sourceKey = Common::AssetHandle::StableKeyForPath( source );

        // REIMPORT: the asset exists. Identity (GUID -> handle) and settings are the asset's and are kept;
        // only a source whose CONTENT changed is taken in again. Same hash = nothing to do (no mtime).
        if ( Assets::IsTextureSourceAssetFile( assetPath ) )
        {
            auto existing = Assets::ReadTextureSourceAssetFile( assetPath );
            if ( !existing.IsSuccess() )
                return Common::MakeError<fs::path>( existing.GetError() );
            if ( existing.GetValue().Import.SourceHash == hash )
                return Common::MakeSuccess( assetPath );
            Assets::TextureSourceAsset updated = existing.ExtractValue();
            updated.Import.SourceFile          = sourceKey;
            updated.Import.SourceHash          = hash;
            updated.Source                     = std::move( bytes );
            if ( auto w = Assets::WriteTextureSourceAssetFile( assetPath, updated ); !w.IsSuccess() )
                return Common::MakeError<fs::path>( w.GetError() );
            return Common::MakeSuccess( assetPath );
        }

        // FIRST IMPORT. A fresh GUID is minted into the header; the handle is its fold (HandleForGuid), so
        // nothing about the source's place enters the identity. A legacy `{"Intent": ...}` sidecar at the asset's
        // path is the authored setting and is folded into ImportInfo; the asset replaces it.
        Assets::TextureImportSettings settings;
        const TextureIntentRead       authored = ReadTextureIntent( source );
        if ( authored.Where == TextureIntentSource::Authored )
        {
            settings.Intent = authored.Intent;
        }
        else if ( authored.Where == TextureIntentSource::Malformed )
        {
            // The old cook kept such a texture uncompressed rather than guess; `Data` is that same outcome
            // written down, so the choice survives the sidecar being replaced.
            LOG_ERROR( "[TextureImporter] '{0}' has an authored intent file that was not used: {1}. The asset "
                       "is imported as Data (uncompressed); set its intent to change that.",
                       assetPath.string(), authored.Problem );
            settings.Intent = Fmt::TextureIntent::Data;
        }
        const fs::path rel  = fs::relative( source, Common::Constants::Path::SKYBOX_PATH );
        const bool     sky  = !rel.empty() && rel.begin()->string() != "..";
        const auto     kind = sky ? Common::Content::ContentKind::Skybox : Common::Content::ContentKind::Texture;
        const Assets::TextureSourceAsset asset =
             Assets::MakeTextureSourceAsset( kind, sourceKey, std::move( bytes ), settings );
        if ( auto w = Assets::WriteTextureSourceAssetFile( assetPath, asset ); !w.IsSuccess() )
            return Common::MakeError<fs::path>( w.GetError() );
        return Common::MakeSuccess( assetPath );
    }

    Common::UUID TextureImporter::Import( const std::filesystem::path& path )
    {
        return Cook( path ).Handle;
    }

    std::array<const std::filesystem::path*, 2> LooseTextureRoots()
    {
        return { &Common::Constants::Path::TEXTUREDIR_PATH, &Common::Constants::Path::MESH_PATH };
    }

    std::vector<std::filesystem::path> LooseTextureSources()
    {
        namespace fs = std::filesystem;

        std::vector<fs::path> sources;
        for ( const fs::path* root : LooseTextureRoots() )
        {
            std::error_code ec;
            if ( !fs::exists( *root, ec ) )
                continue;

            for ( const auto& entry : fs::recursive_directory_iterator( *root, ec ) )
            {
                if ( !entry.is_regular_file() )
                    continue;

                std::string ext = entry.path().extension().string();
                std::transform( ext.begin(), ext.end(), ext.begin(), ::tolower );
                // THE ONE ORDERED LIST, asked rather than re-spelled -- see TextureSourceFormats.hpp for the
                // two hand-written copies that had already drifted before it existed.
                if ( ext == Assets::kTextureAssetExtension )
                {
                    // A legacy JSON sidecar is not an asset; its image is listed below and imports it.
                    if ( Assets::IsTextureSourceAssetFile( entry.path() ) )
                        sources.push_back( entry.path() );
                    continue;
                }
                if ( TextureSourceFormatRank( ext ) == kTextureSourceExtensionCount )
                    continue;
                // An image whose asset exists is that asset's provenance, not a second texture.
                if ( Assets::IsTextureSourceAssetFile( TextureIntentPath( entry.path() ) ) )
                    continue;

                sources.push_back( entry.path() );
            }
        }
        // Sorted, so two runs over the same tree cook — and log — in the same order on every platform.
        std::sort( sources.begin(), sources.end() );
        return sources;
    }

    std::filesystem::path TextureImporter::AssetPathFor( const std::filesystem::path& source )
    {
        return source.extension() == Assets::kTextureAssetExtension ? source : TextureIntentPath( source );
    }

    Common::ResultStr<std::string> TextureImporter::BuildPlatformData( const std::filesystem::path& asset )
    {
        TextureImporter importer;
        const auto      cooked = importer.Cook( asset );
        if ( cooked.Outcome == TextureCookOutcome::Failed || cooked.Outcome == TextureCookOutcome::Unwritten )
            return Common::MakeFormattedError<std::string>(
                 "texture asset '{}' could not be derived (the importer logged why)", asset.string() );
        const auto key = Assets::ReadTextureAssetKey( asset );
        if ( !key.IsSuccess() )
            return Common::MakeError<std::string>( key.GetError() );
        auto entry = Common::DDC::Get( Assets::kTextureDeriver, key.GetValue().DerivedDataKey );
        if ( !entry )
            return Common::MakeFormattedError<std::string>(
                 "texture asset '{}' was derived but its DDC entry {:016x} is not readable", asset.string(),
                 key.GetValue().DerivedDataKey );
        return Common::MakeSuccess( std::move( *entry ) );
    }

} // namespace Desert::Editor