#include <Engine/Destruction/DestructionField.hpp>

#include <algorithm>
#include <cmath>

namespace Desert::Destruction
{
    namespace
    {
        // ScaleFunctionResult + EvalFalloffFunction (FieldSystemNodes.cpp:440-480).
        float ApplyFalloff( FieldFalloff type, float magnitude, float minRange, float maxRange, float f )
        {
            float shaped = f;
            switch ( type )
            {
                case FieldFalloff::None:
                    shaped = 1.0f;
                    break;
                case FieldFalloff::Linear:
                    break;
                case FieldFalloff::Squared:
                    shaped = f * f;
                    break;
                case FieldFalloff::Inverse:
                    shaped = 2.0f * ( 1.0f - 1.0f / ( f + 1.0f ) );
                    break;
                case FieldFalloff::Logarithmic:
                    shaped = std::log2( f + 1.0f );
                    break;
            }
            return magnitude * ( minRange + ( maxRange - minRange ) * shaped );
        }

        float Combine( FieldOperation operation, float left, float right )
        {
            switch ( operation )
            {
                case FieldOperation::Multiply:
                    return right * left;
                case FieldOperation::Divide:
                    return left / right;
                case FieldOperation::Add:
                    return right + left;
                case FieldOperation::Subtract:
                    return left - right;
            }
            return 0.0f;
        }

        glm::vec3 Combine( FieldOperation operation, const glm::vec3& left, const glm::vec3& right )
        {
            switch ( operation )
            {
                case FieldOperation::Multiply:
                    return right * left;
                case FieldOperation::Divide:
                    return left / right;
                case FieldOperation::Add:
                    return right + left;
                case FieldOperation::Subtract:
                    return left - right;
            }
            return glm::vec3( 0.0f );
        }
    } // namespace

    // FRadialFalloff::Evaluator (FieldSystemNodes.cpp:486-508).
    float RadialFalloff::Evaluate( const glm::vec3& point ) const
    {
        if ( !( Radius > 0.0f ) )
            return Default;
        const float delta = glm::length( point - Position );
        if ( delta >= Radius )
            return Default;
        return ApplyFalloff( Falloff, Magnitude, MinRange, MaxRange, 1.0f - delta / Radius );
    }

    // FBoxFalloff::Evaluator (FieldSystemNodes.cpp:716-738).
    float BoxFalloff::Evaluate( const glm::vec3& point ) const
    {
        constexpr float kHalfBox = 50.0f;
        const glm::vec3 local    = glm::vec3( glm::inverse( Transform ) * glm::vec4( point, 1.0f ) );
        if ( std::abs( local.x ) > kHalfBox || std::abs( local.y ) > kHalfBox || std::abs( local.z ) > kHalfBox )
            return Default;
        const glm::vec3 distance = glm::abs( local ) - glm::vec3( kHalfBox );
        const float     delta    = std::min( std::max( distance.x, std::max( distance.y, distance.z ) ), 0.0f );
        return ApplyFalloff( Falloff, Magnitude, MinRange, MaxRange, -delta / kHalfBox );
    }

    // FRadialIntMask::Evaluate (FieldSystemNodes.cpp:161-185), Field_Set_Always.
    int32_t RadialIntMask::Evaluate( const glm::vec3& point ) const
    {
        const glm::vec3 d = Position - point;
        return glm::dot( d, d ) < Radius * Radius ? InteriorValue : ExteriorValue;
    }

    // FUniformVector::Evaluate (FieldSystemNodes.cpp:919-929).
    glm::vec3 UniformVector::Evaluate( const glm::vec3& ) const
    {
        return Magnitude * Direction;
    }

    // FRadialVector::Evaluate (FieldSystemNodes.cpp:974-984); GetSafeNormal is zero at the centre.
    glm::vec3 RadialVector::Evaluate( const glm::vec3& point ) const
    {
        const glm::vec3 d      = point - Position;
        const float     length = glm::length( d );
        return length > 1.0e-8f ? Magnitude * ( d / length ) : glm::vec3( 0.0f );
    }

    // FSumScalar::Evaluate (FieldSystemNodes.cpp:1165-1257).
    float SumScalar::Evaluate( const glm::vec3& point ) const
    {
        return Magnitude *
               Combine( Operation, Destruction::Evaluate( Left, point ), Destruction::Evaluate( Right, point ) );
    }

    // FSumVector::Evaluate (FieldSystemNodes.cpp:1387-1480).
    glm::vec3 SumVector::Evaluate( const glm::vec3& point ) const
    {
        const glm::vec3 left   = Destruction::Evaluate( Left, point );
        const glm::vec3 vector = Right ? Combine( Operation, left, Destruction::Evaluate( *Right, point ) ) : left;
        return Magnitude * Destruction::Evaluate( Scalar, point ) * vector;
    }

    float Evaluate( const ScalarLeaf& field, const glm::vec3& point )
    {
        return std::visit( [&]( const auto& node ) { return static_cast<float>( node.Evaluate( point ) ); },
                           field );
    }

    float Evaluate( const ScalarField& field, const glm::vec3& point )
    {
        return std::visit( [&]( const auto& node ) { return static_cast<float>( node.Evaluate( point ) ); },
                           field );
    }

    glm::vec3 Evaluate( const VectorLeaf& field, const glm::vec3& point )
    {
        return std::visit( [&]( const auto& node ) { return node.Evaluate( point ); }, field );
    }

    glm::vec3 Evaluate( const VectorField& field, const glm::vec3& point )
    {
        return std::visit( [&]( const auto& node ) { return node.Evaluate( point ); }, field );
    }
} // namespace Desert::Destruction
