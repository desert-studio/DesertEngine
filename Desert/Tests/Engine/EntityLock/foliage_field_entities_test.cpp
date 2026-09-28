// FO-9: a foliage field's realized prefab instances are hidden from the outliner and a viewport hit on any
// part of one promotes to the field. The two predicates both callers use, over a hand-built registry.

#include <gtest/gtest.h>

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/FoliageFieldEntities.hpp>

#include <entt/entt.hpp>

using Desert::ECS::FoliageComponent;
using Desert::ECS::HidesChildrenFromOutliner;
using Desert::ECS::OwningFoliageField;
using Desert::ECS::RelationshipComponent;

namespace
{
    //     field (FoliageComponent)
    //      +- instance (prefab root)
    //          +- part
    //     prop
    //      +- propChild
    struct FieldFixture
    {
        entt::registry Registry;
        entt::entity   Field     = Make();
        entt::entity   Instance  = Make();
        entt::entity   Part      = Make();
        entt::entity   Prop      = Make();
        entt::entity   PropChild = Make();

        FieldFixture()
        {
            Registry.emplace<FoliageComponent>( Field );
            Attach( Field, Instance );
            Attach( Instance, Part );
            Attach( Prop, PropChild );
        }

        entt::entity Make()
        {
            const entt::entity e = Registry.create();
            Registry.emplace<RelationshipComponent>( e );
            return e;
        }

        void Attach( entt::entity parent, entt::entity child )
        {
            Registry.get<RelationshipComponent>( parent ).Children.push_back( child );
            Registry.get<RelationshipComponent>( child ).Parent = parent;
        }
    };
} // namespace

TEST( FoliageFieldEntities, TheOutlinerHidesAFieldsChildrenAndNoOtherEntitys )
{
    FieldFixture f;
    EXPECT_TRUE( HidesChildrenFromOutliner( f.Registry, f.Field ) );
    EXPECT_FALSE( HidesChildrenFromOutliner( f.Registry, f.Prop ) );
    EXPECT_FALSE( HidesChildrenFromOutliner( f.Registry, f.Instance ) );
    EXPECT_FALSE( HidesChildrenFromOutliner( f.Registry, entt::null ) );
}

TEST( FoliageFieldEntities, AHitOnAnyPartOfAPlacedInstanceIsOwnedByTheField )
{
    FieldFixture f;
    EXPECT_EQ( OwningFoliageField( f.Registry, f.Instance ), f.Field );
    EXPECT_EQ( OwningFoliageField( f.Registry, f.Part ), f.Field ) << "a submesh of an instance must promote too";

    // The field itself, and entities outside any field, keep their own selection.
    EXPECT_EQ( OwningFoliageField( f.Registry, f.Field ), entt::entity{ entt::null } );
    EXPECT_EQ( OwningFoliageField( f.Registry, f.Prop ), entt::entity{ entt::null } );
    EXPECT_EQ( OwningFoliageField( f.Registry, f.PropChild ), entt::entity{ entt::null } );
}
