#pragma once

#include <Engine/ECS/VolumeComponent.hpp>
#include "../IPanel.hpp"

#include <Common/Core/ResultStr.hpp>
#include <Editor/Core/Selection/MeshBooleanTool.hpp>
#include <Editor/Core/Selection/MeshSelectionOperations.hpp>
#include <Editor/Core/Selection/MeshXformOperations.hpp>
#include <Common/Core/UUID.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>

#include <array>

#include <memory>

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
    // UE5-style Modeling Mode palette: a left rail of the palettes something of ours lives in (Selection,
    // Shapes, Create, PolyModel, TriModel, Transform, UVs, Bake, Collision, in UE's order), each palette's tool buttons,
    // and a Tool Properties section for the active tool. A tool picked here switches the viewport into
    // Modeling mode; the geometry editing itself happens in the viewport, driven through Core::ModelingState.
    class ModelingPanel final : public IPanel
    {
    public:
        ModelingPanel( const std::shared_ptr<Desert::Core::Scene>&   scene,
                       const std::shared_ptr<Assets::AssetManager>& assets );

        void OnUIRender() override;

        // Contextual: the viewport is in Modeling mode.
        bool IsContextual() const override
        {
            return true;
        }

        bool IsRelevant() const override;

        void SetScene( const std::shared_ptr<Desert::Core::Scene>& scene ) override
        {
            m_Scene = scene;
        }

        // UE SetCollisionGeometryTool's simple types we have a shape for (Aligned Boxes, Minimal Spheres,
        // Capsules, Convex Hulls), one element per mesh. The panel's combo and the control channel's
        // "Mesh To Collision <Name>" commands both list this.
        struct CollisionShapeEntry
        {
            const char*        Name;
            Physics::ShapeType Shape;
        };
        static constexpr std::array<CollisionShapeEntry, 4> kCollisionShapes = {
             { { "Box", Physics::ShapeType::Box },
               { "Sphere", Physics::ShapeType::Sphere },
               { "Capsule", Physics::ShapeType::Capsule },
               { "Convex Hull", Physics::ShapeType::ConvexHull } } };

        // UE's Mesh To Collision on the selected entity: fits `shape` to its mesh as its Collider, one undo
        // step. The panel's button and the palette command both run this; a refusal carries its reason.
        static Common::ResultStr<bool> MeshToCollision( const std::shared_ptr<Desert::Core::Scene>& scene,
                                                        const CollisionShapeEntry&                  shape );

        // UE UMeshToVolumeTool's New Volume Type choices. The panel's combo and the control channel's
        // "Mesh To Volume <Name>" commands both list this.
        struct VolumeTypeEntry
        {
            const char*     Name;
            ::Desert::ECS::VolumeType Type;
        };
        static constexpr std::array<VolumeTypeEntry, 2> kVolumeTypes = {
             { { "Trigger", ::Desert::ECS::VolumeType::Trigger }, { "Blocking", ::Desert::ECS::VolumeType::Blocking } } };

        // UE's Mesh To Volume on the selected entity: its mesh's points become a convex volume entity of @p type
        // (outline only, a Jolt hull body with the type's collision profile) that REPLACES it, one undo step.
        static Common::ResultStr<bool> MeshToVolume( const std::shared_ptr<Desert::Core::Scene>& scene,
                                                     const VolumeTypeEntry&                      type );

        // UE LOD Manager's preview on the selection (one static mesh with a LOD chain): `level` -1 = Auto, else
        // the level the active viewport draws it at while the LODs tab is open. The panel's radio buttons and the
        // control channel's "LOD Manager: preview" commands both run this; a refusal carries its reason.
        static Common::ResultStr<bool> PreviewLOD( const std::shared_ptr<Desert::Core::Scene>& scene, int level );

        // UE's Bake Normal Map (Bake palette, BakeTx) on the two selected entities, target first, with the
        // palette's settings (Core::BakeNormalMapSettings). The panel's button and the palette command both run
        // this; a refusal carries its reason.
        static Common::ResultStr<bool> BakeNormalMap( const std::shared_ptr<Desert::Core::Scene>&   scene,
                                                      const std::shared_ptr<Assets::AssetManager>& assets );

    private:
        // The Select Elements tool's properties: mode, counts, and the selection operations.
        void DrawCreateShape();
        static void DrawOutputType();
        void DrawElementSelection();
        // One per rail entry, in UE's palette order (the Palette enum in ModelingPanel.cpp).
        void DrawSelectionPalette();
        void DrawShapesPalette();
        void DrawCreatePalette();
        void DrawPolyModelPalette();
        void DrawTriModelPalette();
        void DrawSculptTools(); // M27's Sculpt / Dynamic Sculpt (+ Remesh), the top of the Deform palette
        void DrawTransformPalette();
        void DrawVoxOpsPalette();
        void DrawDeformPalette();
        void DrawMeshOpsPalette();
        void DrawAttributesPalette();
        void DrawUVsPalette();
        void DrawBakePalette();
        void DrawCollisionPalette();
        void DrawLODsPalette();
        void DrawCubeGrid();
        // A mesh / XForm operation on one click; a refusal is logged with its reason.
        void Operate( Core::MeshOperation op );
        void Transform( Core::XformOperation op );
        void RunBoolean( Core::BooleanTool tool );
        // Boolean and Trim share their Output options (UE's Write To and Handle Inputs).
        static void DrawBooleanOutput();

        std::shared_ptr<Desert::Core::Scene> m_Scene;
        std::shared_ptr<Assets::AssetManager> m_Assets;
        int                                  m_Category  = 0; // index into the rail (Palette in the .cpp)
        int                                  m_ShownTool = 0; // the ModelingState::Tool the rail last followed
        int m_VolumeType     = 0; // Mesh To Volume's New Volume Type: index into kVolumeTypes
        int m_CollisionShape = 0; // Mesh To Collision's type: index into kCollisionShapes in the .cpp
    };
} // namespace Desert::Editor
