#pragma once

// WHAT ONE COOK OF A MESH SOURCE DID. The editor cooks a source's derived mesh only when a load asks for it
// and finds no DDC entry for the source's current bytes (Assets::SetMeshSourceBuilder -> BuildMeshSourceOnMiss,
// UE: a DDC miss rebuilds from the package); nothing is cooked at startup. The envelope is a DDC entry keyed by
// the source file's own bytes (Assets::kMeshSourceDeriver), so an entry for the current bytes is fresh by
// construction, and the DDC write is content-addressed and atomic (DDC::Put -> WriteContentToFileAtomic).

namespace Desert::Editor
{
    enum class CookVerdict
    {
        UpToDate, // the cache already held the entry for the source's current bytes; nothing was built
        Cooked,   // the entry was built and written
        Failed,   // the source was parsed or read and no complete entry reached the cache
        NotCookable,
    };
} // namespace Desert::Editor
