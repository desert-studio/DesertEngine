#include <Engine/Core/Serialize/EngineComponentProvider.hpp>

#include <Engine/Core/Serialize/BuiltinComponentSerializers.hpp>
#include <Engine/Core/Serialize/ComponentRegistration.hpp>
#include <Engine/Core/ModuleHooks.hpp>

#include <Common/Core/Logger.hpp>

#include <span>
#include <vector>

// ENGINE'S COMPONENT PROVIDER (Core/Serialize/ComponentRegistration.hpp, ComponentProvider). The registry is
// Core's and links into the render-free server; the rows of every module Engine links and the hand-written
// serializers (BuiltinComponentSerializers.hpp, render-side: meshes, landscape, scripts) are installed from here,
// in the load order the registry always had. Installed during static initialisation, and again (a no-op) by
// LinkEngineComponentProvider(), which Scene calls: that reference is what links this object into every executable
// that has a Scene (a static library drops an object nothing names).

namespace Desert::Core::Serialize
{
    namespace
    {
        struct ModuleSerializerList
        {
            std::vector<ModuleSerializer> Registered;
            bool                          Ordered = false; // the registry was built: nothing more can be placed
        };

        ModuleSerializerList& ModuleSerializers()
        {
            static ModuleSerializerList list;
            return list;
        }

        void Order( std::span<const ModuleComponent> rows, std::vector<ComponentSerializer>& out )
        {
            // The feature modules register their hand-written serializers at StartupModule: started first (a
            // no-op once done), so the order never depends on which static initialiser ran first.
            StartupEngineModules();
            ModuleSerializers().Ordered = true;

            std::vector<ComponentSerializer>& builtins = out; // the registry's load order itself
            const auto moduleSerializers               = [&out]( ReflectedBlockRun run, ModuleSerializerSlot slot )
            {
                for ( const ModuleSerializer& serializer : ModuleSerializers().Registered )
                    if ( serializer.Run == run && serializer.Slot == slot )
                        out.push_back( serializer.Make() );
            };
            // ---- Reflected data blocks: every module's generated rows (COMPONENT markers), run by run, each
            // between the module serializers registered before and after it ----
            const auto registerRun = [&out, rows, &moduleSerializers]( ReflectedBlockRun run )
            {
                moduleSerializers( run, ModuleSerializerSlot::BeforeRun );
                for ( const ModuleComponent& row : rows )
                    if ( row.Run == run )
                        out.push_back( row.Serializer );
                moduleSerializers( run, ModuleSerializerSlot::AfterRun );
            };

            Builtins::AddMeshSerializers( builtins );
            Builtins::AddActorSerializers( builtins );
            registerRun( ReflectedBlockRun::ActorsAndUI );
            Builtins::AddUIRenderTextureSerializer( builtins );
            registerRun( ReflectedBlockRun::UIAfterRenderTexture );
            Builtins::AddEditorAndSequenceSerializers( builtins );
            Builtins::AddFoliageSerializers( builtins );

            // ---- Hand-mapped authored blocks (no reflected Data; see AuthoredComponentIO.hpp) ----
            // U13. Five components with a full Details editor and no row in this table: everything the
            // artist typed into them was discarded by the next load, and by every Ctrl+C, every delete-undo,
            // every prefab instancing and every Play/Stop, because all of those are this same registry.
            // Desert/Tests/Engine/ComponentPersistence now derives "is authored in Details" from the editor's
            // own registration source and refuses to let a sixth one exist. No version bump: an added key is
            // what ForeignKeys is for, and no scene in this repository carries these blocks yet — nothing
            // ever wrote one. (Projectile, the fifth, is a reflected block now: ProjectileData.)
            Builtins::AddAuthoredSerializers( builtins );

            // ---- Landscape (LS-3): the module's root before the run, its tile and spline after it
            // (Landscape/Private/LandscapeComponentSerializers.cpp) ----
            registerRun( ReflectedBlockRun::Landscape );

            // ---- Skybox, sky, fog, post-process, clouds: one row each in ReflectedComponentBlocks.hpp ----
            registerRun( ReflectedBlockRun::SkyAndAtmosphere );

            // ---- Script (manual: .lua path + exposed-property values) ----
            builtins.push_back( Builtins::MakeScript() );
        }

        const ComponentProvider kEngineProvider{ &Generated::RegisterModuleComponents, &Order };

        [[maybe_unused]] const bool kInstalled = ( InstallComponentProvider( kEngineProvider ), true );
    } // namespace

    void RegisterModuleSerializer( const ModuleSerializer& serializer )
    {
        if ( ModuleSerializers().Ordered )
        {
            LOG_ERROR( "[Scene] a module serializer was registered after the component registry was built; it is "
                       "not placed (register it from the module's StartupModule)" );
            return;
        }
        ModuleSerializers().Registered.push_back( serializer );
    }

    void LinkEngineComponentProvider()
    {
        InstallComponentProvider( kEngineProvider );
    }
} // namespace Desert::Core::Serialize
