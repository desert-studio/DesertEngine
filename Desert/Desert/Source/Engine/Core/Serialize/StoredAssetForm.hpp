#pragma once

#include <optional>
#include <string>

namespace Desert::Core::Serialize
{
    // HOW A REFERENCE IS SPELLED IN A FILE, once the registry has said WHICH file it is.
    //
    // Three forms cover every asset type this engine serializes, and each one is exactly what that
    // type's hand-written `ToPath` branch produced before T2.4 collapsed the twelve lookups into one.
    // The assets-relative spelling (`Materials/M_Rock.demat`) that once stood between the two is GONE
    // (PATH1): the corpus was restated in the tagged key and its reader deleted, so every reference that
    // can name a file outside the assets root says which root it is under. `Desert/Tests/Engine/AssetResolverCensus` asserts that every reflected
    // asset type appears below, so a new type still cannot be added without deciding.
    enum class StoredAssetForm
    {
        // `assets:Textures/T.tex` — the tagged stable key, verbatim. The only form that can name a
        // file under EITHER content root, which is why the texture slot and the three service types
        // use it: an engine texture lives under RESOURCE_PATH, outside the assets root, where a
        // path made relative to the assets root comes out as a `..` chain and falls back to the
        // absolute spelling, i.e. to a developer's home directory in a committed file.
        StableKey,
        // `assets:Textures/HDR/Sky.detex` — the tagged key and NOTHING ELSE: a file outside every content
        // root renders as "" (the resolver names it), so this form can never carry a machine path. Skyboxes
        // (SCNE 29), whose path is only the locator beside the header GUID.
        ProjectKey,
    };

    // Which form `type`'s references are stored in, or std::nullopt for a type this engine cannot
    // write down. The nullopt is what `MakeAssetResolver::ToPath` turns into one loud line naming the
    // type that needs a row; `Desert/Tests/Engine/AssetResolverCensus` catches the same omission
    // earlier, at the moment the field is declared.
    [[nodiscard]] std::optional<StoredAssetForm> StoredFormFor( const std::string& type );

    // Renders the stable key `key` in `form` — the string that actually goes into the file.
    [[nodiscard]] std::string RenderStoredForm( StoredAssetForm form, const std::string& key );
} // namespace Desert::Core::Serialize
