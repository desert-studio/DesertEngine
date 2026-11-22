#pragma once

// PER-MODULE COMPONENT REGISTRATION (BUILD-MOD-5) — the UE shape: UHT emits one registration per module and the
// module hands its own classes over; there is no central list that names every module's component.
//
// A component states its row once, on its struct: COMPONENT( Key( "..." ), Block( Member ) | Whole, Run( ... ) ).
// DesertHeaderTool emits, per module of cmake/DesertModuleTable.cmake,
//
//   Engine/Generated/Components_<Module>.gen.cpp   void RegisterComponents_<Module>( ComponentRegistrar& )
//                                                   — includes ONLY that module's component headers and calls
//                                                   ComponentRegistrar::Add once per row;
//   Engine/Generated/Components.gen.cpp            RegisterModuleComponents: every module's function, in the
//                                                   table's order (dependencies first).
//
// Each Components_<Module>.gen.cpp is placed in its module (cmake/DesertModules.cmake), so a field added to
// DestructibleComponent recompiles Components_Destruction.gen.cpp and the Destruction files that include the
// header — not ComponentRegistry.cpp, ECS/ReflectedComponents.cpp or SaveGame.cpp, which read the rows through
// ModuleComponents() below and include no component header.
//
// A row becomes BOTH things the engine needs of it, built here once from the typed row: the scene serializer
// (MakeReflectedBlock) and the type-erased ECS accessor scripts and save games use (MakeReflectedComponent).

#include <Engine/Core/Serialize/ComponentRegistry.hpp>
#include <Engine/Core/Serialize/ReflectedComponentRows.hpp>
#include <Engine/ECS/ReflectedComponents.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>

