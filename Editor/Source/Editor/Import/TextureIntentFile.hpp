#pragma once

// THE AUTHORED HALF OF THE TEXTURE COOK — a `.detex` beside the source image, and nothing else.
//
// ── WHY BESIDE THE SOURCE AND NOT IN THE COOKED CONTAINER ────────────────────────────────────────
//
// `Cooked/` is derived data. Its documented remedy for every problem is deletion ("if the cook is
// deprecated the user just deletes it", owner decision 2026-09-22, quoted in `TextureBinary.hpp`), and
// this repository has already paid once for putting authored state there: `TextureImporter` used to
// read the asset HANDLE back out of an existing `.tex` in the name of back-compat, and its own comment
// now records that the branch "did not preserve compatibility, it preserved the DEFECT". An intent
// stored only in the cook is an intent a `rm -rf Cooked/` silently reverts to "nobody said", and the
// next cook would then compress a normal map on a measurement — which is the exact failure this whole
// field exists to make impossible.
//
// ── WHY A FILE PER SOURCE, WHEN THIS TREE REFUSED SIDECARS ONCE ──────────────────────────────────
//
// `Common/Utilities/ContentUpdate.hpp` says of its install record: "Not a sidecar per file either —
// that is Unity's `.meta` convention, which this engine does not have and should not acquire for one
// feature." That sentence is about an INSTALL RECORD and its argument is that a record of a pack
// belongs inside the pack; it is not a rule against per-asset authored settings, and the tree already
// has those — a mesh import writes a `.demat` per material and never clobbers it again, which is the
// same shape as this and the precedent it is built on.
//
// The two placements that were weighed against it and lost:
//
//  * A TABLE KEYED BY PATH, one per project or one per folder. Texture sources here are not in one
//    directory — they sit in `Textures/`, in `Meshes/` beside the models that use them and in
//    `Clouds/Layouts/` — so the table would name files it does not live with, and a renamed or moved
//    image loses its row silently. A row that goes missing reverts that texture to "nobody said",
//    which is the failure mode with no symptom.
//  * THE MATERIAL'S SLOT NAME, which is the only signal that exists today. It lives in the material,
//    and two materials may bind one texture in different slots; the answer would then depend on which
//    material was consulted. `BlockCompression.hpp` names this and it is why the field had to be new.
//
// ── WHY IT IS NOT AN ENGINE ASSET ────────────────────────────────────────────────────────────────
//
// Nothing resolves a `.detex` by handle, nothing references one, and the runtime never opens one: it
// is consumed entirely by the cook, at cook time, and what reaches the game is the `.tex` the cook
// wrote. So it gets no `ContentDir` row, no registry entry and no `AssetTypeID` — three mechanisms
// whose whole purpose is to let one file FIND another, for a file nothing needs to find.

#include <Engine/Core/Formats/TextureIntent.hpp>

#include <Common/Utilities/FileSystem.hpp>

