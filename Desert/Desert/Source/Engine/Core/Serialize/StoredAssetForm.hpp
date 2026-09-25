#pragma once

#include <optional>
#include <string>

namespace Desert::Core::Serialize
{
    // HOW A REFERENCE IS SPELLED IN A FILE, once the registry has said WHICH file it is.
    //
    // Three forms cover every asset type this engine serializes, and each one is exactly what that
    // type's hand-written `ToPath` branch produced before T2.4 collapsed the twelve lookups into one.
    // They are kept apart rather than unified because the string in a `.desce` is a FORMAT: making all
    // twelve write the tagged key would be a better format and a corpus migration, and this slice is
    // not the one that does it. `Desert/Tests/Engine/AssetResolverCensus` asserts that every reflected
    // asset type appears below, so a new type still cannot be added without deciding.
    enum class StoredAssetForm
    {
        // `cooked:Textures/T.tex` — the tagged stable key, verbatim. The only form that can name a
        // file under EITHER content root, which is why the texture slot and the three service types
        // use it: a cooked texture lives under COOKED_PATH, a sibling of the assets root, where a
        // path made relative to the assets root comes out as `../Cooked/...` and falls back to the
        // absolute spelling, i.e. to a developer's home directory in a committed file.
        StableKey,
        // `Materials/M_Rock.demat` — relative to the assets root, which is where this kind of content
        // ships. A file OUTSIDE every root has no project-relative identity, so it keeps its own
        // spelling, exactly as the branches this replaces said in the same situation ("outside the
        // project — say so plainly").
        AssetsRelative,
        // The path as this machine spells it. Meshes and skyboxes, and it is what their branches did:
        // `GetMetadata().Filepath.string()`, ABSOLUTE with a project open. That is the same defect the
        // material branch was fixed for and it is still live for these two — named here rather than
        // changed, because changing it rewrites every scene that holds a mesh or a skybox.
        MachinePath,
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
