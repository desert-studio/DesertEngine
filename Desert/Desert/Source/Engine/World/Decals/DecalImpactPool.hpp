#pragma once

// THE WORLD'S IMPACT DECALS (NG-DECALS-MESH; UE UGameplayStatics::SpawnDecalAtLocation + UDecalComponent::
// SetFadeOut / FadeDuration, without UE's one actor per decal): a fixed ring of spawn records, no ECS entity per
// decal. A spawn writes the record after the newest; a spawn into a full ring replaces the oldest record (UE's
// r.Decal.MaxCount-style recycling, here the ring's own order). Every record is a box decal of the same pass as a
// DecalComponent ("Deferred: Decals", Graphic::System::DecalRenderer): ECS::DecalECSSystem hands the live records
// to the renderer each frame as DecalProxy rows, faded by age (FadeDuration before the end of Lifetime) on top of
// the renderer's own distance and screen-size fades.
//
// The capacity is a quality value (Scalability Effects.DecalImpactPoolSize), applied by ECS::DecalECSSystem.
// Units: centimetres, seconds.

#include <Common/Core/ResultStr.hpp>
#include <CoreReflection/ReflectionMacros.hpp>
#include <Engine/Assets/Common.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <mutex>
#include <vector>

namespace Desert::Graphic::System
{
    struct DecalProxy;
}

namespace Desert::World
{
    /// One impact: the argument of World.spawnDecal (a table, so a call names what it sets and leaves the rest
    /// default): `World.spawnDecal{ Material = self.Material, At = hit, Normal = n, Size = vector.create(8, 8, 4),
    /// Lifetime = 20, FadeDuration = 2 }`.
    struct DecalImpactSpawn
    {
        REFLECT( ScriptStruct )

        PROPERTY( Asset<MaterialAsset>,
                  Tooltip( "A material in the Deferred Decal domain (the impact's look is the asset's). Empty "
                           "draws the engine's default decal." ) )
        Assets::AssetHandle Material;
        PROPERTY( Units( "cm" ), Tooltip( "World position of the impact (the box's centre)." ) )
        glm::vec3 At = { 0.0f, 0.0f, 0.0f };
        PROPERTY( Tooltip( "The hit surface's normal: the box's projection axis. Need not be normalised." ) )
        glm::vec3 Normal = { 0.0f, 1.0f, 0.0f };
        PROPERTY( Units( "cm" ),
                  Tooltip( "UE DecalSize / 2: half size along the texture plane (x, y) and the projection axis "
                           "(z, how deep into and out of the surface the decal reaches)." ) )
        glm::vec3 Size = { 8.0f, 8.0f, 4.0f };
        PROPERTY( Units( "deg" ), Tooltip( "Rotation of the texture plane around the normal." ) )
        float Roll = 0.0f;
        PROPERTY( Units( "s" ),
                  Tooltip( "UE LifeSpan: seconds of gameplay time until the record is retired. 0 = kept until the "
                           "ring replaces it." ) )
        float Lifetime = 20.0f;
        PROPERTY( Units( "s" ),
                  Tooltip( "UE SetFadeOut's duration: the last seconds of Lifetime over which the decal fades to "
                           "nothing. 0 = no time fade." ) )
        float FadeDuration = 2.0f;
        PROPERTY( Units( "cm" ),
                  Tooltip( "The camera distance at which the decal starts to fade out (Decal Fade Start). 0 with "
                           "Fade Length 0 = no distance fade." ) )
        float FadeStart = 3000.0f;
        PROPERTY( Units( "cm" ), Tooltip( "The distance over which it fades out past Fade Start; 0 = none." ) )
        float FadeLength = 2000.0f;
        PROPERTY( Tooltip( "UE SortOrder: a higher value draws over lower ones; equal orders draw newest last." ) )
        int32_t SortOrder = 0;
    };

    class DecalImpactPool
    {
    public:
        DecalImpactPool() = default;

        DecalImpactPool( const DecalImpactPool& )            = delete;
        DecalImpactPool& operator=( const DecalImpactPool& ) = delete;

        /// Resizes the ring, keeping the newest min(live, @p capacity) records in spawn order. 0 drops them all
        /// and refuses spawns. Same capacity: no work.
        void SetCapacity( uint32_t capacity ) noexcept;

        /// Writes one record after the newest (replacing the oldest when the ring is full). Refused: no capacity
        /// (Effects.DecalImpactPoolSize 0), a zero normal or a size with a zero or negative axis.
        [[nodiscard]] Common::BoolResultStr Spawn( const DecalImpactSpawn& spawn ) noexcept;

        /// Ages every record by @p seconds of gameplay time and retires the ones past their Lifetime.
        void Advance( float seconds ) noexcept;

        /// Appends one DecalProxy per live record to @p out, oldest first (the renderer sorts by SortOrder, then
        /// by the spawn serial each proxy carries as its EntityId: newer impacts stack over older ones).
        void Collect( std::vector<Graphic::System::DecalProxy>& out ) const;

        /// Drops every record (a level change, PIE end); the capacity stays.
        void Clear() noexcept;

        [[nodiscard]] uint32_t GetCapacity() const noexcept;
        [[nodiscard]] uint32_t GetLiveCount() const noexcept;

    private:
        struct Record
        {
            glm::mat4           DecalToWorld{ 1.0f }; // the unit box in world: axes x size, centre at At
            Assets::AssetHandle Material     = Assets::AssetHandle::Null();
            uint64_t            Serial       = 0; // spawn order: the renderer's tie-break inside one SortOrder
            float               Age          = 0.0f;
            float               Lifetime     = 0.0f;
            float               FadeDuration = 0.0f;
            float               FadeStart    = 0.0f;
            float               FadeLength   = 0.0f;
            int32_t             SortOrder    = 0;
            bool                Live         = false;
        };

        mutable std::mutex  m_Mutex;      // Lua spawns (game thread) vs the collector (a parallel system group)
        std::vector<Record> m_Ring;       // size = capacity
        uint32_t            m_Next   = 0; // the slot the next spawn writes (the oldest when the ring is full)
        uint64_t            m_Serial = 0;
    };
} // namespace Desert::World
