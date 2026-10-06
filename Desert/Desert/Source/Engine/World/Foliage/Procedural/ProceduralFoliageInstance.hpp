#pragma once

// Ported from UE 5.8 Engine/Source/Runtime/Foliage/Public/ProceduralFoliageInstance.h and FoliageType.h's
// procedural helpers (InstancedFoliage.cpp: GetMaxRadius, GetScaleForAge, GetInitAge, GetNextAge,
// GetSpawnsInShade), and Core/Public/Math/RandomStream.h. Adapted: a type is named by its index into the
// spawner's list instead of a UFoliageType pointer; the simulation plane is the world's ground plane (X, Z; Y is
// up in this project), so a location is a glm::vec2 of (world X, world Z) in centimetres; rotation is kept as the
// yaw and pitch UE draws, in degrees.

#include <Engine/Assets/Serialization/FoliageType.hpp>

#include <glm/vec2.hpp>

#include <cstdint>

namespace Desert::World::Foliage::Procedural
{
    /**
     * @brief UE's FRandomStream: the 32-bit linear congruential stream the procedural simulation draws from.
     *
     * Integer arithmetic only (wrapping unsigned), so a seed yields the same sequence on every platform.
     */
    class RandomStream
    {
    public:
        RandomStream() = default;
        explicit RandomStream( int32_t seed )
        {
            Initialize( seed );
        }

        void Initialize( int32_t seed )
        {
            m_Seed = static_cast<uint32_t>( seed );
        }

        /// A float in [0, 1).
        float GetFraction();
        float FRand()
        {
            return GetFraction();
        }
        uint32_t GetUnsignedInt();
        /// A float in [min, max).
        float FRandRange( float min, float max )
        {
            return min + ( max - min ) * FRand();
        }
        /// An integer in [min, max].
        int32_t RandRange( int32_t min, int32_t max );

    private:
        void MutateSeed()
        {
            m_Seed = m_Seed * 196314165u + 907633515u;
        }

        uint32_t m_Seed = 0;
    };

    /// UE's RAND_MAX as Windows states it: the simulation multiplies by it, so a platform's own RAND_MAX would
    /// make the same seed grow another forest on another platform.
    inline constexpr int32_t kProceduralRandMax = 0x7fff;

    /// UE's FBox2D: an inclusive axis-aligned box on the ground plane, centimetres.
    struct Box2
    {
        glm::vec2 Min{ 0.0f };
        glm::vec2 Max{ 0.0f };

        /// Touching boxes intersect (UE FBox2D::Intersect).
        [[nodiscard]] bool Intersects( const Box2& other ) const
        {
            return !( Min.x > other.Max.x || other.Min.x > Max.x || Min.y > other.Max.y || other.Min.y > Max.y );
        }
        [[nodiscard]] bool Contains( const Box2& other ) const
        {
            return other.Min.x >= Min.x && other.Max.x <= Max.x && other.Min.y >= Min.y && other.Max.y <= Max.y;
        }
    };

    // UFoliageType's procedural helpers, over the type's Procedural block.
    [[nodiscard]] float MaxRadius( const Assets::Serialization::FoliageProcedural& type );
    /// ProceduralScale.Min + span * ScaleCurve(clamp(age / MaxAge)); MaxAge 0 reads the curve at 1.
    [[nodiscard]] float ScaleForAge( const Assets::Serialization::FoliageProcedural& type, float age );
    [[nodiscard]] float InitAge( const Assets::Serialization::FoliageProcedural& type, RandomStream& stream );
    /// The age after @p numSteps generations: one per step, never past MaxAge.
    [[nodiscard]] float NextAge( const Assets::Serialization::FoliageProcedural& type, float age,
                                 int32_t numSteps );
    [[nodiscard]] bool  SpawnsInShade( const Assets::Serialization::FoliageProcedural& type );

    /// Which circles of two instances touch (UE ESimulationOverlap). Collision wins when both do.
    enum class OverlapKind
    {
        Collision,
        Shade
    };

    /**
     * @brief One plant of the simulation (UE FProceduralFoliageInstance).
     *
     * A Blocker is an instance a neighbouring tile owns: it takes part in the competition (and always wins it)
     * but is never placed by the tile that holds it.
     */
    struct ProceduralFoliageInstance
    {
        glm::vec2 Location{ 0.0f };
        float     YawDegrees   = 0.0f;
        float     PitchDegrees = 0.0f;
        float     Age          = 0.0f;
        float     Scale        = 1.0f;
        /// Index into the spawner's type list.
        uint32_t TypeIndex = 0;
        bool     Blocker   = false;
        bool     Alive     = true;

        [[nodiscard]] bool operator==( const ProceduralFoliageInstance& ) const = default;
    };

    /// The procedural numbers of an instance's type, and the radii they give at its scale.
    struct InstanceRadii
    {
        float Collision = 0.0f;
        float Shade     = 0.0f;

        [[nodiscard]] float Max() const
        {
            return Collision > Shade ? Collision : Shade;
        }
    };
    [[nodiscard]] InstanceRadii RadiiOf( const ProceduralFoliageInstance&                instance,
                                         const Assets::Serialization::FoliageProcedural& type );

    /**
     * @brief Of two overlapping instances, the one that dies; nullptr when neither does (UE Domination).
     *
     * A blocker always survives (two blockers: neither dies); otherwise the higher OverlapPriority, then the older, then the larger wins. A
     * shade overlap kills nobody whose type CanGrowInShade.
     */
    [[nodiscard]] const ProceduralFoliageInstance*
    Dominated( const ProceduralFoliageInstance& a, const Assets::Serialization::FoliageProcedural& aType,
               const ProceduralFoliageInstance& b, const Assets::Serialization::FoliageProcedural& bType,
               OverlapKind kind );
} // namespace Desert::World::Foliage::Procedural
