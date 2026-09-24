#pragma once

#include <cstddef>
#include <string>

namespace Desert::Editor
{
    // What one cook pass did — for the packager's summary line and for tests.
    struct CookStats
    {
        size_t ShadersCompiled = 0; // stages actually compiled this pass
        size_t ShadersCached   = 0; // stages already present under their key
        size_t FontsBaked      = 0;
        size_t FontsCached     = 0;
        size_t IconsBaked      = 0;
        size_t IconsCached     = 0;
        size_t TexturesCooked  = 0; // `.tex` containers written this pass
        size_t TexturesCached  = 0; // already cooked from the same source bytes by the same rules
        size_t Failures        = 0; // parse/compile/bake failures (each logged where it happened)
        // Artifacts that were produced and then did NOT reach the disk. Counted apart from Failures
        // because they mean something different: a compile failure is content that is already broken
        // (the runtime reports it too, and a project may legitimately ship one — the shader-error
        // fixture does), while an unwritten artifact is a cook that silently shipped nothing under a
        // key the runtime will ask for, which is П2 happening again one file at a time.
        size_t StoreFailures = 0;
    };

    // Pays, ONCE and at packaging time, every deterministic startup cost the runtime would otherwise
    // pay on the player's machine: compiles every stage of every pass of every shipped .shader,
    // bakes the default-size ASCII atlas of every shipped .ttf, and bakes the SDF layers of every
    // shipped .svg — each into the DerivedDataCache (Common/Content/DerivedDataCache.hpp) through the
    // exact key/path/store seams the runtime reads back (ShaderSpirvCache, Text/FontCache,
    // Vector/IconBake). The buckets a game reads — and the driver's pipeline blobs — are then COPIED into
    // Saved/Cooked/<Platform>/, wiped first so a package never carries a previous cook's leftovers, and
    // that directory is the census tree packed under "Cooked" (PackagedContentTrees.hpp). The DDC itself
    // never ships and the editor never reads Saved/Cooked.
    //
    // AND IT COOKS THE TEXTURES — every source under `LooseTextureRoots()`, through the editor's own
    // `TextureImporter` (not a copy of it), into Cooked/Textures/. A texture is the one asset the runtime
    // CANNOT produce for itself: it holds no image decoder (T3.3 removed the last one, the sky
    // panorama's), so a package without the `.tex` is a floor with no checkerboard and a sky with no
    // panorama, with nothing on the player's machine able to repair it. Before this, a package carried
    // whichever `.tex` files an editor session had happened to leave on the packaging machine — in CI,
    // one committed file. The rows the texture cook enters are then written to the cook's registry
    // (`AssetRegistry.cooked.dreg`), which ships in the same tree: the runtime finds textures through
    // the registry and not by walking a directory.
    //
    // Incremental by construction: an artifact already present under its key is not rebuilt (a
    // texture's key is its source's bytes plus the cook's rules, see TextureImporter::Cook).
    //
    // `spirvDebugInfo` is the TARGET runtime's profile (Core::SpirvDebugInfoForConfigName), not this
    // editor's: a Debug editor packaging a Release game must cook Release keys, or the shipped cache
    // is dead on arrival.
    CookStats CookContentCaches( bool spirvDebugInfo );
} // namespace Desert::Editor
