#pragma once

// AN HLOD THE EDITOR SHOWS FOR A PART OF THE WORLD IT DOES NOT HOLD (WP-EDITOR-HLOD; UE 5.5's editor HLODs,
// UWorldPartitionHLODEditorSubsystem / UWorldPartitionEditorHLOD).
//
// The editor opens a partitioned world in part (EditorRegions); everything outside the held regions would be
// empty ground. Editor/Panels/WorldPartition/EditorHLODs makes the SAVED HLOD of every such cell (the records
// Build HLODs wrote to <Scene>_HLOD/) entities of the edited scene, through the very InstantiateRecords Play uses,
// so they are drawn, shadowed and lit by the one path the game draws them with. This tag is what makes them NOT
// the world's:
//   - SceneSerializer leaves them out of every document (save, the Play snapshot, LiveEntities - hence out of
//     the packages, the region load's census and the undo history, all of which read the serializer);
//   - the Outliner does not list them and a click in the viewport does not select them;
//   - PlaySession destroys them before Play takes its snapshot (Play streams its own HLODs), and EditorHLODs
//     makes them again once the editor is back in Edit.
// Never reflected, never written: an entity carries it only from the moment EditorHLODs made it.

#include <cstddef>

namespace Desert::ECS
{
    struct EditorHLODComponent
    {
        std::size_t HLOD = 0; // EditorHLODs' index of the HLOD (the HLODRuntime's) this entity belongs to
    };
} // namespace Desert::ECS
