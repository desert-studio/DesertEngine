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
    // layout (relative to that dir, so handles/paths are stable); textures ANYWHERE ELSE under Resources/
    // (e.g. a pack's Assets/Collections/<pack>/textures/) map relative to Resources/ instead — otherwise the
    // relative path escapes Cooked/Textures with "../" and the cook silently fails.
    inline std::filesystem::path CookedTexturePath( const std::filesystem::path& source, const std::string& ext )
    {
        namespace fs = std::filesystem;

        fs::path   rel      = fs::relative( source, Common::Constants::Path::TEXTUREDIR_PATH );
        const bool underTex = !rel.empty() && rel.begin()->string() != "..";
        if ( !underTex )
        {
            const fs::path relRes = fs::relative( source, Common::Constants::Path::RESOURCE_PATH );
            if ( !relRes.empty() && relRes.begin()->string() != ".." )
                rel = relRes;
        }

        fs::path result = Common::Constants::Path::TEXTURE_PATH_COOKED / rel;
        result.replace_extension( ext );
        return result;
    }
} // namespace Desert::Assets
