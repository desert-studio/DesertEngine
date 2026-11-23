#pragma once

#include <Common/Core/ResultStr.hpp>
#include <CoreReflection/ReflectionMacros.hpp>
#include <Engine/ECS/EntityValue.hpp>

#include <glm/vec3.hpp>

#include <string>

namespace Desert::Libraries
{
    /// The script face of Animation::IModularCharacter (ECS::ModularCharacter) - the MECHANISMS a gameplay script
    /// composes into "put on the vest" (owner 2026-09-27: equipment, slots and items are gameplay, in scripts).
    /// Methods of the LEADER entity (the body): `body:attachFollowerMesh("Assets/Vest.skmesh")`. Ids are numbers
    /// scoped to that body and never reused; every refusal is returned as (nil, why), never a silent nothing.
    /// Meshes are named by path; a cloth asset by its cooked path (AssetHandle::FromCookedPath).
    struct ModularCharacterLibrary
    {
        REFLECT( ScriptName( "ModularCharacter" ) )

        /// UE SetLeaderPoseComponent: a child entity skinned with this body's pose, at its LOD and size.
        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "attachFollowerMesh" ) )
        static Common::ResultStr<int> AttachFollowerMesh( ECS::Entity body, const std::string& skinnedMesh );

        /// Destroys the follower's cloth first, then the follower.
        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "detachFollowerMesh" ) )
        static Common::BoolResultStr DetachFollowerMesh( ECS::Entity body, int follower );

        /// A static mesh on a socket (or bone) of this body; offsets on top of it, euler radians.
        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "attachToSocket" ) )
        static Common::ResultStr<int> AttachToSocket( ECS::Entity body, const std::string& staticMesh,
                                                      const std::string& socket, const glm::vec3& translation,
                                                      const glm::vec3& rotation, const glm::vec3& scale );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "detachFromSocket" ) )
        static Common::BoolResultStr DetachFromSocket( ECS::Entity body, int attachment );

        /// UE ShowMaterialSection: a section (submesh) of the body hidden under clothing, or shown again.
        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "setBodySectionVisible" ) )
        static Common::BoolResultStr SetBodySectionVisible( ECS::Entity body, int section, bool visible );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "createCloth" ) )
        static Common::ResultStr<int> CreateCloth( ECS::Entity body, int follower, const std::string& clothAsset,
                                                   const std::string& backend );

        FUNCTION( ScriptCallable, ScriptMethod, ScriptName( "destroyCloth" ) )
        static Common::BoolResultStr DestroyCloth( ECS::Entity body, int cloth );
    };
} // namespace Desert::Libraries
