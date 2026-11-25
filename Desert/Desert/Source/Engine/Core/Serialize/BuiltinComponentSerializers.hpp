#pragma once

// THE HAND-WRITTEN COMPONENT SERIALIZERS, declared (BUILD-MOD-5). Their file form is NOT their reflection alone,
// so they are not generated rows (ReflectedComponentBlocks.hpp says which are). Defined by subject beside this
// header (Mesh / Actor / World ComponentSerializers.cpp; a feature module's own through
// EngineComponentProvider.hpp, ScriptComponentSerializer.cpp); ComponentRegistry.cpp calls them in load order
// between the generated runs. Each Add* appends its blocks to `out` in their order.

#include <Engine/Core/Serialize/ComponentRegistry.hpp>

#include <vector>

namespace Desert::Core::Serialize::Builtins
{
    // StaticMesh, InstancedStaticMesh, Material, SkinnedMesh.
    void AddMeshSerializers( std::vector<ComponentSerializer>& out );
    // UIAnim, Text, Animation.
    void AddActorSerializers( std::vector<ComponentSerializer>& out );
    // UIRenderTexture (its ScenePath stored as a {Guid, Path} scene reference).
    void AddUIRenderTextureSerializer( std::vector<ComponentSerializer>& out );
    // CubeGridBlockout, Folder, Lock, AlwaysLoaded, Visibility, LevelSequence.
    void AddEditorAndSequenceSerializers( std::vector<ComponentSerializer>& out );
    // ProceduralFoliageField, Foliage.
    void AddFoliageSerializers( std::vector<ComponentSerializer>& out );
    // Morph, SocketAttachment.
    void AddAuthoredSerializers( std::vector<ComponentSerializer>& out );

    ComponentSerializer MakeScript();
} // namespace Desert::Core::Serialize::Builtins
