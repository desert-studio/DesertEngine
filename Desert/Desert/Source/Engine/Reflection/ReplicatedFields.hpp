#pragma once

#include <Engine/Reflection/ReflectionTypes.hpp>

#include <span>
#include <string_view>

namespace Desert::Reflection
{
    // THE REPLICATION LAYOUT (UE: the replicated properties UHT lists per class, FRepLayout), render-free. One row per
    // reflected field marked PROPERTY(Replicated) of a COMPONENT block, named as the wire names it: the block's record
    // key ("DirectionLight") and the field. DesertHeaderTool emits the rows (Engine/Generated/ReplicatedFields.gen.cpp,
    // compiled in CoreReflection), so a process that links no component header — the dedicated server — still knows
    // which fields it may replicate and refuses a write to any other by name.
    struct ReplicatedField
    {
        std::string_view Component; // the block's record key
        std::string_view Field;     // the reflected field's name
        FieldType        Type = FieldType::Unknown;
    };

    // Every replicated field of every component block, ordered by component key, then field.
    std::span<const ReplicatedField> ReplicatedFields();

    // The row of `component`.`field`, or null when that field is not marked Replicated (or does not exist).
    const ReplicatedField* FindReplicatedField( std::string_view component, std::string_view field );
} // namespace Desert::Reflection
