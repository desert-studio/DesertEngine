// ReflectedComponents — the component <-> reflected-type table any by-name consumer (scripts) reaches components
// through, built from the serializer's list (Core::Serialize::ForEachReflectedComponentBlock).
//
// Relations pinned: every serializer row is a table row under the same key and the same type (one list, not two);
// a row's Data is the member the serializer reads (or the whole component for a whole block); a write announced
// through NotifyChanged reaches registry.on_update<T>.

#include <Engine/Core/Serialize/ReflectedComponentBlocks.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/ReflectedComponents.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{
    namespace ECS = Desert::ECS;

    int  gPointLightUpdates = 0;
    void CountPointLightUpdate( entt::registry&, entt::entity )
    {
        ++gPointLightUpdates;
    }
} // namespace

TEST( ReflectedComponents, EverySerializerRowIsATableRowUnderTheSameKeyAndType )
{
    std::vector<std::pair<std::string, std::string>> serializer;
    Desert::Core::Serialize::ForEachReflectedComponentBlock(
         [&serializer]( const auto& row ) { serializer.emplace_back( row.Key, row.TypeName ); } );

    const auto all = ECS::AllReflectedComponents();
    ASSERT_EQ( all.size(), serializer.size() );
    for ( std::size_t i = 0; i < serializer.size(); ++i )
    {
        SCOPED_TRACE( serializer[i].first );
        EXPECT_EQ( all[i].Name, serializer[i].first );
        EXPECT_EQ( all[i].TypeName, serializer[i].second );
        EXPECT_EQ( ECS::FindReflectedComponent( serializer[i].first ), &all[i] );
        const auto* type = all[i].Type();
        ASSERT_NE( type, nullptr ) << "reflection registry has no " << serializer[i].second;
        EXPECT_EQ( type->Name, serializer[i].second );
    }
    EXPECT_EQ( ECS::FindReflectedComponent( "NoSuchComponent" ), nullptr );
}

TEST( ReflectedComponents, DataIsTheMemberTheSerializerReads )
{
    entt::registry     r;
    const entt::entity e = r.create();

    const auto* light = ECS::FindReflectedComponent( "PointLight" );
    ASSERT_NE( light, nullptr );
    EXPECT_FALSE( light->Has( r, e ) );
    light->Add( r, e );
    ASSERT_TRUE( light->Has( r, e ) );
    EXPECT_EQ( light->Data( r, e ), static_cast<void*>( &r.get<ECS::PointLightComponent>( e ).Data ) );
    light->Add( r, e ); // present: no-op, the data survives
    EXPECT_TRUE( light->Has( r, e ) );
    light->Remove( r, e );
    EXPECT_FALSE( light->Has( r, e ) );
    light->Remove( r, e ); // absent: no-op

    const auto* skybox = ECS::FindReflectedComponent( "Skybox" ); // a WHOLE block: the component is the data
    ASSERT_NE( skybox, nullptr );
    skybox->Add( r, e );
    EXPECT_EQ( skybox->Data( r, e ), static_cast<void*>( &r.get<ECS::SkyboxComponent>( e ) ) );
}

TEST( ReflectedComponents, NotifyChangedReachesOnUpdate )
{
    entt::registry     r;
    const entt::entity e = r.create();
    gPointLightUpdates   = 0;
    r.on_update<ECS::PointLightComponent>().connect<&CountPointLightUpdate>();

    const auto* light = ECS::FindReflectedComponent( "PointLight" );
    ASSERT_NE( light, nullptr );
    light->Add( r, e );
    EXPECT_EQ( gPointLightUpdates, 0 ); // adding is construction, not an edit
    light->NotifyChanged( r, e );
    EXPECT_EQ( gPointLightUpdates, 1 );

    // Another component's notification is not this one's.
    const auto* spot = ECS::FindReflectedComponent( "SpotLight" );
    ASSERT_NE( spot, nullptr );
    spot->Add( r, e );
    spot->NotifyChanged( r, e );
    EXPECT_EQ( gPointLightUpdates, 1 );
}
