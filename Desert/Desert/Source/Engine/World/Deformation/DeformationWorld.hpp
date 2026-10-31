#pragma once

#include <Common/Core/ResultStr.hpp>
#include <CoreReflection/ReflectionMacros.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// THE WORLD'S DEFORMATION SERVICE (DEFORM-FIELD; Notion Вики DEFORM-ARCH, «Выбранная архитектура»). The mechanism
// half of surface deformation: every source (a script's World.deform today; animation Contact keys, Jolt bodies
// and wheels in DEFORM-SOURCES) submits a DeformContact here, and every consumer that keeps a field of its own
// (Graphic/Deformation/DeformationFieldRenderer: the clipmap around a view; the CPU mirror of DEFORM-RESPONSE;
// the persistent pages of DEFORM-PERSIST) reads the contacts it has not applied yet by their sequence number.
//
// WHY A LOG AND NOT A PER-FRAME QUEUE. A queue drained by "the" renderer has one reader, and a world has several:
// two viewports of one scene are two SceneRenderers with a clipmap each, and the CPU mirror is a third. Each
// reader keeps the sequence it has reached; nothing here knows who reads. The log is bounded (kMaxRetained): a
// reader that fell further behind than that has missed contacts, and Since() says how many so the miss is not
// silent.
//
// THE SHAPE IS NOT HERE. A contact names its stamp (a `Domain DeformStamp` shader asset, by name) and carries the
// stamp's typed inputs; what the stamp does with them lives in the asset's graph. Units: centimetres, cm/s, kg.
namespace Desert::World
{
    /// The typed inputs of a DeformStamp material (its contract, Deformation/DeformStampContract.glslh, reads the
    /// same names). UE's analogue is a Niagara user parameter block: data a stamp's graph may read, not behaviour.
    struct DeformStampParams
    {
        REFLECT()

        PROPERTY( Units( "cm" ), Tooltip( "Footprint radius: the stamp's local space spans [-1, 1] over this." ) )
        float Radius = 15.0f;
        PROPERTY( Tooltip( "Contact pressure the stamp reads (F/A; 1 = the stamp's authored depth)." ) )
        float Pressure = 1.0f;
        PROPERTY( Units( "cm/s" ), Tooltip( "Contact speed along the surface." ) )
        float Speed = 0.0f;
        PROPERTY( Units( "cm" ), Tooltip( "Slip of the contact over its lifetime (X, Z in world space)." ) )
        glm::vec2 SlipVector = { 0.0f, 0.0f };
        PROPERTY( Units( "rad" ), Tooltip( "Heading of the contact around the world up axis (0 = +Z)." ) )
        float Heading = 0.0f;
        PROPERTY( Units( "kg" ), Tooltip( "Mass bearing on the contact." ) )
        float Mass = 70.0f;
        PROPERTY( Tooltip( "Surface normal under the contact (world)." ) )
        glm::vec3 ContactNormal = { 0.0f, 1.0f, 0.0f };
        PROPERTY( Units( "s" ), Tooltip( "Age of the contact when it is applied." ) )
        float Age = 0.0f;
        PROPERTY( Tooltip( "Per-contact random seed the stamp may vary its shape by." ) )
        float Seed = 0.0f;
    };

    /// One contact of a source with a deformable surface.
    struct DeformContact
    {
        std::string       Stamp; // the DeformStamp shader's name (its file stem)
        glm::vec3         At{ 0.0f };
        DeformStampParams Params;
        uint64_t          Sequence = 0; // assigned by Submit; strictly increasing per world
    };

    class DeformationWorld
    {
    public:
        static constexpr std::size_t kMaxRetained = 16384;

        /// Appends @p contact (its Sequence is assigned here). Refused, with the reason, for a contact no field
        /// can apply: no stamp named, a radius that is not positive, a position that is not finite.
        Common::BoolResultStr Submit( DeformContact contact );

        /// The contacts with Sequence >= @p from, in submission order; @p missed = how many of those the log no
        /// longer holds (the reader fell more than kMaxRetained behind).
        [[nodiscard]] std::vector<DeformContact> Since( uint64_t from, uint64_t& missed ) const;

        /// The sequence the next contact will get (a new reader starting here sees only future contacts).
        [[nodiscard]] uint64_t NextSequence() const
        {
            return m_Next;
        }

    private:
        std::deque<DeformContact> m_Log;
        uint64_t                  m_Next = 0;
    };
} // namespace Desert::World
