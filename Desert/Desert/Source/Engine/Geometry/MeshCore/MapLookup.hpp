#pragma once

namespace Desert::Geometry
{
    // Pointer-or-null lookup: std::unordered_map offers only iterators, and the mesh code reads "absent" as a
    // null pointer inside if-conditions. Const-ness follows the map (a const map yields a pointer to const).
    template <typename MapType, typename KeyType>
    auto* FindValue( MapType& Map, const KeyType& Key )
    {
        auto It = Map.find( Key );
        return It == Map.end() ? nullptr : &It->second;
    }
} // namespace Desert::Geometry
