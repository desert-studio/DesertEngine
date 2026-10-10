// ONE MISSING MATERIAL WHITENS ONLY ITS OWN SECTION (UE: UMeshComponent::GetMaterial per section, a missing one
// drawing UMaterial::GetDefaultMaterial). The defect this suite exists for (BISTRO-LEAFCOLOR-b): the slot adoption
// was all-or-nothing, so a Bistro cypress whose trunk material had no content-registry row left EVERY slot empty
// and the whole tree - leaves included - drew the white Default Surface without its alpha cut.
//
// No GPU: FillSectionMaterials is the rule MeshECSSystem::AdoptMeshMaterialSlots runs, with the material service's
// external->handle lookup passed in.

#include <Engine/ECS/System/MeshSectionMaterials.hpp>

#include <gtest/gtest.h>

#include <unordered_map>

using namespace Desert;

namespace
{
    const Common::UUID        kLeavesId{ 0x1111ull };
    const Common::UUID        kTrunkId{ 0x2222ull };
    const Assets::AssetHandle kLeaves{ 0xAAAAull };
    const Assets::AssetHandle kTrunk{ 0xBBBBull };

    struct Registry
    {
        std::unordered_map<uint64_t, Assets::AssetHandle> Rows;
        Assets::AssetHandle                               operator()( const Common::UUID& id ) const
        {
            const auto it = Rows.find( static_cast<uint64_t>( id ) );
            return it == Rows.end() ? Assets::AssetHandle{} : it->second;
        }
    };
} // namespace

TEST( MeshSectionMaterials, AMissingSectionLeavesOnlyItsOwnSlotDefault )
{
    Registry registry;
    registry.Rows[static_cast<uint64_t>( kLeavesId )] = kLeaves; // the trunk has no row

    std::vector<Assets::AssetHandle> slots;
    const auto                       fill = ECS::FillSectionMaterials( slots, { kLeavesId, kTrunkId }, registry );

    ASSERT_EQ( slots.size(), 2u ) << "one slot per mesh section, resolved or not";
    EXPECT_EQ( slots[0], kLeaves ) << "the resolved section keeps its own material";
    EXPECT_TRUE( slots[1].IsNull() ) << "only the unresolved section falls to the default surface";
    EXPECT_TRUE( fill.Changed );
    ASSERT_EQ( fill.Unresolved.size(), 1u );
    EXPECT_EQ( fill.Unresolved[0], 1u );
}

TEST( MeshSectionMaterials, ALateRegisteredMaterialArrivesOnTheNextPass )
{
    Registry registry;
    registry.Rows[static_cast<uint64_t>( kLeavesId )] = kLeaves;
    std::vector<Assets::AssetHandle> slots;
    (void)ECS::FillSectionMaterials( slots, { kLeavesId, kTrunkId }, registry );

    registry.Rows[static_cast<uint64_t>( kTrunkId )] = kTrunk;
    const auto again = ECS::FillSectionMaterials( slots, { kLeavesId, kTrunkId }, registry );

    EXPECT_TRUE( again.Changed ) << "the caller must rebuild its instances when a section resolves";
    EXPECT_TRUE( again.Unresolved.empty() );
    EXPECT_EQ( slots[0], kLeaves );
    EXPECT_EQ( slots[1], kTrunk );

    const auto settled = ECS::FillSectionMaterials( slots, { kLeavesId, kTrunkId }, registry );
    EXPECT_FALSE( settled.Changed ) << "a fully resolved component is left alone";
}

TEST( MeshSectionMaterials, ASlotTheComponentNamesIsNeverReplaced )
{
    Registry registry;
    registry.Rows[static_cast<uint64_t>( kLeavesId )] = kLeaves;
    registry.Rows[static_cast<uint64_t>( kTrunkId )]  = kTrunk;
    const Assets::AssetHandle authored{ 0xCCCCull };

    std::vector<Assets::AssetHandle> slots{ authored, Assets::AssetHandle{} };
    const auto                       fill = ECS::FillSectionMaterials( slots, { kLeavesId, kTrunkId }, registry );

    EXPECT_EQ( slots[0], authored ) << "the component's own material wins over the mesh's";
    EXPECT_EQ( slots[1], kTrunk ) << "an empty slot takes the mesh's section material";
    EXPECT_TRUE( fill.Changed );
}
