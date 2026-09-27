#pragma once

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>
#include <Common/Core/Math/Ray.hpp>

#include <Engine/Geometry/VoxelBlockout.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor::Commands
{
    struct EntityStateSnapshot;
}

namespace Desert::Editor::Tools
{
    // UE5-style CubeGrid blockout tool (Modeling mode). Marquee-select a rectangle on the work surface (the
    // ground, or a face of what you built), then Push / Pull to extrude that region. Blocks Per Step multiplies
    // the extrude height. The voxel volume, its edits and the bake are Geometry::VoxelBlockout; this class is
    // the interaction around it (picking, marquee, Corner Mode posts, the panel, the Blockout entity).
    class CubeGridTool
    {
    public:
        void Update( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray, const glm::mat4& viewProj,
                     const glm::vec2& viewportPos, const glm::vec2& viewportSize, bool interactive );

    private:
        void RegenMesh( ::Desert::Core::Scene& scene );                // rebuild the blockout mesh from the volume
        void Cancel( ::Desert::Core::Scene& scene );                   // delete the in-progress blockout
        void PushPull( ::Desert::Core::Scene& scene, int dir, int K ); // extrude the selection out/in
        void RefineBy( int F ); // subdivide the base grid by F (split every cell + the selection xF)
        void FreezeActive();    // commit the volume being worked on into an immutable layer
        // Write the Corner Mode heights into the top layer of cells under the selection.
        void        ApplyCornerHeights( ::Desert::Core::Scene& scene );
        void        SyncCornerHeights(); // read the rectangle's corner heights back out of the cells
        // Shift+E / Shift+Q: move the selection `dir` x Blocks Per Step blocks along its outward normal.
        void SlideSelection( int dir, int K );
        // Shift+B: the Quick Material onto the exposed faces under the selection, in every layer.
        void PaintSelection( ::Desert::Core::Scene& scene );
        // The Quick Material's ID in m_Materials, appended on first use (UE UpdateOpMaterials). Empty, with
        // the reason logged, when the set is full (a face stores its ID in a byte).
        std::optional<uint8_t> OpMaterialId();
        // Point the entity's material slots at m_Materials in the order the bake's submeshes use them.
        void        ApplyMaterialSlots( ::Desert::Core::Scene& scene, const std::vector<int>& submeshMaterialIds );
        void        ResetSession(); // forget the volume, the selection and the material set (Accept / Cancel)
        // Reopen on the selected entity (UE: the tool takes the selected mesh as its target): its voxels become
        // the volume, the grid goes onto its last piece, and Accept / Cancel end in ONE undo step / the entity
        // as it was. Refused with a toast, by reason (BlockoutSession.hpp, ReopenBlockout).
        void        EditSelected( ::Desert::Core::Scene& scene );
        // Accept's first step: renumber the materials to the entity's slots, re-bake, and store the voxels (in
        // the entity's space) and the key of the mesh they baked to on the entity.
        Common::BoolResultStr StoreVoxels( ::Desert::Core::Scene& scene );
        // The volume in the entity's space, which is what the entity's mesh and saved voxels are in.
        Geometry::VoxelBlockout::Volume LocalVolume() const;
        static bool WorldToScreen( const glm::vec3& world, const glm::mat4& vp, const glm::vec2& pos,
                                   const glm::vec2& size, glm::vec2& out );

        // Every layer, committed and active. Its Unit is the base cell size; Block Size = K * Unit.
        Geometry::VoxelBlockout::Volume m_Volume;
        float        m_BakedUnit = -1.0f;                // base size the live mesh was last baked at
        Common::UUID m_Entity    = Common::UUID::Null(); // live blockout entity
        // A reopened blockout: the entity as it was when the session began (null for a new blockout), and the
        // entity's transform as a grid frame (identity for a new one, which is built in the world).
        std::shared_ptr<const Commands::EntityStateSnapshot> m_Before;
        Geometry::VoxelBlockout::GridFrame                   m_EntityFrame;
        bool m_WasActive = false; // last frame's toolActive: opening the tool on a blockout reopens it

        float     m_GroundY    = 0.0f;    // ground work-plane height in the active grid frame (Level up/down)
        bool      m_GroundToPivot = false;   // Ctrl+MMB moved the pivot: next frame's ground goes through it
        bool      m_HoverValid = false;   // last frame's cursor targeting hit something

        // Work-plane (in BASE cells): locked while selecting and kept for the active selection.
        Geometry::VoxelBlockout::WorkPlane m_Plane;

        // Marquee rectangle selection (inclusive BASE-cell range, always Block-aligned).
        bool       m_Selecting = false;
        glm::ivec2 m_Anchor{ 0 }; // press cell (base)
        bool       m_HasSel = false;
        Geometry::VoxelBlockout::Rect m_Sel;

        // Corner Mode (Z): the selection rectangle's four corner posts. Heights are in the same
        // 1/CornerDen base-cell units as Cell::V; index order is (uMin,vMin) (uMax,vMin) (uMin,vMax)
        // (uMax,vMax).
        bool m_CornerMode = false;

        // The blockout's material set: a face's material ID indexes it. ID 0 is the engine default, so a
        // blockout nobody picked a material for bakes to one submesh with an empty slot, as before.
        std::vector<Common::AssetHandle> m_Materials{ Common::AssetHandle{} };

        // Ctrl + LMB drag (UE): the cursor ray is projected onto the line through the selection's centre
        // along its outward normal (grid space); the distance from the press, in whole Blocks Per Step,
        // is how far the selection has been pulled out (or pushed in) so far.
        bool      m_DragExtrude = false;
        glm::vec3 m_DragOrigin{ 0.0f };
        glm::vec3 m_DragAxis{ 0.0f, 1.0f, 0.0f };
        float     m_DragStart   = 0.0f;
        int       m_DragApplied = 0; // blocks (or corner snap steps) already applied by this drag
        bool m_CornerSel[4]{};
        Geometry::VoxelBlockout::CornerHeights m_CornerH{};
    };
} // namespace Desert::Editor::Tools