#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Desert::Core::Serialize
{
    // Builds a handler for a component whose serializable payload is a reflected data block. Adding a PROPERTY
    // field to that block automatically extends serialization — no code change here.
    template <class TComponent, class TData>
    ComponentSerializer MakeReflected( std::string key, std::string typeName, TData TComponent::*member )
    {
        ComponentSerializer s;
        s.Key = std::move( key );
        s.Has = []( ECS::Entity e ) { return e.HasComponent<TComponent>(); };

        // The AssetResolver is passed on BOTH directions, exactly as MakeReflectedSelf passes it. It is a no-op
        // for a component with no PROPERTY(Asset<...>) field, and load-bearing for the ones that have one
        // (UIImage's Sprite, UIText's Font, UIPanel's Video). Without it an AssetHandle field is written as a raw
        // 64-bit id and read back as one: the id is minted at load time from the file path, so it does not
        // survive a restart, and the component comes back pointing at nothing with no error anywhere.
        s.Serialize = [member, typeName]( ECS::Entity e, const Assets::AssetManager& mgr ) -> Common::Json::Value
        {
            const auto* type = Reflection::ReflectionRegistry::Get().Find( typeName );
            if ( !type )
                return { Common::Json::Object{} };
            const auto& comp     = e.GetComponent<TComponent>();
            auto        resolver = MakeAssetResolver( mgr );
            return Reflection::SerializeReflected( *type, &( comp.*member ), &resolver );
        };

        s.Deserialize = [member, typeName]( ECS::Entity e, const Common::Json::Node& block,
                                            const Assets::AssetManager& mgr, Common::Json::Issues& issues )
        {
            const auto* type = Reflection::ReflectionRegistry::Get().Find( typeName );
            if ( !type )
                return;
            if ( block.ExpectKind( Common::Json::Kind::Object, issues ) )
            {
                auto& comp =
                     e.HasComponent<TComponent>() ? e.GetComponent<TComponent>() : e.AddComponent<TComponent>();
                auto resolver = MakeAssetResolver( mgr );
                Reflection::DeserializeReflected( *type, &( comp.*member ), block, issues, &resolver );
            }
        };

        return s;
    }

    // Like MakeReflected, but reflects the WHOLE component (no Data sub-member) and threads an AssetResolver so
    // AssetHandle fields round-trip as paths (Skybox).
    template <class TComponent>
    ComponentSerializer MakeReflectedSelf( std::string key, std::string typeName )
    {
        ComponentSerializer s;
        s.Key = std::move( key );
        s.Has = []( ECS::Entity e ) { return e.HasComponent<TComponent>(); };

        s.Serialize = [typeName]( ECS::Entity e, const Assets::AssetManager& mgr ) -> Common::Json::Value
        {
            const auto* type = Reflection::ReflectionRegistry::Get().Find( typeName );
            if ( !type )
                return { Common::Json::Object{} };
            auto resolver = MakeAssetResolver( mgr );
            return Reflection::SerializeReflected( *type, &e.GetComponent<TComponent>(), &resolver );
        };

        s.Deserialize = [typeName]( ECS::Entity e, const Common::Json::Node& block,
                                    const Assets::AssetManager& mgr, Common::Json::Issues& issues )
        {
            const auto* type = Reflection::ReflectionRegistry::Get().Find( typeName );
            if ( !type )
                return;
            if ( block.ExpectKind( Common::Json::Kind::Object, issues ) )
            {
                auto  resolver = MakeAssetResolver( mgr );
                auto& comp =
                     e.HasComponent<TComponent>() ? e.GetComponent<TComponent>() : e.AddComponent<TComponent>();
                Reflection::DeserializeReflected( *type, &comp, block, issues, &resolver );
            }
        };

        return s;
    }

    // One row as the serializer its shape needs.
    template <class TComponent, class TData>
    ComponentSerializer MakeReflectedBlock( const ReflectedMemberBlock<TComponent, TData>& row )
    {
        return MakeReflected<TComponent, TData>( row.Key, row.TypeName, row.Member );
    }
    template <class TComponent>
    ComponentSerializer MakeReflectedBlock( const ReflectedWholeBlock<TComponent>& row )
    {
        return MakeReflectedSelf<TComponent>( row.Key, row.TypeName );
    }

    // The block's reflected struct inside its component: the component itself (Whole) or one member of it.
    template <class TComponent>
    void* ReflectedDataOf( TComponent& component, const ReflectedWholeBlock<TComponent>& )
    {
        return &component;
    }
    template <class TComponent, class TData>
    void* ReflectedDataOf( TComponent& component, const ReflectedMemberBlock<TComponent, TData>& row )
    {
        return &( component.*row.Member );
    }

    // One row as the type-erased ECS accessor. The member pointer of a row is a value, not a type, so the Data
    // accessor is a lambda over a per-row static; one instantiation per component type (a component appears in
    // the list once).
    template <class TRow>
    ECS::ReflectedComponent MakeReflectedComponent( const TRow& row )
    {
        using TComponent       = typename TRow::Component;
        static const TRow kRow = row;

        ECS::ReflectedComponent out;
        out.Name     = row.Key;
        out.TypeName = row.TypeName;
        out.Has      = []( entt::registry& r, entt::entity e ) { return r.has<TComponent>( e ); };
        out.Data     = []( entt::registry& r, entt::entity e ) -> void*
        { return ReflectedDataOf( r.get<TComponent>( e ), kRow ); };
        out.Add = []( entt::registry& r, entt::entity e )
        {
            if ( !r.has<TComponent>( e ) )
                r.emplace<TComponent>( e );
        };
        out.Remove = []( entt::registry& r, entt::entity e )
        {
            if ( r.has<TComponent>( e ) )
                r.remove<TComponent>( e );
        };
        out.NotifyChanged = []( entt::registry& r, entt::entity e ) { r.patch<TComponent>( e ); };
        out.Prepare       = []( entt::registry& r ) { r.prepare<TComponent>(); };
        return out;
    }

    // One registered row: what the engine reads of a module's component.
    struct ModuleComponent
    {
        ECS::ReflectedComponent Component;
        ComponentSerializer     Serializer;
        ReflectedBlockRun       Run = ReflectedBlockRun::ActorsAndUI;
    };

    // What a module's generated RegisterComponents_<Module>() hands its rows to.
    class ComponentRegistrar
    {
    public:
        template <class TRow>
        void Add( const TRow& row )
        {
            m_Rows.push_back( { MakeReflectedComponent( row ), MakeReflectedBlock( row ), row.Run } );
        }

        [[nodiscard]] std::vector<ModuleComponent> Take()
        {
            return std::move( m_Rows );
        }

    private:
        std::vector<ModuleComponent> m_Rows;
    };

    // Every module's rows, built once on first use: the generated RegisterModuleComponents (module-table order),
    // then ordered by key — the order the single generated list had, so load order within a run is unchanged.
    const std::vector<ModuleComponent>& ModuleComponents();

    // THE EXECUTABLE'S COMPONENT PROVIDER (UE: the modules an executable links register their component classes at
    // StartupModule; the dedicated server links fewer modules and so knows fewer classes). This registry is Core's,
    // compiled into the render-free server too, so it names no module's rows and no hand-written serializer: the
    // executable's top module installs them. Engine installs every module's generated rows and its hand-written
    // serializers (Engine/Core/Serialize/EngineComponentProvider.cpp); DesertServer installs the rows of Core's
    // closure (Server/Source/ServerComponents.cpp). One provider per process; the registry is built on first use.
    struct ComponentProvider
    {
        // Every linked module's generated rows (Engine/Generated/Components.gen.cpp's shape).
        void ( *RegisterRows )( ComponentRegistrar& registrar ) = nullptr;
        // The registry in load order: the hand-written serializers interleaved with @p rows run by run. Null: the
        // rows alone, in key order.
        void ( *Order )( std::span<const ModuleComponent> rows, std::vector<ComponentSerializer>& out ) = nullptr;
    };

    /// Installs the process's provider, before the first ComponentRegistry::Get / ModuleComponents. A second,
    /// different provider, or one after the registry was built, is refused loudly (nothing is registered twice).
    void InstallComponentProvider( const ComponentProvider& provider );

    namespace Generated
    {
        // Engine/Generated/Components.gen.cpp (Engine's provider calls it; Core does not).
        void RegisterModuleComponents( ComponentRegistrar& registrar );
    } // namespace Generated
} // namespace Desert::Core::Serialize
