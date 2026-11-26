#pragma once

#include <rflcpp/rfl/json.hpp>

#include <optional>
#include <string>
#include <utility>

// The record edits every scene-upgrade step shares. Internal to Core/SceneUpgrade/Steps.
namespace Desert::Core::UpgradeEdits
{
    // Applies @p edit to the object block @p component of @p components, if it is stated as an object.
    template <typename Edit>
    void EditBlock( rfl::ExtraFields<rfl::Generic>& components, const char* component, Edit&& edit )
    {
        const auto payload = components.get( component );
        if ( !payload.has_value() )
            return;
        auto fields = payload.value().to_object();
        if ( !fields.has_value() )
            return;
        rfl::Generic::Object block = std::move( fields.value() );
        if ( edit( block ) )
            components[component] = rfl::Generic( std::move( block ) );
    }

    // @p block without @p key; true when the key was there.
    inline bool DropKey( rfl::Generic::Object& block, const std::string& key )
    {
        if ( !block.get( key ).has_value() )
            return false;
        rfl::Generic::Object kept;
        for ( const auto& [name, value] : block )
            if ( name != key )
                kept[name] = value;
        block = std::move( kept );
        return true;
    }

    // A stated JSON number (a float, or an integer such as `0`) as a double; nullopt for anything else.
    inline std::optional<double> NumberOf( const rfl::Generic& value )
    {
        if ( const auto real = value.to_double(); real.has_value() )
            return real.value();
        if ( const auto whole = value.to_int(); whole.has_value() )
            return static_cast<double>( whole.value() );
        return std::nullopt;
    }
} // namespace Desert::Core::UpgradeEdits
