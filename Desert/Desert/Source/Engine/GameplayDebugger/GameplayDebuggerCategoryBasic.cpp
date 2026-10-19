#include <Engine/GameplayDebugger/GameplayDebuggerCategoryBasic.hpp>

#if DESERT_DEV_INSTRUMENTS
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Serialize/ComponentRegistry.hpp>
#include <Engine/ECS/Components.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

#include <format>
#include <string>

namespace Desert::Debug
{
    namespace
    {
        const glm::vec4 kLabel( 0.65f, 0.65f, 0.65f, 1.0f );
        const glm::vec4 kValue( 1.0f, 1.0f, 1.0f, 1.0f );
        const glm::vec4 kMarker( 1.0f, 0.85f, 0.2f, 1.0f );
        const glm::vec4 kVelocity( 0.3f, 0.9f, 1.0f, 1.0f );
        // The cross that marks the target, cm from its centre to a tip.
        constexpr float kMarkerHalfSize = 25.0f;

        void Row( GameplayDebuggerCategoryData& out, std::string label, std::string value )
        {
            out.AddTextLine( { { std::move( label ), kLabel }, { std::move( value ), kValue } } );
        }
    } // namespace

    void GameplayDebuggerCategoryBasic::CollectData( const Core::Scene& scene, const ECS::Entity* targetEntity,
                                                     GameplayDebuggerCategoryData& out )
    {
        const entt::registry& registry = scene.GetRegistry();
        if ( targetEntity == nullptr || !registry.valid( targetEntity->GetHandle() ) )
        {
            out.AddTextLine( { { "no target: look at an entity or select one", kLabel } } );
            return;
        }
        const ECS::Entity& entity = *targetEntity;
        const entt::entity target = entity.GetHandle();

        const std::string name =
             registry.has<ECS::TagComponent>( target ) ? registry.get<ECS::TagComponent>( target ).Tag : "";
        Row( out, "Name  ", name.empty() ? std::string( "<untagged>" ) : name );
        if ( registry.has<ECS::UUIDComponent>( target ) )
            Row( out, "UUID  ", registry.get<ECS::UUIDComponent>( target ).UUID.ToString() );

        // The WORLD transform (a child's TransformComponent is its parent's space).
        const glm::mat4 world = entity.GetWorldTransform();
        glm::vec3       location( 0.0f ), scale( 1.0f ), skew( 0.0f );
        glm::vec4       perspective( 0.0f );
        glm::quat       rotation( 1.0f, 0.0f, 0.0f, 0.0f );
        glm::decompose( world, scale, rotation, location, skew, perspective );
        const glm::vec3 degrees = glm::degrees( glm::eulerAngles( rotation ) );
        Row( out, "Location ", std::format( "{:.1f} {:.1f} {:.1f} cm", location.x, location.y, location.z ) );
        Row( out, "Rotation ", std::format( "{:.1f} {:.1f} {:.1f} deg", degrees.x, degrees.y, degrees.z ) );
        Row( out, "Scale    ", std::format( "{:.2f} {:.2f} {:.2f}", scale.x, scale.y, scale.z ) );

        out.AddShape( { .Type        = GameplayDebuggerShape::Kind::Point,
                        .A           = location,
                        .Radius      = kMarkerHalfSize,
                        .Color       = kMarker,
                        .Description = name } );

        if ( const auto* character = registry.try_get<ECS::CharacterControllerComponent>( target ) )
        {
            const glm::vec3 v = character->Velocity;
            Row( out, "Velocity ",
                 std::format( "{:.1f} {:.1f} {:.1f} cm/s ({:.1f})", v.x, v.y, v.z, glm::length( v ) ) );
            if ( glm::length( v ) > 0.0f )
                out.AddShape( { .Type   = GameplayDebuggerShape::Kind::Arrow,
                                .A      = location,
                                .B      = location + v,
                                .Radius = kMarkerHalfSize,
                                .Color  = kVelocity } );
        }

        std::string components;
        for ( const Core::Serialize::ComponentSerializer* serializer :
              Core::Serialize::ComponentRegistry::Get().InFileOrder() )
        {
            if ( !serializer->Has( entity ) )
                continue;
            std::format_to( std::back_inserter( components ), "{}{}", components.empty() ? "" : ", ",
                            serializer->Key );
        }
        Row( out, "Components ", components.empty() ? std::string( "<none>" ) : components );
    }
} // namespace Desert::Debug
#endif // DESERT_DEV_INSTRUMENTS
