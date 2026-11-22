#include <Engine/Core/Serialize/EngineComponentProvider.hpp>

#include <Engine/Core/Serialize/BuiltinComponentSerializers.hpp>
#include <Engine/Core/Serialize/ComponentRegistration.hpp>

#include <span>
#include <vector>

// ENGINE'S COMPONENT PROVIDER (Core/Serialize/ComponentRegistration.hpp, ComponentProvider). The registry is Core's
// and links into the render-free server; the rows of every module Engine links and the hand-written serializers
// (BuiltinComponentSerializers.hpp, render-side: meshes, landscape, scripts) are installed from here, in the load
// order the registry always had. Installed during static initialisation, and again (a no-op) by
// LinkEngineComponentProvider(), which Scene calls: that reference is what links this object into every executable
// that has a Scene (a static library drops an object nothing names).

namespace Desert::Core::Serialize
{
    namespace
    {
        void Order( std::span<const ModuleComponent> rows, std::vector<ComponentSerializer>& out )
        {
        std::vector<ComponentSerializer>& builtins = out; // the registry's load order itself
        // ---- Reflected data blocks: every module's generated rows (COMPONENT markers), run by run ----
        const auto registerRun = [&out, rows]( ReflectedBlockRun run )
        {
            for ( const ModuleComponent& row : rows )
                if ( row.Run == run )
                    out.push_back( row.Serializer );
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
        }

        const ComponentProvider kEngineProvider{ &Generated::RegisterModuleComponents, &Order };

        [[maybe_unused]] const bool kInstalled = ( InstallComponentProvider( kEngineProvider ), true );
    } // namespace

    void LinkEngineComponentProvider()
    {
        InstallComponentProvider( kEngineProvider );
    }
} // namespace Desert::Core::Serialize