#include <rflcpp/rfl.hpp>
#include <rflcpp/rfl/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace Desert::Editor
{
    /// The authored file's extension: `de` + the noun, which is this project's own convention
    /// (`.desce`, `.demat`, `.detheme`, `.derig`).
    ///
    /// IT IS NOT `.tex`, AND THE PAIR IS EASY TO CONFUSE, so it is worth saying which is which: `.tex`
    /// is the COOKED container under `Cooked/` and `.detex` is the AUTHORED statement beside the source
    /// under `Assets/`. They never live in the same directory, which is the second thing keeping them
    /// apart.
    inline constexpr std::string_view kTextureIntentExtension = ".detex";

    /// WHERE THE AUTHORED FILE FOR @p source LIVES: the same directory, the same stem. One function, so
    /// the cook and any editor that offers to write one cannot disagree about where to look.
    ///
    /// `replace_extension` and not string concatenation: a source named `T_Rock.Colour.png` has the
    /// stem `T_Rock.Colour`, and the two spellings differ there.
    [[nodiscard]] inline std::filesystem::path TextureIntentPath( const std::filesystem::path& source )
    {
        std::filesystem::path path = source;
        path.replace_extension( kTextureIntentExtension );
        return path;
    }

    /// THE FILE'S CONTENTS. One field today, and the struct exists rather than a bare string so that
    /// the day a second authored setting arrives (an sRGB marking is the obvious one) it is a field in
    /// a format people already have on disk rather than a second file.
    ///
    /// The intent travels as its NAME. See `kTextureIntentNames` for why a hand-edited file may not
    /// carry the number.
    struct TextureIntentFileData
    {
        std::string Intent;
    };

    /// WHAT THE COOK LEARNED BY LOOKING. Three outcomes, because "nobody authored one" and "somebody
    /// authored one I cannot read" must not collapse into the same behaviour: the first is an ordinary
    /// texture and the second is a person's instruction that went missing, and the cook's answer to
    /// them differs.
    enum class TextureIntentSource
    {
        /// No `.detex` exists. `Intent` is `Unspecified` and the cook falls back to measuring.
        NotAuthored,
        /// A `.detex` was read and names an intent this build knows.
        Authored,
        /// A `.detex` exists and could not be understood. `Intent` is `Unspecified` and `Problem` says
        /// why. The cook must NOT silently treat this as `NotAuthored`: somebody wrote the file, so
        /// falling back to a measurement is falling back past an instruction.
        Malformed,
    };

    struct TextureIntentRead
    {
        TextureIntentSource          Where  = TextureIntentSource::NotAuthored;
        Core::Formats::TextureIntent Intent = Core::Formats::TextureIntent::Unspecified;
        /// Empty unless `Where == Malformed`; then it is one sentence naming what was wrong.
        std::string Problem;
    };

    /// Every intent this build accepts, as one comma-separated clause. Quoted into the refusal so a
    /// typo in an authored file comes back with the vocabulary rather than with "invalid".
    [[nodiscard]] inline std::string TextureIntentVocabulary()
    {
        std::string all;
        for ( uint32_t i = 0; i < Core::Formats::kTextureIntentCount; ++i )
        {
            if ( !all.empty() )
                all += ", ";
            all += std::string( Core::Formats::kTextureIntentNames[i] );
        }
        return all;
    }

    /// Read the authored intent for @p source. Never throws, never fails: an unreadable or nonsensical
    /// file is an outcome with a sentence, not an error the caller has to unwrap, because the cook goes
    /// on to produce a correct uncompressed texture either way.
    [[nodiscard]] inline TextureIntentRead ReadTextureIntent( const std::filesystem::path& source )
    {
        const std::filesystem::path path = TextureIntentPath( source );

        std::error_code ec;
        if ( !std::filesystem::exists( path, ec ) )
            return TextureIntentRead{};

        const auto raw = Common::Utils::FileSystem::ReadFileContent( path );
        if ( !raw.IsSuccess() )
        {
            return TextureIntentRead{ TextureIntentSource::Malformed, Core::Formats::TextureIntent::Unspecified,
                                      "it exists but could not be read (" + raw.GetError() + ")" };
        }

        const auto parsed = rfl::json::read<TextureIntentFileData>( raw.GetValue() );
        if ( !parsed )
        {
            return TextureIntentRead{ TextureIntentSource::Malformed, Core::Formats::TextureIntent::Unspecified,
                                      "it is not readable as {\"Intent\": \"<name>\"}" };
        }

        const Core::Formats::TextureIntent intent = Core::Formats::TextureIntentFromName( parsed.value().Intent );
        if ( intent == Core::Formats::TextureIntent::Count )
        {
            return TextureIntentRead{ TextureIntentSource::Malformed, Core::Formats::TextureIntent::Unspecified,
                                      "it names the intent '" + parsed.value().Intent +
                                           "', which is not one of: " + TextureIntentVocabulary() };
        }

        // `Unspecified` WRITTEN OUT IS STILL AN AUTHORED ANSWER, and it is not the same as no file. It
        // is how a person says "I looked at this and it has no special meaning", which is worth being
        // able to say -- and the cook's log distinguishes the two, so the set of textures nobody has
        // ever considered stays countable.
        return TextureIntentRead{ TextureIntentSource::Authored, intent, {} };
    }
} // namespace Desert::Editor
