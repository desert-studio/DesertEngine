#pragma once

// CLOTH SIMULATION SEAM — the interface a cloth backend implements, the factory that makes it, and the
// registry a backend is registered in.
//
// UE counterpart: IClothingSimulation, IClothingSimulationFactory and the IModularFeatures registration of
// factories (ClothingSystemRuntimeInterface/Public/ClothingSimulationInterface.h,
// ClothingSimulationFactory.h). Pattern, not letter:
//   * UE's IClothingSimulation holds every clothing ACTOR of one skeletal mesh component. Here ONE simulation
//     is ONE piece of cloth, because pieces are put on and taken off individually (a vest, a cape) and a Jolt
//     soft body is one body per piece anyway; batching them into one physics system is a backend detail.
//   * Destroy is the destruction of the unique_ptr the factory returned. UE needs an explicit
//     DestroySimulation because a module owns the allocation; here the backend object owns its bodies and
//     releases them in its destructor.
//   * The registry is a plain object with a name lookup instead of a global modular-feature list; a missing
//     backend is an error that names the registered ones, never a silent "no cloth".
//
// WIND. The step context carries a wind VELOCITY; the cloth owns no wind. Today the engine's only wind is
// the cloud layer's (VolumetricCloudComponent::WindDirection / WindSpeed,
// Engine/ECS/VolumetricCloudComponent.hpp); foliage has none. Where the velocity comes from is decided with CLO1 —
// the context is the seam.
//
// NOT HERE: CLO1 — the Jolt SoftBody backend (factory "JoltSoftBody"), where the registry lives (the physics
// world is the expected owner), and the ECS system that steps simulations at the physics fixed step.

#include <Engine/Physics/Cloth/ClothingAsset.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Physics::Cloth
{
    // Everything one fixed step needs; the simulation keeps no pointer to any of it past Step().
    struct ClothStepContext
    {
        float     FixedDeltaSeconds = 0.0f; // > 0; a variable dt is the CALLER's bug, the step is fixed
        glm::mat4 ComponentToWorld  = glm::mat4( 1.0f );
        // The wearer's current model-space bone transforms, in ClothingAsset::UsedBoneNames order.
        std::span<const glm::mat4> BoneModelTransforms;
        glm::vec3                  Gravity      = { 0.0f, -980.0f, 0.0f }; // cm/s^2, from the physics world
        glm::vec3                  WindVelocity = { 0.0f, 0.0f, 0.0f };    // cm/s, world space
    };

    // What the renderer reads after a step: one entry per ClothPhysicalMesh vertex, component space (cm).
    // Valid until the next Step()/Teleport() on the same simulation.
    struct ClothSimulationOutput
    {
        std::span<const glm::vec3> Positions;
        std::span<const glm::vec3> Normals;
    };

    enum class ClothTeleportMode
    {
        // Move with the component, keep the current shape and drop the velocity (UE
        // ETeleportType::TeleportPhysics).
        Teleport,
        // Snap back to the skinned reference pose (UE ETeleportType::ResetPhysics).
        Reset,
    };

    class IClothingSimulation
    {
    public:
        virtual ~IClothingSimulation() = default;

        // Advances by exactly context.FixedDeltaSeconds. Same asset + same sequence of contexts = same output
        // bit for bit: determinism is part of the contract (replays and networked characters depend on it).
        // Fails, naming the numbers, on dt <= 0 or on a bone span of the wrong length.
        virtual Common::BoolResultStr Step( const ClothStepContext& context ) = 0;

        [[nodiscard]] virtual ClothSimulationOutput GetOutput() const = 0;

        virtual void Teleport( const glm::mat4& componentToWorld, ClothTeleportMode mode ) = 0;
    };

    class IClothingSimulationFactory
    {
    public:
        virtual ~IClothingSimulationFactory() = default;

        // The registry key, e.g. "JoltSoftBody".
        [[nodiscard]] virtual std::string_view GetName() const = 0;

        // Fails with the reason when this backend cannot simulate the asset (UE SupportsAsset, with a why).
        [[nodiscard]] virtual Common::BoolResultStr SupportsAsset( const ClothingAsset& asset ) const = 0;

        // The returned simulation starts in the reference pose placed at componentToWorld.
        [[nodiscard]] virtual Common::ResultStr<std::unique_ptr<IClothingSimulation>>
        CreateSimulation( const ClothingAsset& asset, const glm::mat4& componentToWorld ) = 0;
    };

    // The registration point for backends. Owns the factories; lookups hand out non-owning pointers that stay
    // valid for the registry's lifetime.
    class ClothingSimulationFactoryRegistry
    {
    public:
        Common::BoolResultStr Register( std::unique_ptr<IClothingSimulationFactory> factory )
        {
            const std::string name( factory->GetName() );
            for ( const auto& existing : m_Factories )
            {
                if ( existing->GetName() == name )
                    return Common::MakeError<bool>( "cloth backend '" + name + "' is already registered" );
            }
            m_Factories.push_back( std::move( factory ) );
            return Common::MakeSuccess( true );
        }

        [[nodiscard]] Common::ResultStr<IClothingSimulationFactory*> Find( std::string_view name ) const
        {
            std::string registered;
            for ( const auto& existing : m_Factories )
            {
                if ( existing->GetName() == name )
                    return Common::MakeSuccess( existing.get() );
                registered += registered.empty() ? "" : ", ";
                registered += existing->GetName();
            }
            return Common::MakeError<IClothingSimulationFactory*>( "no cloth backend '" + std::string( name ) +
                                                                   "'; registered: [" + registered + "]" );
        }

    private:
        std::vector<std::unique_ptr<IClothingSimulationFactory>> m_Factories;
    };
} // namespace Desert::Physics::Cloth
