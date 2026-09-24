#pragma once

#include <Common/Core/UUID.hpp>
#include <Common/Core/Math/Ray.hpp>

#include <Engine/Geometry/VoxelBlockout.hpp>

#include <glm/glm.hpp>

namespace Desert::Core
{
    class Scene;
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
        static bool WorldToScreen( const glm::vec3& world, const glm::mat4& vp, const glm::vec2& pos,
                                   const glm::vec2& size, glm::vec2& out );

        // Every layer, committed and active. Its Unit is the base cell size; Block Size = K * Unit.
        Geometry::VoxelBlockout::Volume m_Volume;
        float        m_BakedUnit = -1.0f;                // base size the live mesh was last baked at
        Common::UUID m_Entity    = Common::UUID::Null(); // live blockout entity

        float     m_GroundY    = 0.0f;    // ground work-plane height in the active grid frame (Level up/down)
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
        bool m_CornerSel[4]{};
        Geometry::VoxelBlockout::CornerHeights m_CornerH{};
    };
} // namespace Desert::Editor::Tools
