#pragma once

#include <Engine/Core/Serialize/ComponentRegistration.hpp>

#include <cstdint>

namespace Desert::Core::Serialize
{
    /// Where a feature module's hand-written serializer sits in the registry's load order: before or after the
    /// generated rows of its run, in the order the module registered it.
    enum class ModuleSerializerSlot : uint8_t
    {
        BeforeRun,
        AfterRun,
    };

    /// A FEATURE MODULE'S HAND-WRITTEN SERIALIZER (UE: a plugin's component class with its own Serialize, known to
    /// the engine because the plugin's module registered it at StartupModule). Its file form is not its reflection
    /// alone (the landscape's root, tile and spline), so it is no generated row; the module hands it over here and
    /// Engine's provider places it beside its run. Engine names no module's component.
    struct ModuleSerializer
    {
        ReflectedBlockRun    Run        = ReflectedBlockRun::ActorsAndUI;
        ModuleSerializerSlot Slot       = ModuleSerializerSlot::AfterRun;
        ComponentSerializer ( *Make )() = nullptr;
    };

    /// Called from a module's StartupModule (Engine/Core/Modules.cpp), which the provider runs before it builds
    /// the registry. One registered after the registry was built is refused loudly: it could not be placed.
    void RegisterModuleSerializer( const ModuleSerializer& serializer );

    /// Hands Engine the executable's generated module list (Components.gen.cpp calls it at static initialisation;
    /// UE: the target's module list). Engine names no module's generated rows; one linked after the registry was
    /// built is refused loudly.
    void LinkModuleComponentRows( void ( *registerRows )( ComponentRegistrar& ) );

    /// Installs Engine's component provider (idempotent; also done at static initialisation). Scene calls it, so
    /// every executable with a Scene links the provider (EngineComponentProvider.cpp).
    void LinkEngineComponentProvider();
} // namespace Desert::Core::Serialize
