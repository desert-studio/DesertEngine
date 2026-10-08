#pragma once

#include <filesystem>
#include <string>

// Standalone utility (no engine dependencies — Assimp + STL only). The launcher will invoke this when
// installing a pack; for now it runs as a CLI tool.
namespace FbxSplit
{
    struct FbxSplitResult
    {
        bool        Success      = false;
        int         MeshCount    = 0;
        int         MaterialCount = 0; // Lit materials detected from texture files (by filename suffix)
        std::string ManifestPath; // collection.json written next to the FBX
        std::string Error;
    };

    // Splits EVERY mesh inside `fbxPath` (Blender often shows ~10) into a separate Wavefront .obj under
    // <Resources>/Mesh/<fbx-stem>/, then writes a `collection.json` next to the FBX listing them all (so the
    // engine's Collections panel shows each as its own card). The .obj sources cook on demand through the
    // normal engine pipeline. Node WORLD transforms ARE baked into each mesh (FBX keeps the real size/placement
    // there), then each mesh is recentered to the origin with its base at Y=0 — ideal for foliage placement.
    // `scale` is a uniform multiplier on the output geometry to resolve FBX unit ambiguity (e.g. 0.01 if a
    // cm-authored pack imports 100x too large); 1.0 keeps the authored scale.
    FbxSplitResult SplitFbxIntoMeshes( const std::filesystem::path& fbxPath, float scale = 1.0f );
} // namespace FbxSplit
