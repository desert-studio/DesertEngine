#pragma once

#include <Engine/UI/Args/ArgKind.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// THE TREE THE UI FRAMEWORK READS, AND THE ONLY THING IT KNOWS ABOUT WHO STORES IT.
//
// Layout, picking and introspection used to ask `entt::registry` directly — which component an element
// carries, who its children are — so the framework could not exist without the ECS and could not be fed a
// tree from anywhere else (a widget host, a test that builds nodes by hand, a tool). This is the seam UE draws
// between UMG (UObjects authored in a level) and Slate (a widget tree the framework walks): the framework is
// handed a read view, the engine adapts its storage to it (Engine/UI/Ecs/EcsUITree).
//
// It is a VIEW, not a retained widget graph. There is no invalidation here (plan §0.1): a walk asks the
// tree every frame, exactly as the ECS walk did, so a frame drawn through it is the frame drawn before.
namespace Desert::UI
{
    // A node of the tree. Bit-for-bit an `entt::entity` in the ECS adapter, so converting is free and the
    // id a debugger prints for an element is the same id either side of the seam.
    enum class NodeId : std::uint32_t
    {
        Null = 0xFFFFFFFFu
    };

    class IUITree
    {
    public:
        virtual ~IUITree() = default;

        [[nodiscard]] virtual bool Valid( NodeId n ) const = 0;

        // An upper bound on the number of nodes. Bounds every ancestor walk: a Parent cycle is authorable
        // (a hierarchy panel can reparent), and an unbounded walk would hang instead of answering "none".
        [[nodiscard]] virtual std::size_t NodeBound() const = 0;

        [[nodiscard]] virtual NodeId Parent( NodeId n ) const = 0; // NodeId::Null for a root or an orphan

        // Children IN DRAW ORDER. Asked by index rather than handed out as a span: the ECS stores them as
        // `entt::entity`, and reading that storage through a NodeId would be an aliasing violation, not a
        // conversion. An invalid child may be listed — callers skip what Valid() refuses, as before.
        [[nodiscard]] virtual std::size_t ChildCount( NodeId n ) const             = 0;
        [[nodiscard]] virtual NodeId      ChildAt( NodeId n, std::size_t i ) const = 0;

        [[nodiscard]] virtual std::string_view Name( NodeId n ) const     = 0; // authored name, "" when none
        [[nodiscard]] virtual std::uint64_t    StableId( NodeId n ) const = 0; // survives save/load; 0 when none

        // The node's authored arguments of kind @p kind (the UI*Data whose `Arg` is @p kind), or nullptr.
        [[nodiscard]] virtual const void* Find( NodeId n, ArgKind kind ) const = 0;

        // The same arguments, writable — ONLY for the kinds a control writes its value back into (Toggle,
        // Slider, InputField, Dropdown, ScrollView, ListView). nullptr for every other kind, so a framework
        // walk cannot quietly edit layout or style through this door.
        [[nodiscard]] virtual void* FindState( NodeId n, ArgKind kind ) = 0;

        // Every node carrying @p kind (Canvas, Screen, Overlay …), in AUTHORED order — the order the scene
        // created them, which is the tie-break every "which comes first" rule above the tree relies on.
        virtual void Roots( ArgKind kind, std::vector<NodeId>& out ) const = 0;

        template <class T>
        [[nodiscard]] const T* Get( NodeId n ) const
        {
            return static_cast<const T*>( Find( n, T::Arg ) );
        }

        template <class T>
        [[nodiscard]] bool Has( NodeId n ) const
        {
            return Find( n, T::Arg ) != nullptr;
        }

        template <class T>
        [[nodiscard]] T* GetState( NodeId n )
        {
            return static_cast<T*>( FindState( n, T::Arg ) );
        }

    protected:
        IUITree()                            = default;
        IUITree( const IUITree& )            = default;
        IUITree& operator=( const IUITree& ) = default;
        IUITree( IUITree&& )                 = default;
        IUITree& operator=( IUITree&& )      = default;
    };
} // namespace Desert::UI
