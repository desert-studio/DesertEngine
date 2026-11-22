#include <Engine/Core/Serialize/ComponentRegistration.hpp>

// THE SERVER'S COMPONENT PROVIDER (Core/Serialize/ComponentRegistration.hpp, ComponentProvider). The dedicated
// server links Core and its closure only (Server/CMakeLists.txt), so it registers the generated rows of exactly
// those modules, in module-table order (cmake/DesertModuleTable.cmake), and no hand-written serializer: those are
// render-side and Engine's (Engine/Core/Serialize/EngineComponentProvider.cpp). A block of a component the server
// does not link is a foreign key to it (ForeignKeys), kept and written back unread, as UE's server skips the
// classes of a client-only module.

namespace Desert::Core::Serialize::Generated
{
    void RegisterComponents_Common( ComponentRegistrar& r );
    void RegisterComponents_CoreReflection( ComponentRegistrar& r );
    void RegisterComponents_Projects( ComponentRegistrar& r );
    void RegisterComponents_GeometryCore( ComponentRegistrar& r );
    void RegisterComponents_AssetCore( ComponentRegistrar& r );
    void RegisterComponents_Core( ComponentRegistrar& r );
} // namespace Desert::Core::Serialize::Generated

namespace Desert::Server
{
    namespace
    {
        void RegisterServerRows( Core::Serialize::ComponentRegistrar& r )
        {
            using namespace Core::Serialize::Generated;
            RegisterComponents_Common( r );
            RegisterComponents_CoreReflection( r );
            RegisterComponents_Projects( r );
            RegisterComponents_GeometryCore( r );
            RegisterComponents_AssetCore( r );
            RegisterComponents_Core( r );
        }

        [[maybe_unused]] const bool kInstalled =
             ( Core::Serialize::InstallComponentProvider( { &RegisterServerRows, nullptr } ), true );
    } // namespace
} // namespace Desert::Server
