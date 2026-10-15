#include <Engine/Physics/RagdollDesc.hpp>

#include <glm/gtc/constants.hpp>

namespace Desert::Physics
{
    float PhysicsBodyMassKg( const PhysicsAssetBody& body )
    {
        if ( body.MassKg > 0.0f )
            return body.MassKg;
        const float pi  = glm::pi<float>();
        const float r   = body.Radius;
        float       cm3 = 0.0f;
        switch ( body.Shape )
        {
            case PhysicsBodyShape::Sphere:
                cm3 = 4.0f / 3.0f * pi * r * r * r;
                break;
            case PhysicsBodyShape::Capsule:
                cm3 = pi * r * r * body.Length + 4.0f / 3.0f * pi * r * r * r;
                break;
            case PhysicsBodyShape::Box:
                cm3 = body.BoxExtents.x * body.BoxExtents.y * body.BoxExtents.z;
                break;
        }
        // grams -> kilograms
        return cm3 * body.DensityGramsPerCm3 / 1000.0f;
    }
} // namespace Desert::Physics
