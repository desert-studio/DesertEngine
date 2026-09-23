#pragma once

#include <Common/Core/Constants.hpp>

#include <filesystem>
#include <string>

// WHERE A TEXTURE SOURCE'S COOKED FORM LIVES — the ONE formula, in the engine and not in the editor.
//
// It used to be `Editor::CookPaths::CookedTexture`, and that was correct for as long as only the cooker
// needed it: the runtime found cooked textures through the registry, never by deriving a path. The sky
// panorama broke that. A `SkyboxAsset` names its `.hdr` SOURCE, and the environment used to decode that
// file right there in the draw layer (`Texture2D::Create(path)` -> stb). The decode is the cooker's job
// now, so the runtime has to find what the cooker wrote from the name it holds — and a second copy of
// this ladder on the engine side is exactly the drift `CookPaths.hpp` was created to end (it records
// "textures outside Resources/Textures silently failed to cook"). So the formula moved down a layer and
// the editor's `CookPaths::CookedTexture` forwards to it.
//
// PURE: no directory is created. The writer creates directories; a reader must not.
namespace Desert::Assets
{
    // Source texture -> Cooked/Textures/<rel>.<ext>. Textures under Resources/Assets/Textures keep their
    // layout (relative to that dir, so handles/paths are stable); textures ANYWHERE ELSE in the project's
    // assets root (images beside a mesh, a pack's Assets/Collections/<pack>/textures/) map under
    // `Assets/`, relative to that root; engine-resource images under Resources/ map relative to Resources/.
    // Otherwise the relative path escapes Cooked/Textures with "../" and the cook silently fails.
    //
    // WHY `Assets/` IS SPELLED HERE AND NOT READ OFF THE ROOT'S NAME. Until PK1 the middle case was "relative
    // to Resources/", which gave `Assets/Meshes/x.tex` in the sandbox only because its assets root happens to
    // be `Resources/Assets`. A project's root is not under Resources/ at all (`GameAssets/` beside the
    // `.deproj`), and neither is a package's (`Assets/` beside the archive), so an image beside a mesh
    // cooked to `Cooked/Textures/../Meshes/x.tex` — outside the tree the registry scans and the packager
    // packs. Measured by Desert/Tests/Editor/PackagedContent, TheTexturesAPackageCarriesAreCookedInsideIt,
    // the first run that cooked a mesh-side texture in a project. The fixed prefix keeps every sandbox cook
    // exactly where it was and gives every other layout the same answer.
    inline constexpr const char* kCookedAssetsSubdir = "Assets";

    inline std::filesystem::path CookedTexturePath( const std::filesystem::path& source, const std::string& ext )
    {
        namespace fs = std::filesystem;

        const auto inside = []( const fs::path& rel ) { return !rel.empty() && rel.begin()->string() != ".."; };

        fs::path rel = fs::relative( source, Common::Constants::Path::TEXTUREDIR_PATH );
        if ( !inside( rel ) )
        {
            if ( const fs::path relAssets = fs::relative( source, Common::Constants::Path::ASSETS_PATH );
                 inside( relAssets ) )
            {
                rel = fs::path( kCookedAssetsSubdir ) / relAssets;
            }
            else if ( const fs::path relRes = fs::relative( source, Common::Constants::Path::RESOURCE_PATH );
                      inside( relRes ) )
            {
                rel = relRes;
            }
        }

        fs::path result = Common::Constants::Path::TEXTURE_PATH_COOKED / rel;
        result.replace_extension( ext );
        return result;
    }
} // namespace Desert::Assets
