#pragma once

// THE WORLD PARTITION MAP — UE's World Partition editor window (SWorldPartitionEditorGrid2D), as an ImGui panel.
//
// A top-down map of the partition: every cell the plan holds, coloured by what streaming is doing with it, the
// background grid of the level that is legible at this zoom, the world axes, and the streaming source with its
// loading circle and unload band. What decides the picture — view transform, zoom, paint order, the cell under
// the cursor, colours — is WorldPartitionMap.hpp and tested there; this file only reads the data and paints it.
//
// TWO SOURCES OF DATA, ONE PICTURE.
//   Edit — the plan of the scene as it is now, made the way the loader makes it (SceneSerializer → the parsed
//          file → PlanWorldPartition). Made when the panel first shows a scene and on Refresh, not every frame:
//          serialising a world is a frame's worth of work. No cell streams, so every cell is Unstreamed.
//   Play — the streamer's own plan and residency (WorldStreamer::Plan / Residency), read live each frame, and
//          its last streaming source. The streamer is asked for through a getter each frame rather than
//          handed in once, because Play's stop and a streaming error both destroy it from the editor's side.

#include "../IPanel.hpp"
#include "WorldPartitionMap.hpp"

#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <Common/Core/ResultStr.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace Desert::Core
{
    class Scene;
    class WorldStreamer;
} // namespace Desert::Core

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
    class WorldPartitionPanel final : public IPanel
    {
    public:
        // @p streamer answers the editor's live streamer, or nullptr outside Play.
        using StreamerGetter = std::function<const ::Desert::Core::WorldStreamer*()>;

        WorldPartitionPanel( std::shared_ptr<::Desert::Core::Scene> scene,
                             const ::Desert::Assets::AssetManager* assets, StreamerGetter streamer );

        void OnUIRender() override;
        void SetScene( const std::shared_ptr<::Desert::Core::Scene>& scene ) override;

        /// SWITCHES THIS PANEL'S SCENE ON AS A PARTITIONED WORLD — UE's "Convert Level to World
        /// Partition", as one undoable history entry.
        ///
        /// The decision is Rules::ConvertToWorldPartition (pure, tested in
        /// Desert/Tests/Engine/WorldPartition); this only stores its answer on the scene, pushes the
        /// entry that can put it back, and marks the plan for a rebuild. The scene becomes dirty because
        /// the entry moves CommandHistory's revision, which is what the editor's unsaved-changes star
        /// reads — there is no second dirty flag to set.
        ///
        /// REFUSES, BY NAME, a scene that is already partitioned. The refusal is the returned error, so
        /// the button and the command palette both show the same sentence.
        [[nodiscard]] Common::BoolResultStr ConvertSceneToWorldPartition();
        // The default layout docks the panel (EditorLayer). A layout saved before that line has no place for
        // it and ImGui floats it: at this size the map is still readable instead of a ~30 px strip.
        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 640.0f, 720.0f };
        }

    private:
        // The Edit plan of m_Scene, rebuilt from its serialised form; the reason when there is none.
        void RebuildEditPlan();
        void DrawMap( const ::Desert::Core::Rules::WorldPartitionPlan& plan,
                      const ::Desert::Core::WorldPartitionSerialized&  partition,
                      const ::Desert::Core::Rules::ResidencyState*     residency,
                      const ::Desert::Core::WorldStreamer*             streamer );

        std::shared_ptr<::Desert::Core::Scene> m_Scene;
        const ::Desert::Assets::AssetManager*  m_Assets;
        StreamerGetter                         m_Streamer;

        // Edit: the plan made from the scene, or why there is none ("not partitioned", a parse error).
        std::optional<::Desert::Core::Rules::WorldPartitionPlan> m_EditPlan;
        ::Desert::Core::WorldPartitionSerialized                 m_EditPartition;
        std::string                                              m_EditPlanStatus;
        bool                                                     m_EditPlanStale = true;
        // The scene's partition as it was when m_EditPlan was made. The plan is remade when the scene's
        // own partition stops matching it, which is how an UNDO of a conversion reaches this panel: the
        // history entry writes the scene and nothing else, so a notification would be a second path to
        // keep in step with it (and one nothing would notice going missing).
        std::optional<::Desert::Core::WorldPartitionSerialized> m_PlanSource;
        // The last conversion's refusal, shown under the button until the next attempt.
        std::string m_ConvertStatus;

        WorldPartitionMap::View m_View;
        bool                    m_FocusPending = true; // fit the plan once the canvas has a size
        bool                    m_Fitted       = false; // the view is the last fit: a resize refits it
        glm::dvec2              m_MapSize{ 0.0 };       // the canvas size the view was last drawn at
        bool                    m_Follow       = true; // Play: keep the streaming source centred (UE's default)
        int                     m_Level        = -1;   // -1 shows every level
        bool                    m_WasPlaying   = false;
    };
} // namespace Desert::Editor
