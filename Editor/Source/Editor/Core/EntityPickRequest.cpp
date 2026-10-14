#include <Editor/Core/EntityPickRequest.hpp>

namespace Desert::Editor::EntityPickRequest
{
    namespace
    {
        struct State
        {
            std::optional<std::uint32_t> Armed;  // the field waiting for a click
            std::optional<std::uint32_t> Owner;  // the field an answer is held for
            Common::UUID                 Picked; // that answer
        };

        State& Get()
        {
            static State state;
            return state;
        }
    } // namespace

    void Arm( std::uint32_t owner )
    {
        Get() = State{ .Armed = owner, .Owner = std::nullopt, .Picked = Common::UUID() };
    }

    void Cancel()
    {
        Get() = State{};
    }

    bool IsArmed()
    {
        return Get().Armed.has_value();
    }

    bool IsArmedFor( std::uint32_t owner )
    {
        return Get().Armed == owner;
    }

    void Deliver( std::optional<Common::UUID> picked )
    {
        State& state = Get();
        if ( !state.Armed )
            return;
        state.Owner  = picked ? state.Armed : std::nullopt;
        state.Picked = picked.value_or( Common::UUID() );
        state.Armed.reset();
    }

    std::optional<Common::UUID> TakeFor( std::uint32_t owner )
    {
        State& state = Get();
        if ( state.Owner != owner )
            return std::nullopt;
        const Common::UUID picked = state.Picked;
        state                     = State{};
        return picked;
    }
} // namespace Desert::Editor::EntityPickRequest
