#pragma once

#include <Editor/Core/Selection/ModelingState.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>
#include <Engine/Geometry/DynamicMeshSelection.hpp>

#include <memory>
#include <optional>
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

namespace Desert::Editor::Tools
{
    // Modeling Mode -> Attributes -> Edit Materials (UE UEditMeshMaterialsTool): the triangles picked with the
    // element selection (ElementSelectTool runs while this tool is active: click, Shift adds, Ctrl removes;
    // Triangle or PolyGroup mode) get the active material slot. The tool owns a working copy of the mesh and of
    // the component's material slots:
    //   - Assign Active Material writes the slot's id into the selected triangles' MaterialID (preview);
    //   - Add Slot / Remove Last Slot / slot material edit the slot list (preview; a used slot is not removed);
    //   - Accept records mesh + slots as ONE undo step (Commands::ApplyXformEdit), Cancel / another tool /
    //     another entity puts both back.
    class EditMaterialsTool
    {
    public:
        void Update( ::Desert::Core::Scene& scene, const ::Desert::Assets::AssetManager* assets );

    private:
        struct Session
        {
            Common::UUID                                  Entity;
            std::shared_ptr<const Geometry::DynamicMesh3> Committed; // what the component held (null: asset only)
            std::vector<Common::AssetHandle>              CommittedSlots;
            std::shared_ptr<const Geometry::DynamicMesh3> Preview; // the tool target with the edited MaterialIDs
            std::vector<Common::AssetHandle>              Slots;
            std::vector<std::string>                      SlotLabels; // what the panel names each slot
        };

        void               Begin( ::Desert::Core::Scene& scene, Core::ModelingState& ms );
        void               Show( ::Desert::Core::Scene& scene );
        void               Restore( ::Desert::Core::Scene& scene );
        void               AssignSelection( ::Desert::Core::Scene& scene, Core::ModelingState& ms );
        void               RemoveLastSlot( ::Desert::Core::Scene& scene, Core::ModelingState& ms );
        void               SetSlotMaterial( ::Desert::Core::Scene& scene, const ::Desert::Assets::AssetManager* assets,
                                            const std::string& path, Core::ModelingState& ms );
        [[nodiscard]] bool Accept( ::Desert::Core::Scene& scene, Core::ModelingState& ms );

        std::optional<Session> m_Session;
    };
} // namespace Desert::Editor::Tools
