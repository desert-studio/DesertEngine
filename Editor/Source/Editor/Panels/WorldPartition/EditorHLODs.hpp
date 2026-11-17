#pragma once

// THE SAVED HLODs OF THE CELLS THE EDITOR DOES NOT HOLD (WP-EDITOR-HLOD) - UE 5.5's editor HLODs
// (UWorldPartitionHLODEditorSubsystem, wp.Editor.HLOD.AllowShowingHLODsInEditor; Show HLODs and the HLOD-in-editor
// Min/Max draw distances of UWorldPartitionEditorPerProjectUserSettings).
//
// A world opens in the editor in part (EditorRegions, decision O3): without this, everything outside the held
// regions is empty ground. Here the HLOD Build HLODs saved for every cell (<Scene>_HLOD/, WorldCells::
// ReadBuiltHLODs) is made entities of the edited scene - one cell's records at a time, through the
// InstantiateRecords Play makes its HLODs with, so they are drawn, shadowed and lit by the game's own path (the
// card's variant (a): no second, editor-only draw path) - tagged ECS::EditorHLODComponent, which keeps them out of
// the save, the Play snapshot, the packages, the undo history, the Outliner and picking.
//
// WHICH ONE IS DRAWN: Play's switch, unchanged (Rules::HLODRuntime over the world's plan), fed with the residency
// the editor holds (Rules::EditorResidency: a cell is Activated when the editor holds any record of it) - so a
// region load hides the HLODs of its cells in the frame its entities arrive, and an unload shows them again. On
// top of it the user's view: Show HLODs, and the Min/Max distance from the camera to the cell's content
// (Rules::InEditorHLODRange).
//
// Staleness is not judged here: the editor shows what was saved, as UE's editor shows the built HLOD actors; Build
// HLODs and Play are where a stale cell is named.

#include <Engine/Core/Serialize/EditorRegions.hpp>
#include <Engine/Core/Serialize/WorldPartitionHLODSwitch.hpp>

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <glm/glm.hpp>

#include <cstddef>
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

namespace Desert::Editor
{
    // The user's answer (EditorPreferences): Show HLODs, and the draw range in cm (Max 0 = no far limit).
    struct EditorHLODView
    {
        bool  Show        = true;
        float MinDistance = 0.0f;
        float MaxDistance = 0.0f;
    };

    class EditorHLODs final : public ::Desert::Core::Rules::HLODVisibilityWorld
    {
    public:
        // Makes the saved HLOD of every cell of @p scene's world as tagged entities, hidden until SyncHeld and
        // Apply say otherwise; what was made before is destroyed first. A scene that is not a partitioned world
        // opened from its files, a world no runtime grid of which names an HLOD layer, and a world whose HLODs
        // were never built make none - Status() says which. Refused, by name and leaving nothing made, when the
        // world's index or a saved HLOD cannot be read or an HLOD record has the id of an entity the scene holds.
        [[nodiscard]] Common::BoolResultStr Make( ::Desert::Core::Scene& scene, ::Desert::Assets::AssetManager* assets );

        // Destroys every entity Make made, and forgets them.
        void Release( ::Desert::Core::Scene& scene );

        // Forgets what Make made without touching the scene: its entities are gone already (Play took the scene,
        // a load replaced it).
        void Forget();

        // Destroys every editor HLOD entity of @p scene, whoever made it: PlaySession, before Play's snapshot - Play
        // streams the world's own HLODs, which these would double (and whose ids they share).
        static void ReleaseAll( ::Desert::Core::Scene& scene );

        // Whether what Make made is still in @p scene (false once a load or a Play replaced its entities).
        [[nodiscard]] bool IsInScene( const ::Desert::Core::Scene& scene ) const;

        // The residency the editor holds now (the scene's packages) through the HLOD switch: after Make, and after
        // every region or data-layer change.
        [[nodiscard]] Common::BoolResultStr SyncHeld( ::Desert::Core::Scene& scene );

        // Every editor frame: an HLOD is drawn when the switch shows it, the user shows HLODs and @p eye (the
        // editor camera, cm; none = no range test) is within the user's range of its cell. Only a change is told
        // to the scene.
        void Apply( ::Desert::Core::Scene& scene, const EditorHLODView& view, const std::optional<glm::vec3>& eye );

        [[nodiscard]] std::size_t Count() const noexcept
        {
            return m_HLODs.size();
        }
        [[nodiscard]] std::size_t        DrawnCount() const noexcept;
        [[nodiscard]] const std::string& Status() const noexcept
        {
            return m_Status;
        }

        // HLODVisibilityWorld: the switch's answer, kept until Apply combines it with the user's view.
        void SetHLODVisible( std::size_t hlod, bool visible ) override;

    private:
        struct MadeHLOD
        {
            std::size_t               Unit = 0;         // the cell unit of m_World->Plan it stands in for
            std::vector<Common::UUID> Ids;              // the entities of its records (what InstantiateRecords made)
            bool                      StandsIn = false; // the switch shows it: the editor holds nothing of its cell
            std::optional<bool>       Drawn;            // what Apply last told the scene; none before the first
        };

        std::optional<::Desert::Core::EditorRegions::WorldOnDisk> m_World;
        std::optional<::Desert::Core::Rules::HLODRuntime>         m_Runtime;
        std::vector<MadeHLOD>                                     m_HLODs; // the switch's HLOD index
        std::string                                               m_Status;
    };
} // namespace Desert::Editor
