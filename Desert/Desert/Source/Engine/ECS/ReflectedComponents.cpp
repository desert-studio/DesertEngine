#include "ReflectedComponents.hpp"

#include <Engine/Core/Serialize/ReflectedComponentBlocks.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>

#include <string>
#include <vector>

namespace Desert::ECS
{
    namespace
    {
        template <class TComponent>
        void* DataOf( TComponent& component, const Core::Serialize::ReflectedWholeBlock<TComponent>& )
        {
            return &component;
        }

        template <class TComponent, class TData>
        void* DataOf( TComponent& component, const Core::Serialize::ReflectedMemberBlock<TComponent, TData>& row )
        {
            return &( component.*row.Member );
        }

        // The member pointer of a row is a value, not a type, so the Data accessor of each row is a lambda over a
        // per-row static; one instantiation per component type (a component appears in the list once).
        template <class TRow>
        ReflectedComponent MakeRow( const TRow& row )
        {
            using TComponent       = typename TRow::Component;
            static const TRow kRow = row;

            ReflectedComponent out;
            out.Name     = row.Key;
            out.TypeName = row.TypeName;
            out.Has      = []( entt::registry& r, entt::entity e ) { return r.has<TComponent>( e ); };
            out.Data     = []( entt::registry& r, entt::entity e ) -> void*
            { return DataOf( r.get<TComponent>( e ), kRow ); };
            out.Add = []( entt::registry& r, entt::entity e )
            {
                if ( !r.has<TComponent>( e ) )
                    r.emplace<TComponent>( e );
            };
            out.Remove = []( entt::registry& r, entt::entity e )
            {
                if ( r.has<TComponent>( e ) )
                    r.remove<TComponent>( e );
            };
            out.NotifyChanged = []( entt::registry& r, entt::entity e ) { r.patch<TComponent>( e ); };
            return out;
        }

        const std::vector<ReflectedComponent>& Rows()
        {
            static const std::vector<ReflectedComponent> rows = []
            {
                std::vector<ReflectedComponent> all;
                Core::Serialize::ForEachReflectedComponentBlock( [&all]( const auto& row )
                                                                 { all.push_back( MakeRow( row ) ); } );
                return all;
            }();
            return rows;
        }
    } // namespace

    const Reflection::TypeInfo* ReflectedComponent::Type() const
    {
        return Reflection::ReflectionRegistry::Get().Find( std::string( TypeName ) );
    }

    std::span<const ReflectedComponent> AllReflectedComponents()
    {
        return Rows();
    }

    const ReflectedComponent* FindReflectedComponent( std::string_view name )
    {
        for ( const auto& row : Rows() )
            if ( row.Name == name )
                return &row;
        return nullptr;
    }
} // namespace Desert::ECS
