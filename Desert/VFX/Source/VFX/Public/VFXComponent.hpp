#pragma once

#include <Engine/ECS/SequencerEffectDrive.hpp>
#include <entt/entt.hpp>

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
#include <string_view>

#include <Engine/Assets/Common.hpp>
#include <VFX/Public/VFXPointSet.hpp>
#include <VFX/Public/VFXUserOverride.hpp>

namespace Desert::ECS
{
    /**
     * @brief THE EFFECT AN ENTITY PLAYS (UE UNiagaraComponent): a `.dfx` system and whether it starts by itself.
     *
     * Every emitter of the system simulates on the GPU through its compiled module stack (Engine/VFX
     * VFXStackCompiler -> the ParticleSimulate host program), born by the emitter's spawn plan
     * (CompileSpawnPlan), at the entity's transform. Empty System = the entity plays nothing.
     */
    struct VFXComponentData
    {
        REFLECT()

        PROPERTY( DisplayName( "System" ), Category( "VFX" ), Asset<VFXSystemAsset>,
                  Tooltip( "The .dfx effect this entity plays" ) )
        Assets::AssetHandle System;

        // UE bAutoActivate: false = the system is placed but bears no particles until it is activated.
        PROPERTY( DisplayName( "Auto Activate" ), Category( "VFX" ) )
        bool AutoActivate = true;

        // UE UNiagaraComponent::OverrideParameters: this placement's values for the system's User parameters
        // (a parameter absent here plays its Default from the `.dfx`). Read by the spawn plan (VFXWorld) and the
        // GPU parameter rows (ParticleWorldGpu) every tick. Drawn by the VFX entry's "User Parameters" section,
        // which lists the SYSTEM's rows (a name, a type, a default) - a bare list of name/value pairs cannot.
        PROPERTY( DisplayName( "User Parameters" ), Category( "VFX" ), Hidden,
                  Tooltip( "This placement's values for the system's User parameters" ) )
        std::vector<VFXUserOverride> Overrides;

        // VFX-15: this placement's point sets, painted in VFX Paint mode and read by the stack's PointSet.<name>
        // bindings (engine:ShapePointSet). Level data, like Overrides: two placements of one system differ by it.
        PROPERTY( DisplayName( "Point Sets" ), Category( "VFX" ), Hidden,
                  Tooltip( "The point sets painted for this placement (VFX Paint mode)" ) )
        std::vector<VFXPointSet> PointSets;

        /// The set named @p name, or null.
        [[nodiscard]] const VFXPointSet* FindPointSet( std::string_view name ) const
        {
            for ( const VFXPointSet& s : PointSets )
                if ( s.Name == name )
                    return &s;
            return nullptr;
        }

        /// UE SetVariable*: this placement's User.<name> becomes @p value (packed like the row's Default),
        /// starting from the next VFX tick. The type is the system's to check (VFX.setVariable does).
        void SetVariable( std::string_view name, const glm::vec4& value )
        {
            for ( VFXUserOverride& o : Overrides )
                if ( o.Name == name )
                {
                    o.Value = value;
                    return;
                }
            Overrides.push_back( VFXUserOverride{ std::string( name ), value } );
        }

        /// The parameter plays its Default again (UE: resetting an override in the Details panel).
        void ClearVariable( std::string_view name )
        {
            std::erase_if( Overrides, [&]( const VFXUserOverride& o ) { return o.Name == name; } );
        }
    };

    /// A Level Sequence's hold on an effect (UE: UNiagaraComponent under a Niagara system track - Activate /
    /// Deactivate plus AgeUpdateMode DesiredAge): Active = the playhead is inside one of its VFX track's
    /// sections and the effect stands at AgeSeconds (VFXWorld replays this instance's fixed steps to it);
    /// inactive = outside every section, the instance holds no particles.
    // (the struct is the sequencer's: Engine/ECS/SequencerEffectDrive.hpp)

    struct VFXComponent
    {
        COMPONENT( Key( "VFX" ), Block( Data ), Run( ActorsAndUI ) )
        VFXComponentData Data;

        // One-shot restart from the editor (every emitter of the system starts over). Transient - not reflected.
        bool RequestRestart = false;

        // Set by a Level Sequence's VFX track (LevelSequenceEntityHost::DriveEffect); nullopt = the effect runs on
        // the world's time as authored. While set, this instance's age is the sequence's - the scene's delta and
        // AutoActivate do not move it. The Sequencer preview gives it back on Restore. Transient - not reflected.
        std::optional<VFXSequencerDrive> SequencerDrive;
    };
} // namespace Desert::ECS
