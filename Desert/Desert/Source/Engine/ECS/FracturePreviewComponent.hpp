#pragma once
// THE FRACTURE MODE'S LEVEL-VIEWPORT PREVIEW (DST-06; UE: the Fracture Mode shows the selected collection's
// pieces in the level viewport, UGeometryCollectionComponent::ApplyExplodedView moving their DISPLAY transforms).
//
// The editor's FractureTool puts this on the entity it fractures while the mode is open and takes it off when
// the mode closes. The entity then draws as the tool's pieces (MeshECSSystem -> Destruction/FracturePieces.hpp),
// each moved by its Explode offset. EDITOR STATE ONLY: not reflected, not in the scene's component blocks, never
// written to a scene or a `.dfrac` (census FracturePieces.TheExplodeOffsetIsPreviewStateOnly).
#include <Engine/Destruction/FractureEdit.hpp>

#include <memory>

namespace Desert::ECS
{
    struct FracturePreviewComponent
    {
        std::shared_ptr<const Destruction::FractureData> Fracture; // the tool's fracture, as the panel shows it
        Destruction::FractureViewSettings                View;     // Explode Amount / Fracture Level
    };
} // namespace Desert::ECS
