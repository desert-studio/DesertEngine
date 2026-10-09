#pragma once

// THE COMPONENT BLOCKS WHOSE WHOLE ON-DISK FORM IS THEIR REFLECTION — ONE LIST, THREE READERS.
//
// A block in this list is written by Reflection::SerializeReflected over one reflected struct and read back by
// Reflection::DeserializeReflected into it, with nothing else in between. That makes its canonical text a
// function of the struct alone, which three places need:
//
//   * ComponentRegistry registers a serializer per row (MakeReflected / MakeReflectedSelf), run by run;
//   * ECS::ReflectedComponents maps a component key to its reflected type for scripts;
//   * Tools/SceneMigrator rewrites every such block in the corpus into the saver's bytes (CanonicaliseScene),
//     so a scene saved with no edit is byte-identical to its file.
//
// THE LIST IS GENERATED (SCR-API-2c): a component states its row once, on its own struct, as
// COMPONENT( Key( "..." ), Block( Member ) | Whole, Run( ... ) ), and DesertHeaderTool emits
// Engine/Generated/ReflectedComponentBlocks.gen.hpp (ForEachReflectedComponentBlock, ordered by key). A new
// reflected component is one marker; there is no second list to keep in step.
//
// NOT IN THIS LIST, because their file form is NOT their reflection alone: every hand-written serializer in
// ComponentRegistry.cpp (StaticMesh, SkinnedMesh, InstancedStaticMesh, Material slots, Script, Landscape root
// and tile, Foliage, AnimGraph, CubeGridBlockout, the authored components, …) and UIRenderTexture, whose
// reflected block has its `ScenePath` replaced by a `Scene` {Guid, Path} reference on disk.
//
// ORDER IS NOT FORMAT. A record states its component keys sorted by key (EntitySerializer, through
// ComponentRegistry::InFileOrder), whatever order the serializers were registered in. The run a row names is
// LOAD order only: where among the hand-written serializers the row is registered, and so read.

#include <Engine/ECS/Components.hpp>

namespace Desert::Core::Serialize
{
    // Where a run is registered among the hand-written serializers (see ORDER IS NOT FORMAT above: load order only).
    enum class ReflectedBlockRun
    {
        ActorsAndUI,          // after the animation handlers, before UIRenderTexture
        UIAfterRenderTexture, // after UIRenderTexture, before CubeGridBlockout
        Landscape,            // between the Landscape root and its tiles
        SkyAndAtmosphere      // after the tiles, before Script
    };

    // A block that is one reflected `TData` member of `TComponent`.
    template <class TComponent, class TData>
    struct ReflectedMemberBlock
    {
        using Component = TComponent;
        using Data      = TData;

        const char* Key;      // the block's key in a record
        const char* TypeName; // the reflected type, as ReflectionRegistry names it
        TData TComponent::*Member;
        ReflectedBlockRun  Run;
    };

    // A block that is the WHOLE reflected component (Skybox).
    template <class TComponent>
    struct ReflectedWholeBlock
    {
        using Component = TComponent;
        using Data      = TComponent;

        const char*       Key;
        const char*       TypeName;
        ReflectedBlockRun Run;
    };
} // namespace Desert::Core::Serialize

// THE ROWS: generated from the COMPONENT(...) marker on each component struct (ReflectionMacros.hpp).
#include <Engine/Generated/ReflectedComponentBlocks.gen.hpp>
