#pragma once

#include <UI/UITree.hpp>

#include <entt/entt.hpp>

#include <cstdint>
#include <type_traits>

// The engine's adapter from its ECS scene to the framework's IUITree — UE's UMG side of the seam: the
// authored UI lives as UI*Component{Data} on entities, the hierarchy in RelationshipComponent, the name in
// TagComponent, the stable id in UUIDComponent. Nothing here caches; every answer is read from the registry
// when asked, so the tree is never stale and costs nothing to build per call.
namespace Desert::UI
{
    static_assert( std::is_same_v<std::underlying_type_t<entt::entity>, std::uint32_t>,
                   "NodeId is bit-for-bit an entt::entity" );

    // Bit for bit for every real entity. The null id is mapped explicitly both ways: entt's null is its
    // ENTITY bits all set (the version bits are not part of it, and differ by entt release), so it is not
    // the all-ones NodeId::Null as a bit pattern — and `e == entt::null` is entt's own test for it.
    [[nodiscard]] constexpr NodeId ToNode( entt::entity e )
    {
        return e == entt::null ? NodeId::Null : static_cast<NodeId>( static_cast<std::uint32_t>( e ) );
    }

    [[nodiscard]] constexpr entt::entity ToEntity( NodeId n )
    {
        return n == NodeId::Null ? entt::entity( entt::null )
                                 : static_cast<entt::entity>( static_cast<std::uint32_t>( n ) );
    }

    static_assert( ToNode( entt::entity( entt::null ) ) == NodeId::Null, "entt::null is NodeId::Null" );
    static_assert( ToEntity( NodeId::Null ) == entt::null, "NodeId::Null is entt::null" );

    class EcsUITree final : public IUITree
    {
    public:
        explicit EcsUITree( entt::registry& reg ) : m_Reg( &reg )
        {
        }

        [[nodiscard]] bool             Valid( NodeId n ) const override;
        [[nodiscard]] std::size_t      NodeBound() const override;
        [[nodiscard]] NodeId           Parent( NodeId n ) const override;
        [[nodiscard]] std::size_t      ChildCount( NodeId n ) const override;
        [[nodiscard]] NodeId           ChildAt( NodeId n, std::size_t i ) const override;
        [[nodiscard]] std::string_view Name( NodeId n ) const override;
        [[nodiscard]] std::uint64_t    StableId( NodeId n ) const override;
        [[nodiscard]] const void*      Find( NodeId n, ArgKind kind ) const override;
        [[nodiscard]] void*            FindState( NodeId n, ArgKind kind ) override;
        void                           Roots( ArgKind kind, std::vector<NodeId>& out ) const override;
        [[nodiscard]] const void*      Storage() const override
        {
            return m_Reg;
        }
        [[nodiscard]] std::optional<glm::vec3> WorldOrigin( NodeId n ) const override;

        [[nodiscard]] entt::registry& Registry() const
        {
            return *m_Reg;
        }

    private:
        entt::registry* m_Reg;
    };
} // namespace Desert::UI
