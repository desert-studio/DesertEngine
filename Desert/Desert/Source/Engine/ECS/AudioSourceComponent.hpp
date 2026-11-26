#pragma once

#include <Common/Core/Entt.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <CoreReflection/ReflectionMacros.hpp>

#include <Engine/Assets/Common.hpp>

namespace Desert::ECS
{
    // --- Audio (miniaudio) ------------------------------------------------------------------------------
    // A sound emitter (reflected -> Details UI + serialization). AudioECSSystem creates the runtime
    // source in Play mode (AutoPlay), positions spatial sources at the entity's world transform, and
    // stops everything on the Play->Edit transition.
    struct AudioSourceData
    {
        REFLECT()

        // The SOUND asset (`.desound`, UE's USoundWave) by its GUID's handle: the audio file is the asset's to
        // name (SoundAsset::Resolve, with the class and loading it plays in), so moving the pair moves no
        // reference. Unset = silent.
        PROPERTY( DisplayName( "Sound" ), Category( "Audio" ), Summary, Asset<SoundAsset> )
        Assets::AssetHandle Sound;

        PROPERTY( DisplayName( "Volume" ), Category( "Audio" ), Range( 0.0f, 2.0f ) )
        float Volume = 1.0f;

        PROPERTY( DisplayName( "Loop" ), Category( "Audio" ) )
        bool Loop = false;

        PROPERTY( DisplayName( "Auto Play" ), Category( "Audio" ) )
        bool AutoPlay = true; // start when the scene enters Play

        PROPERTY( DisplayName( "3D Spatial" ), Category( "Audio" ), Advanced )
        bool Spatial = true; // attenuate/pan from the entity position vs. the listener (camera)
    };

    struct AudioSourceComponent
    {
        COMPONENT( Key( "AudioSource" ), Block( Data ), Run( ActorsAndUI ) )
        AudioSourceData Data;
    };
} // namespace Desert::ECS
