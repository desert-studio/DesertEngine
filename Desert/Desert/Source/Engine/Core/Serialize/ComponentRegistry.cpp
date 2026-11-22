#include "ComponentRegistry.hpp"

#include <Engine/Core/Serialize/BuiltinComponentSerializers.hpp>
#include <Engine/Core/Serialize/ComponentRegistration.hpp>

#include <algorithm>
#include <utility>

// THE REGISTRY TYPE AND THE LOAD ORDER — NOTHING ELSE (BUILD-MOD-5). This file used to be the engine's whole
// component table (2400 lines, every component header of every module): a field added to any component
// recompiled it. Now:
//   * reflected blocks are registered by their own module (Engine/Generated/Components_<Module>.gen.cpp,
//     ComponentRegistration.hpp) and read here through ModuleComponents();
//   * hand-written blocks live by subject in *ComponentSerializers.cpp (BuiltinComponentSerializers.hpp);
//   * the AssetResolver is AssetResolver.cpp.

namespace Desert::Core::Serialize
{
    const std::vector<ModuleComponent>& ModuleComponents()
    {
        static const std::vector<ModuleComponent> rows = []
        {
            ComponentRegistrar registrar;
            Generated::RegisterModuleComponents( registrar );
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
        std::vector<ComponentSerializer> builtins;
        const auto                       flush = [this, &builtins]
        {
            for ( ComponentSerializer& s : builtins )
                Register( std::move( s ) );
            builtins.clear();
        };
        // ---- Reflected data blocks: every module's generated rows (COMPONENT markers), run by run ----
        const auto registerRun = [this, &flush]( ReflectedBlockRun run )
        {
            flush();
            for ( const ModuleComponent& row : ModuleComponents() )
                if ( row.Run == run )
                    Register( row.Serializer );
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

        // ---- Landscape (LS-3) ----
        // The root is its frame and nothing else; the tile loads its heights from the file it names
        // (MakeLandscapeTile above). No version bump: two new block keys, and no scene carried them before.
        builtins.push_back( Builtins::MakeLandscapeRoot() );
        registerRun( ReflectedBlockRun::Landscape );
        builtins.push_back( Builtins::MakeLandscapeTile() );
        // One entity per spline (UE ALandscapeSplineActor), its root named by id; scene v76 moved the root's
        // Splines here (Tools/SceneMigrator, MigrateLandscapeSplineEntitiesV75ToV76).
        builtins.push_back( Builtins::MakeLandscapeSpline() );

        // ---- Skybox, sky, fog, post-process, clouds: one row each in ReflectedComponentBlocks.hpp ----
        registerRun( ReflectedBlockRun::SkyAndAtmosphere );

        // ---- Script (manual: .lua path + exposed-property values) ----
        builtins.push_back( Builtins::MakeScript() );
        flush();
    }
} // namespace Desert::Core::Serialize
