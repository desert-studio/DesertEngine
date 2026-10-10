// A mesh entity takes its mesh asset's materials PER SECTION (Engine/ECS/MaterialSlotAdoption.hpp; caller
// MeshECSSystem::AdoptMeshMaterialSlots, static and skinned). Pinned as relations between sections: one
// missing material leaves ONLY its own section on the default, and the material arriving later replaces
// that default without touching the sections already resolved.
//
// The defect it closes (BISTRO-LEAFCOLOR-b): the rule was all-or-nothing, so the cypress's one missing
// Foliage_Trunk.demat left every slot empty and the whole tree white.

#include <Engine/ECS/MaterialSlotAdoption.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace
{
    using ::Common::AssetHandle;
    using ::Common::UUID;

    // The material service as the adopter sees it: external id -> registered handle, Null while unregistered.
    struct FakeMaterialRegistry
    {
        std::map<uint64_t, uint64_t> Registered;
        int                          Lookups = 0;

        AssetHandle operator()( const UUID& id )
        {
            ++Lookups;
            const auto it = Registered.find( static_cast<uint64_t>( id ) );
            return it == Registered.end() ? AssetHandle() : AssetHandle( it->second );
        }
    };

    const std::vector<UUID> kSections = { UUID( 101ull ), UUID( 102ull ), UUID( 103ull ) }; // leaves, trunk, bark
} // namespace

// One section's material is missing: the other two get theirs, the missing one stays Null (= the default).
TEST( MaterialSlotAdoption, OneMissingSlotDoesNotWhitenTheOthers )
{
    FakeMaterialRegistry registry;
    registry.Registered = { { 101, 9001 }, { 103, 9003 } }; // 102 (the trunk) never registered

    std::vector<AssetHandle> slots;
    EXPECT_TRUE( Desert::ECS::AdoptSectionMaterials( slots, kSections, std::ref( registry ) ) );

    ASSERT_EQ( slots.size(), kSections.size() );
    EXPECT_EQ( static_cast<uint64_t>( slots[0] ), 9001u );
    EXPECT_TRUE( slots[1].IsNull() );
    EXPECT_EQ( static_cast<uint64_t>( slots[2] ), 9003u );
    EXPECT_TRUE( Desert::ECS::HasUnadoptedSlot( slots ) ); // still pending: it is tried again
}

// The missing material registers later: its section takes it, the resolved sections are not looked up again.
TEST( MaterialSlotAdoption, LateLoadReplacesTheDefault )
{
    FakeMaterialRegistry registry;
    registry.Registered = { { 101, 9001 }, { 103, 9003 } };

    std::vector<AssetHandle> slots;
    Desert::ECS::AdoptSectionMaterials( slots, kSections, std::ref( registry ) );

    // A frame with nothing new: no change reported, so the cached instances are kept.
    registry.Lookups = 0;
    EXPECT_FALSE( Desert::ECS::AdoptSectionMaterials( slots, kSections, std::ref( registry ) ) );
    EXPECT_EQ( registry.Lookups, 1 ); // only the pending section

    registry.Registered[102] = 9002;
    EXPECT_TRUE( Desert::ECS::AdoptSectionMaterials( slots, kSections, std::ref( registry ) ) );
    EXPECT_EQ( static_cast<uint64_t>( slots[0] ), 9001u );
    EXPECT_EQ( static_cast<uint64_t>( slots[1] ), 9002u );
    EXPECT_EQ( static_cast<uint64_t>( slots[2] ), 9003u );
    EXPECT_FALSE( Desert::ECS::HasUnadoptedSlot( slots ) );
}

// An authored slot is never replaced by the mesh's material; only Null slots adopt.
TEST( MaterialSlotAdoption, AuthoredSlotIsKept )
{
    FakeMaterialRegistry registry;
    registry.Registered = { { 101, 9001 }, { 102, 9002 }, { 103, 9003 } };

    std::vector<AssetHandle> slots = { AssetHandle( 7777ull ), AssetHandle(), AssetHandle( 8888ull ) };
    EXPECT_TRUE( Desert::ECS::AdoptSectionMaterials( slots, kSections, std::ref( registry ) ) );
    EXPECT_EQ( static_cast<uint64_t>( slots[0] ), 7777u );
    EXPECT_EQ( static_cast<uint64_t>( slots[1] ), 9002u );
    EXPECT_EQ( static_cast<uint64_t>( slots[2] ), 8888u );
}
