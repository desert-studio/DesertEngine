#pragma once

#include <Engine/Assets/Mesh/MeshAsset.hpp>

namespace Desert
{
    class Mesh;
}
namespace Desert::Core
{
    class Scene;
}
namespace Desert::ECS
{
    class Entity;
}

namespace Desert::Editor
{
    // The "Mesh" section of the Details panel: what the mesh IS (source file) plus the facts UE puts
    // next to a mesh — triangles / vertices, element (submesh) count, LOD levels and the level actually
    // being drawn, bounds extent in cm, UV channels, and whether the entity carries a collision shape —
    // with a per-element fold underneath. Shared by the static and skinned mesh widgets; each hands it
    // the runtime mesh it renders, so the numbers describe what the GPU really has.
    class MeshDetailsWidget final
    {
    public:
        struct Context
        {
            // Source asset (may be null: primitives and edited runtime meshes have no MeshAsset).
            Assets::Asset<Assets::MeshAsset> Asset;
            // The built GPU mesh. Null while it is still building — the section says so instead of guessing.
            const ::Desert::Mesh* RuntimeMesh = nullptr;
            // Owning entity: supplies the world transform (bounds in world size, LOD pick) and the collider.
            const ECS::Entity* Entity = nullptr;
            // Active camera source; without it the active-LOD readout is omitted rather than faked.
            const ::Desert::Core::Scene* Scene = nullptr;

            int ForcedLOD = -1; // component's Force LOD (-1 = auto)
            int LODBias   = 0;
        };

        static void Show( const Context& ctx );

    private:
        static void ShowMeshSection( const Context& ctx );
    };
} // namespace Desert::Editor
