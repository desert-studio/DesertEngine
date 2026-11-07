#pragma once

#include <Common/Core/UUID.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace Desert::Core
{
    class Scene;
}
namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
    // In-editor rigging session. While active it holds the skeleton the user is placing onto ONE static-mesh
    // entity (bone heads in the mesh's LOCAL space). "Convert to Skinned" turns that static mesh + these bones
    // into a runtime SkinnedMesh (CPU auto-weighted via Geometry::AutoSkinVertices) and swaps the entity's
    // StaticMeshComponent for a SkinnedMeshComponent carrying the built mesh. A single session at a time.
    //
    // The swap must not run mid-widget-render (it removes the very component being drawn), so the UI calls
    // RequestConvert() and EditorLayer runs ProcessPending() once per frame, outside component iteration.
    class RigBuilder
    {
    public:
        struct Bone
        {
            std::string Name;
            glm::vec3   Head{ 0.0f }; // mesh-local joint position
            int         Parent = -1;  // index into Bones(); -1 = root
        };

        static bool                     IsActive();
        static const Common::UUID&      Target(); // active entity (0/invalid when inactive)
        static const std::vector<Bone>& Bones();
        static int                      SelectedBone(); // index, -1 = none
        static void                     SelectBone( int index );

        // Begin a session on `entity`, seeding a single root bone at `seedHead` (typically the mesh's local
        // AABB centre). Replaces any existing session.
        static void Begin( const Common::UUID& entity, const glm::vec3& seedHead );

        // Add a child of `parent` (-1 = root) at `head`; the new bone becomes selected. Returns its index.
        static int  AddBone( int parent, const glm::vec3& head );
        static void DeleteBone( int index ); // also deletes the sub-tree rooted at `index`
        static void SetHead( int index, const glm::vec3& head );
        static void Cancel();

        // Queue the conversion of Target() and end the placement UI. The heavy swap happens in ProcessPending.
        static void RequestConvert();
        // A conversion is queued: what ProcessPending would act on. The editor defers the swap behind the render
        // work (RenderTask::AfterJoin) only then, so a frame with nothing queued defers no full fence.
        [[nodiscard]] static bool ConvertPending() noexcept;

        // Runs a queued conversion (no-op otherwise). Reads the entity's static geometry, auto-weights it to
        // the placed bones, builds a runtime SkinnedMesh + Skeleton and swaps the component. Safe to call every
        // frame from the editor update loop. Returns true iff a conversion was performed.
        static bool ProcessPending( ::Desert::Core::Scene& scene, const ::Desert::Assets::AssetManager& assets );
    };
} // namespace Desert::Editor
