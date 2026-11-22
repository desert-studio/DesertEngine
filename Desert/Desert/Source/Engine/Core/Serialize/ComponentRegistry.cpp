#include "ComponentRegistry.hpp"

#include <Common/Core/Logger.hpp>
#include <Engine/Core/Serialize/ComponentRegistration.hpp>

#include <algorithm>
#include <utility>

// THE REGISTRY TYPE AND THE LOAD ORDER — NOTHING ELSE (BUILD-MOD-5). This file used to be the engine's whole
// component table (2400 lines, every component header of every module): a field added to any component
// recompiled it. Now:
//   * reflected blocks are registered by their own module (Engine/Generated/Components_<Module>.gen.cpp,
//     ComponentRegistration.hpp) and read here through ModuleComponents();
//   * hand-written blocks live by subject in *ComponentSerializers.cpp (BuiltinComponentSerializers.hpp), ordered
//     by Engine's provider (EngineComponentProvider.cpp): this file is Core's and names neither (the server links
//     it);
//   * the AssetResolver is AssetResolver.cpp.

namespace Desert::Core::Serialize
{
    namespace
    {
        struct ProviderSlot
        {
            ComponentProvider Provider;
            bool              Installed = false;
            bool              Read      = false; // the registry (or ModuleComponents) was built from it
        };

        ProviderSlot& Slot()
        {
            static ProviderSlot slot;
            return slot;
        }

        const ComponentProvider& ReadProvider()
        {
            ProviderSlot& slot = Slot();
            if ( !slot.Installed )
                LOG_ERROR( "[Components] no component provider is installed: the registry knows no component (the "
                           "executable's top module installs one, InstallComponentProvider)" );
            slot.Read = true;
            return slot.Provider;
        }
    } // namespace

    void InstallComponentProvider( const ComponentProvider& provider )
    {
        ProviderSlot& slot = Slot();
        if ( slot.Installed && slot.Provider.RegisterRows == provider.RegisterRows &&
             slot.Provider.Order == provider.Order )
            return; // the same provider again (its module's anchor and its static registration both install it)
        if ( slot.Installed || slot.Read )
        {
            LOG_ERROR( "[Components] a component provider is installed {}: refused",
                       slot.Read ? "after the registry was built" : "twice" );
            return;
        }
        slot.Provider  = provider;
        slot.Installed = true;
    }

    const std::vector<ModuleComponent>& ModuleComponents()
    {
        static const std::vector<ModuleComponent> rows = []
        {
            ComponentRegistrar registrar;
            if ( const ComponentProvider& provider = ReadProvider(); provider.RegisterRows != nullptr )
                provider.RegisterRows( registrar );
            std::vector<ModuleComponent> all = registrar.Take();
            // Key order: the order the one generated list had, so a run registers its rows as it always did.
            std::stable_sort( all.begin(), all.end(), []( const ModuleComponent& a, const ModuleComponent& b )
                              { return a.Serializer.Key < b.Serializer.Key; } );
            return all;
        }();
        return rows;
    }

    const ComponentRegistry& ComponentRegistry::Get()
    {
        static ComponentRegistry instance;
        return instance;
    }

    ComponentRegistry::ComponentRegistry()
    {
        RegisterBuiltins();
        for ( const auto& serializer : m_Serializers )
            m_InFileOrder.push_back( &serializer );
        std::sort( m_InFileOrder.begin(), m_InFileOrder.end(),
                   []( const ComponentSerializer* a, const ComponentSerializer* b ) { return a->Key < b->Key; } );
    }

    void ComponentRegistry::Register( ComponentSerializer serializer )
    {
        m_Serializers.push_back( std::move( serializer ) );
    }

    void ComponentRegistry::RegisterBuiltins()
    {
        const std::vector<ModuleComponent>& rows     = ModuleComponents();
        const ComponentProvider&            provider = ReadProvider();
        if ( provider.Order == nullptr )
        {
            for ( const ModuleComponent& row : rows )
                Register( row.Serializer );
            return;
        }
        std::vector<ComponentSerializer> ordered;
        provider.Order( rows, ordered );
        for ( ComponentSerializer& s : ordered )
            Register( std::move( s ) );
    }
} // namespace Desert::Core::Serialize
