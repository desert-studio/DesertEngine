#pragma once

// FIELDS acting on destruction — a port of UE's Chaos field nodes (Chaos/Field/FieldSystemNodes.cpp) and of
// the physics types a field drives (EFieldPhysicsType, FieldSystemTypes.h:148-159), evaluated at points in
// the world. A field is a value per point: a scalar (falloffs, a mask, their sum) or a vector (uniform,
// radial, their sum weighted by a scalar). A FieldCommand pairs a field with what it does; it is applied
// once, at the instant it fires, by DestructionWorld::ApplyField.
//
// What a command does (UE name in brackets):
//   Impulse        — the vector field at a body's centre of mass is an impulse on that body (kg*cm/s), and
//                    its length at a piece's centre of mass strains that piece like a contact of that
//                    impulse would [LinearForce + ExternalClusterStrain, as UE's "master field" pairs them];
//   ExternalStrain — the scalar at a piece's centre of mass is its external strain [ExternalClusterStrain];
//   Kill           — a body whose centre of mass reads above zero leaves the simulation [Kill];
//   Anchor         — a leaf whose centre of mass reads above zero is anchored from now on (our own; UE's
//                    anchor field sets a dynamic state).
//
// Every number is in centimetres, kilograms and seconds.

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <variant>

namespace Desert::Destruction
{
    /// UE EFieldFalloffType: how a falloff's value goes from its centre (1) to its edge (0).
    enum class FieldFalloff
    {
        None,       ///< Magnitude everywhere inside
        Linear,     ///< 1 - d/r
        Squared,    ///< (1 - d/r)^2
        Inverse,    ///< 2 (1 - 1 / (f + 1))
        Logarithmic ///< log2(f + 1)
    };

    /// UE EFieldOperationType.
    enum class FieldOperation
    {
        Multiply,
        Divide,
        Add,
        Subtract
    };

    /// UE FRadialFalloff: Magnitude * (MinRange + (MaxRange - MinRange) * falloff(1 - d / Radius)) inside the
    /// sphere, Default outside (and everywhere when the radius is not positive).
    struct RadialFalloff
    {
        float        Magnitude = 1.0f;
        float        MinRange  = 0.0f;
        float        MaxRange  = 1.0f;
        float        Default   = 0.0f;
        float        Radius    = 0.0f;
        glm::vec3    Position  = { 0.0f, 0.0f, 0.0f };
        FieldFalloff Falloff   = FieldFalloff::Linear;

        [[nodiscard]] float Evaluate( const glm::vec3& point ) const;
    };

    /// UE FBoxFalloff: the 100 cm unit box placed by Transform; inside, the falloff runs from 1 at the centre
    /// to 0 at the nearest face; Default outside.
    struct BoxFalloff
    {
        float        Magnitude = 1.0f;
        float        MinRange  = 0.0f;
        float        MaxRange  = 1.0f;
        float        Default   = 0.0f;
        glm::mat4    Transform = glm::mat4( 1.0f );
        FieldFalloff Falloff   = FieldFalloff::Linear;

        [[nodiscard]] float Evaluate( const glm::vec3& point ) const;
    };

    /// UE FRadialIntMask (condition Field_Set_Always): InteriorValue strictly inside the sphere, ExteriorValue
    /// elsewhere.
    struct RadialIntMask
    {
        float     Radius        = 0.0f;
        glm::vec3 Position      = { 0.0f, 0.0f, 0.0f };
        int32_t   InteriorValue = 1;
        int32_t   ExteriorValue = 0;

        [[nodiscard]] int32_t Evaluate( const glm::vec3& point ) const;
    };

    /// UE FUniformVector: Magnitude * Direction everywhere.
    struct UniformVector
    {
        float     Magnitude = 1.0f;
        glm::vec3 Direction = { 0.0f, 0.0f, 1.0f };

        [[nodiscard]] glm::vec3 Evaluate( const glm::vec3& point ) const;
    };

    /// UE FRadialVector: Magnitude along the direction from Position to the point (zero at Position).
    struct RadialVector
    {
        float     Magnitude = 1.0f;
        glm::vec3 Position  = { 0.0f, 0.0f, 0.0f };

        [[nodiscard]] glm::vec3 Evaluate( const glm::vec3& point ) const;
    };

    using ScalarLeaf = std::variant<RadialFalloff, BoxFalloff, RadialIntMask>;
    using VectorLeaf = std::variant<UniformVector, RadialVector>;

    /// UE FSumScalar: Magnitude * (Left op Right).
    struct SumScalar
    {
        float          Magnitude = 1.0f;
        ScalarLeaf     Left;
        ScalarLeaf     Right;
        FieldOperation Operation = FieldOperation::Multiply;

        [[nodiscard]] float Evaluate( const glm::vec3& point ) const;
    };

    using ScalarField = std::variant<RadialFalloff, BoxFalloff, RadialIntMask, SumScalar>;

    /// UE FSumVector: Magnitude * Scalar * (Left op Right); without Right, Magnitude * Scalar * Left.
    struct SumVector
    {
        float                     Magnitude = 1.0f;
        ScalarField               Scalar;
        VectorLeaf                Left;
        std::optional<VectorLeaf> Right;
        FieldOperation            Operation = FieldOperation::Add;

        [[nodiscard]] glm::vec3 Evaluate( const glm::vec3& point ) const;
    };

    using VectorField = std::variant<UniformVector, RadialVector, SumVector>;

    [[nodiscard]] float     Evaluate( const ScalarLeaf& field, const glm::vec3& point );
    [[nodiscard]] float     Evaluate( const ScalarField& field, const glm::vec3& point );
    [[nodiscard]] glm::vec3 Evaluate( const VectorLeaf& field, const glm::vec3& point );
    [[nodiscard]] glm::vec3 Evaluate( const VectorField& field, const glm::vec3& point );

    /// UE EFieldPhysicsType, the part destruction acts on (see the header comment).
    enum class FieldPhysicsType
    {
        Impulse,
        ExternalStrain,
        Kill,
        Anchor,
    };

    struct FieldCommand
    {
        FieldPhysicsType Type = FieldPhysicsType::ExternalStrain;
        ScalarField      Scalar; ///< ExternalStrain, Kill, Anchor
        VectorField      Vector; ///< Impulse
    };
} // namespace Desert::Destruction
