#pragma once

// THE SAVED HLODs OF THE CELLS THE EDITOR DOES NOT HOLD (WP-EDITOR-HLOD) - UE 5.5's editor HLODs
// (UWorldPartitionHLODEditorSubsystem, wp.Editor.HLOD.AllowShowingHLODsInEditor; Show HLODs and the HLOD-in-editor
// Min/Max draw distances of UWorldPartitionEditorPerProjectUserSettings).
//
// A world opens in the editor in part (EditorRegions, decision O3): without this, everything outside the held
// regions is empty ground. Here the HLOD Build HLODs saved for every cell (<Scene>_HLOD/, WorldCells::
// ReadBuiltHLODs) is made entities of the edited scene through the InstantiateRecords Play makes its HLODs with, so they are drawn, shadowed and lit by the game's own path (the
// card's variant (a): no second, editor-only draw path) - tagged ECS::EditorHLODComponent, which keeps them out of
// the save, the Play snapshot, the packages, the undo history, the Outliner and picking.
//
// WHICH ONE IS DRAWN: Play's switch, unchanged (Rules::HLODRuntime over the world's plan), fed with the residency
// the editor holds (Rules::EditorResidency: a cell is Activated when the editor holds any record of it) - so a
// region load hides the HLODs of its cells in the frame its entities arrive, and an unload shows them again. On
// top of it the user's view: Show HLODs, and the Min/Max distance from the camera to the cell's content
// (Rules::InEditorHLODRange). An HLOD carries the runtime data layers of its cell (UE: an HLOD actor carries the
// data layers of what it was built from), so the HLOD of a cell none of whose layers this user has loaded in the
// editor stands in for nothing, as the cell's own records are not held either (EditorRegions::SelectRecords).
//
// READ WHEN DRAWN, OFF THE MAIN THREAD (WP-HLOD-HANG; UE 5.5: the HLOD editor subsystem loads an HLOD actor's
// package when it is to be shown, the editor never loads every HLOD of the world up front): Make reads only the
// index (HLODs.dhlod: one row per cell, its proxy file and the ids of its records) and plans the switch over it. An
// HLOD's proxy file is read and parsed on a JobSystem worker the first time Apply wants it drawn, and made entities
// on the main thread the frame it lands, one HLOD per frame. Before, Make parsed every proxy file of the world on
// the main thread (598 files, 668 MB on WP_Protocol: the editor stood ~10 min after Build HLODs).
//
// Staleness is not judged here: the editor shows what was saved, as UE's editor shows the built HLOD actors; Build
// HLODs and Play are where a stale cell is named.

#include <Engine/Core/Serialize/EditorRegions.hpp>
#include <Engine/Core/Serialize/WorldCells.hpp>
#include <Engine/Core/Serialize/WorldPartitionHLODSwitch.hpp>

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <string_view>
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
    struct HLODStatus; // Editor/Core/WorldPartitionHLODBuilder.hpp

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
        // Plans the saved HLOD of every cell of @p scene's world from @p status - the panel's one answer
        // (HLODStatusOf, made once per change, never here): only a node whose State is Current is drawn, from the
        // row and the files that status read. Each is made tagged entities only once Apply wants it drawn (its
        // proxy file read on a worker); what was made before is destroyed first. A scene that is not a partitioned
        // world opened from its files, a world no runtime grid of which names an HLOD layer, a status that could
        // not be made (@p statusError) and a world whose HLODs were never built make none - Status() says which.
        // Refused, by name and leaving nothing made, when the world's descriptor index cannot be read or an HLOD
        // lists a record with the id of a record of the world or of an entity the scene holds (as Play refuses
        // it). A Current node whose cell the editor's plan does not have is logged by name and not drawn. A proxy
        // file that does not read, or whose records cannot be made, is refused when it lands: logged by name, that
        // HLOD is never drawn. @p layerLoaded: SyncHeld's.
        [[nodiscard]] Common::BoolResultStr Make( ::Desert::Core::Scene&          scene,
                                                  ::Desert::Assets::AssetManager* assets,
                                                  const std::vector<bool>& layerLoaded, const HLODStatus* status,
                                                  std::string_view statusError );

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
        // every region or data-layer change. @p layerLoaded[l]: whether the world's data layer `l` is loaded in
        // the editor for this user (EditorRegions::DataLayersLoadedInEditor).
        [[nodiscard]] Common::BoolResultStr SyncHeld( ::Desert::Core::Scene&   scene,
                                                      const std::vector<bool>& layerLoaded );

        // Every editor frame: an HLOD is drawn when the switch shows it, the user shows HLODs and @p eye (the
        // editor camera, cm; none = no range test) is within the user's range of its cell - and its proxy is made.
        // One that should be drawn and was never read starts its read here; one read that has landed is made
        // (one per frame). Only a change is told to the scene.
        void Apply( ::Desert::Core::Scene& scene, const EditorHLODView& view, const std::optional<glm::vec3>& eye );

        [[nodiscard]] std::size_t Count() const noexcept
        {
            return m_HLODs.size();
        }
        [[nodiscard]] std::size_t        DrawnCount() const noexcept;
        // The HLODs the last Apply wanted drawn whose proxy is still being read or is not made yet: a frame drawn
        // now shows their cells without them (the Control channel's PendingWork::EditorHLODs).
        [[nodiscard]] std::size_t WaitingCount() const noexcept
        {
            return m_Waiting;
        }
        [[nodiscard]] const std::string& Status() const noexcept
        {
            return m_Status;
        }

        // HLODVisibilityWorld: the switch's answer, kept until Apply combines it with the user's view.
        void SetHLODVisible( std::size_t hlod, bool visible ) override;

    private:
        using ProxyRecords = Common::ResultStr<std::vector<::Desert::Assets::EntityData>>;
        enum class Proxy : std::uint8_t
        {
            Unread,  // never wanted drawn: only its index row is known
            Reading, // its file is being read and parsed on a worker (Read)
            Made,    // its records are entities of the scene
            Refused  // its file did not read or its records could not be made (logged): never drawn
        };
        struct MadeHLOD
        {
            std::size_t                               Unit = 0; // the cell unit of m_World->Plan it stands in for
            ::Desert::Core::WorldCells::BuiltHLODCell Row;      // its index row: the proxy file, its records' ids
            std::vector<Common::UUID>                 Ids;      // the ids of its records (the row's): its entities
            Proxy                                     State = Proxy::Unread;
            std::future<ProxyRecords>                 Read;             // while Reading
            bool                                      StandsIn = false; // the switch shows it
            std::optional<bool>                       Drawn; // what Apply last told the scene; none before the first
        };

        void StartRead( MadeHLOD& made );
        // The landed read of HLOD @p hlod made entities (hidden), or the HLOD refused by name.
        void MakeProxy( ::Desert::Core::Scene& scene, std::size_t hlod );

        std::optional<::Desert::Core::EditorRegions::WorldOnDisk> m_World;
        std::optional<::Desert::Core::Rules::HLODRuntime>         m_Runtime;
        std::vector<MadeHLOD>                                     m_HLODs; // the switch's HLOD index
        ::Desert::Core::WorldCells::FileReader                    m_Reader; // a proxy file's bytes by its name
        ::Desert::Assets::AssetManager*                           m_Assets  = nullptr;
        std::size_t                                               m_Waiting = 0;
        std::string                                               m_Status;
    };
} // namespace Desert::Editor
