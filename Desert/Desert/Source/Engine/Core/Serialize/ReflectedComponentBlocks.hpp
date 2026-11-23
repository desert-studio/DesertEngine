#pragma once

// THE COMPONENT BLOCKS WHOSE WHOLE ON-DISK FORM IS THEIR REFLECTION — ONE LIST, THREE READERS.
//
// A block in this list is written by Reflection::SerializeReflected over one reflected struct and read back by
// Reflection::DeserializeReflected into it, with nothing else in between. That makes its canonical text a
// function of the struct alone, which these places need:
//
//   * the ENGINE does not include this list (BUILD-MOD-5): the same marker also generates one
//     RegisterComponents_<Module>() per module (Engine/Generated/Components_<Module>.gen.cpp), which hands its
//     rows to ComponentRegistration.hpp -- ComponentRegistry, ECS::ReflectedComponents and SaveGame read those,
//     so a field added to a module's component recompiles that module's file, not the engine;
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

#include <Engine/Core/Serialize/ReflectedComponentRows.hpp>

// THE ROWS: generated from the COMPONENT(...) marker on each component struct (ReflectionMacros.hpp).
#include <Engine/Generated/ReflectedComponentBlocks.gen.hpp>
