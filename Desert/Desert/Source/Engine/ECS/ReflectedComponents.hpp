#pragma once

// COMPONENT <-> REFLECTED TYPE, LANGUAGE-FREE — the one table any consumer that reaches a component BY NAME uses
// (the script binding today; the generated Luau binder and the C ABI next).
//
// A row is built from Core::Serialize::ForEachReflectedComponentBlock, the list the scene saver and the migrator
// already read, so a component is reachable by name exactly when its block is "reflection and nothing else" on
// disk; the name is the block's record key. There is no second list to keep in step (the script binding had its
// own copy, kReflectedComponents, which reached 19 of the 54 rows).
//
// Writes through a row end in NotifyChanged, which is EnTT's `patch` on the component: the ECS-native "this
// component was edited" signal (registry.on_update<T>()), the same one a system subscribes to for any other edit.

#include <entt/entt.hpp>

#include <span>
#include <string_view>

namespace Desert::Reflection
{
    struct TypeInfo;
}

namespace Desert::ECS
{
    struct ReflectedComponent
    {
        std::string_view Name;     // the block's record key — the name a script asks for
        std::string_view TypeName; // the reflected struct of the data, as ReflectionRegistry names it

        bool ( *Has )( entt::registry&, entt::entity )    = nullptr;
        void* ( *Data )( entt::registry&, entt::entity )  = nullptr; // the reflected struct; the entity must Has
        void ( *Add )( entt::registry&, entt::entity )    = nullptr; // default-constructed; no-op when present
        void ( *Remove )( entt::registry&, entt::entity ) = nullptr; // no-op when absent
        void ( *NotifyChanged )( entt::registry&,
                                 entt::entity )           = nullptr; // registry.patch<T>: fires on_update<T>

        // The reflected type of Data; null only when the registry has not been populated (no reflection linked).
        [[nodiscard]] const Reflection::TypeInfo* Type() const;
    };

    // Every row, in the serializer's order.
    std::span<const ReflectedComponent> AllReflectedComponents();

    // The row whose Name is `name`, or null.
    const ReflectedComponent* FindReflectedComponent( std::string_view name );
} // namespace Desert::ECS
