#pragma once

// WHO LISTENS TO WHICH ENTITY'S EVENT — UE's sparse multicast delegate (DECLARE_DYNAMIC_MULTICAST_SPARSE_DELEGATE,
// SparseDelegate.h): the event is DESCRIBED on the type (EVENT(...) -> Reflection::EventInfo) and BOUND per
// instance in a side table, so an entity nobody listens to carries nothing and costs nothing. One table per
// registry, in its context (registry.ctx_or_set<ComponentEvents>()).
//
// Language-agnostic like a reflected call (FunctionInfo::Invoke): a listener receives the payload as Values in
// the event's parameter order. C++, Luau and C# subscribe through the same Subscribe; a language binding wraps
// its closure in a Listener (SCR-PORT). The broadcaster is the Gameplay Framework (ComponentEventSystem).

#include <Common/Core/ResultStr.hpp>
#include <Engine/Reflection/ReflectionTypes.hpp>
#include <Engine/Reflection/Value.hpp>

#include <entt/entt.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace Desert::ECS
{
    class ComponentEvents
    {
    public:
        using Listener     = std::function<void( std::span<const Reflection::Value> payload )>;
        using Subscription = std::uint64_t; ///< 0 is never a subscription

        /// Binds @p listener to the event @p event of the reflected type @p type on @p self (UE AddDynamic).
        /// Refused by name when the type or the event is not reflected, or @p listener is empty.
        [[nodiscard]] Common::ResultStr<Subscription> Subscribe( entt::entity self, std::string_view type,
                                                                std::string_view event, Listener listener );
        /// The same, for an EventInfo already looked up (it must be the registry's own record: binding is by
        /// identity, so a copy of the record names no event).
        [[nodiscard]] Common::ResultStr<Subscription> Subscribe( entt::entity self, const Reflection::EventInfo& event,
                                                                Listener listener );

        /// Unbinds one subscription (UE RemoveDynamic). False when it is not bound (already removed, or pruned
        /// with its entity).
        bool Unsubscribe( Subscription subscription );

        /// Whether anything listens to @p event on @p self — what a broadcaster asks BEFORE packing a payload.
        [[nodiscard]] bool IsBound( entt::entity self, const Reflection::EventInfo& event ) const;
        [[nodiscard]] bool Empty() const
        {
            return m_Bound.empty();
        }

        /// Calls every listener of @p event on @p self with @p payload, in subscription order; returns how many.
        /// A listener may subscribe or unsubscribe while being called: the call goes to the listeners bound when
        /// the broadcast began. A payload of another count or kind than the event's parameters is refused, and
        /// nobody is called.
        [[nodiscard]] Common::ResultStr<std::size_t> Broadcast( entt::entity self, const Reflection::EventInfo& event,
                                                               std::span<const Reflection::Value> payload ) const;

        /// Drops the subscriptions of entities @p registry no longer has. An id carries its version, so a
        /// recycled id is a different entity and never receives a dead one's listeners; this only frees them.
        void Prune( const entt::registry& registry );

    private:
        struct Key
        {
            entt::entity                 Self  = entt::null;
            const Reflection::EventInfo* Event = nullptr;
            bool                         operator==( const Key& ) const = default;
        };
        struct KeyHash
        {
            std::size_t operator()( const Key& key ) const noexcept
            {
                return std::hash<const void*>{}( key.Event ) ^
                       ( std::hash<std::uint64_t>{}( static_cast<std::uint64_t>( static_cast<std::underlying_type_t<entt::entity>>( key.Self ) ) )
                         << 1u );
            }
        };
        struct Bound
        {
            Subscription Id = 0;
            Listener     Call;
        };

        std::unordered_map<Key, std::vector<Bound>, KeyHash> m_Bound;
        Subscription                                         m_Next = 1;
    };
} // namespace Desert::ECS
