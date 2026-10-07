#pragma once

#include <filesystem>
#include <string>

namespace Desert::Editor
{
    // A texture reference as the source file states it, with its separators made generic: a Windows-authored
    // reference ("..\..\textures\foo.jpg", Poly Haven's FBX) is ONE filename to a POSIX std::filesystem::path,
    // so every '\' becomes '/' before anything (filename, stem, parent) is taken from it. A reference that is
    // already generic ("a/b.png") comes back unchanged.
    std::filesystem::path NormalizeTextureReference( std::string reference );

    // Finds the file a source material's texture reference names, relative to `basePath` (the folder of the
    // source file). The stored path can't be trusted (Sketchfab FBX often store an absolute build path,
    // relativized to a long "../../.../mnt/prod/.../foo.jpg" that escapes the project), so: try it literally,
    // then the FILENAME next to the source file and in a sibling "textures/" folder, then the same stem with
    // each extension of kTextureSourceExtensions (the gothic FBX asks for "..._nor_gl_4k.exr" but only the
    // .jpg ships). Returns {} when nothing is found.
    std::filesystem::path FindSourceTexture( const std::filesystem::path& basePath, const std::string& reference );
} // namespace Desert::Editor
