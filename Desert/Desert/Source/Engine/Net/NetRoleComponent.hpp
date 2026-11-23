#pragma once

// THE NET ROLE OF A REPLICATED PAWN ON A CLIENT (UE ENetRole: ROLE_AutonomousProxy / ROLE_SimulatedProxy).
// Runtime-only, never serialized: ClientNetDriver puts it on the pawns the server replicates to this client.
// A pawn with a role is MOVED BY THE NETWORK: PhysicsECSSystem neither steps its character nor writes the
// character's position back - the driver writes the transform from the replicated state every frame.
//   * Autonomous: this client's own pawn. Its input (CharacterControllerComponent::MoveInput, the script's intent)
//     goes to the server; its position is the server's (NET-4 predicts it locally and corrects from the ack).
//   * Simulated: another player's pawn, drawn interpolated kInterpolationDelaySeconds behind the server clock.

#include <cstdint>

namespace Desert::ECS
{
    enum class ENetRole : std::uint8_t
    {
        Autonomous,
        Simulated,
    };

    struct NetRoleComponent
    {
        ENetRole      Role   = ENetRole::Simulated;
        std::uint32_t NetId  = 0; // Desert::Net::NetId of the pawn
        std::uint32_t Player = 0; // the server's player number
    };
} // namespace Desert::ECS
