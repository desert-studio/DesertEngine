#pragma once
// THE HLOD BUILDERS, BY NAME (WP-HLOD-BUILDERS) - UE 5.5's UHLODBuilder (Engine/Source/Runtime/Engine/Public/
// WorldPartition/HLOD/HLODBuilder.h) and UHLODLayer::GetHLODBuilderClass / HLODBuilderSettings, the pattern and
// not the letter.
//
// UE builds a cell's HLOD with the builder CLASS its layer names - UHLODBuilderInstancing, MeshMerge,
// MeshSimplify, MeshApproximate for the built-in types, any UHLODBuilder subclass of a loaded module for
// EHLODLayerType::Custom - and every builder reads its own settings object (the UHLODBuilderSettings subclass
// GetSettingsClass names), which the layer serialises. UE finds the class by reflection; here a builder is
// REGISTERED by name (HLODBuilderRegistry::Register): the engine registers the built-in types' builders
// (Core::HLODBuilders, HLODMeshBuilder.hpp), a project or game module registers its own and a Custom layer names
// it. The layer's `Settings` are untyped in the file (HLODLayerSerialized::Settings) and read by the builder alone
// (IHLODBuilder::ValidateSettings, ReadHLODBuilderSettings), so a builder added later needs no field in the layer.
//
// WHAT A BUILDER IS HANDED (UE UHLODBuilder::Build: the source components gathered from the cell's actors, those
// of an actor with a custom HLOD builder class - a landscape proxy - handed to that builder instead): the cell's
// sources as the Instancing pass gathered them (BuildInstancingHLOD: the batches it would instance and every hole
// it named), and it answers the HLOD the file holds. Landscape tiles are the registry's LANDSCAPE builder's (UE
// ALandscapeProxy::GetCustomHLODBuilderClass -> ULandscapeHLODBuilder) and landscape splines its LANDSCAPE SPLINE
// builder's (UE: an ALandscapeSplineActor's USplineMeshComponents), whatever the layer's type.
//
// PURE: no geometry is linked here. The mesh builders' geometry (Core::BuildHLODMesh,
// Core::BuildLandscapeHLODMesh) is given to the builders the engine registers, as CustomShaderSource is given to
// the rules.
#include <Engine/Core/Serialize/WorldPartitionHLODMember.hpp>
#include <Engine/Core/Serialize/WorldPartitionHLODRules.hpp>

#include <Common/Json/Json.hpp>
#include <Common/Utilities/AssetRegistry.hpp>

#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <Common/Json/ReadWrite.hpp>

namespace Desert::Core::Rules
{
    // UE FHLODBuildContext: what one HLOD is built from. The cell's records and their composed WORLD matrices, the
    // members the HLOD stands in for, the nearest it is ever drawn from (cm; the source cell's grid LoadingRange,
    // HLODMinVisibleDistance), the layer's Settings (null when it states none; the merged material and the
    // approximation are the mesh builders' own Settings, WP v12), the asset registries a builder reads meshes
    // through and the mesh geometry the registry states (HLODBuilderRegistry::MeshGeometry; empty = BuildHLODMesh
    // over Registries).
    struct HLODBuildContext
    {
        std::span<const Assets::EntityData>           Records;
        std::span<const glm::mat4>                    World;
        std::span<const std::size_t>                  Members;
        float                                         MinVisibleDistance = 0.0f; // cm
        const Common::Json::Value*                    Settings           = nullptr;
        std::span<const Common::Utils::AssetRegistry> Registries;
        HLODMeshBuilder                               Geometry;
    };

    // UE UHLODBuilder. Stateless: everything one build reads is in its context, so one registered builder serves
    // every cook of every world.
    class IHLODBuilder
    {
    public:
        virtual ~IHLODBuilder() = default;

        // UE UHLODBuilder::GetSettingsClass and the settings object's own checks: whether @p settings (the layer's
        // Settings, null when it states none) are what this builder reads. The refusal names what is wrong without
        // the layer (HLODLayerBuilderIssues names it). A builder that reads no settings keeps this one: it refuses
        // any, so a setting nobody reads cannot sit in a file.
        [[nodiscard]] virtual Common::BoolResultStr ValidateSettings( const Common::Json::Value* settings ) const
        {
            if ( settings != nullptr )
                return Common::MakeError<bool>( "its builder reads no Settings; drop them" );
            return Common::MakeSuccess( true );
        }

        // UE UHLODBuilderSettings::GetCRC: the canonical text of the settings THIS builder reads - its typed
        // settings struct read by ReadHLODBuilderSettings and written back (HLODBuilderSettingsKey), so key order
        // and number spelling make no saved HLOD stale. A builder that reads none answers "". Called only with
        // settings ValidateSettings accepted.
        [[nodiscard]] virtual Common::ResultStr<std::string> SettingsKey( const Common::Json::Value* settings ) const
        {
            (void)settings;
            return Common::MakeSuccess( std::string{} );
        }

        // UE UHLODBuilder::Build: the HLOD of one cell from @p gathered (BuildInstancingHLOD of its members).
        // Called only with Settings this builder accepted. An error refuses the build by name.
        [[nodiscard]] virtual Common::ResultStr<InstancingHLOD> Build( const HLODBuildContext& context,
                                                                       InstancingHLOD gathered ) const = 0;
    };

    // The layer's Settings read as @p T, the builder's settings struct (UE: the UHLODBuilderSettings subclass).
    // Strict, as every read of a file is: a key @p T does not declare is refused by name.
    template <typename T>
    [[nodiscard]] Common::ResultStr<T> ReadHLODBuilderSettings( const Common::Json::Value* settings )
    {
        if ( settings == nullptr )
            return Common::MakeError<T>( "it states no Settings" );
        auto read = Common::Json::Read<T>( Common::Json::Write( *settings ) );
        if ( !read )
            return Common::MakeError<T>( std::format( "Settings: {}", read.GetError() ) );
        return read;
    }

    // The settings @p T canonical (IHLODBuilder::SettingsKey): read strictly, written back with Common::Json::Write
    // (one key order, one number spelling).
    template <typename T>
    [[nodiscard]] Common::ResultStr<std::string> HLODBuilderSettingsKey( const Common::Json::Value* settings )
    {
        auto read = ReadHLODBuilderSettings<T>( settings );
        if ( !read )
            return Common::MakeError<std::string>( read.GetError() );
        return Common::MakeSuccess( Common::Json::Write( read.GetValue() ) );
    }

    // UE UHLODBuilderInstancing: the gathered batches ARE the HLOD (one instanced component per distinct mesh,
    // materials and flags). Reads no settings.
    class HLODBuilderInstancing final : public IHLODBuilder
    {
    public:
        [[nodiscard]] Common::ResultStr<InstancingHLOD> Build( const HLODBuildContext& /*context*/,
                                                               InstancingHLOD gathered ) const override
        {
            return Common::MakeSuccess( std::move( gathered ) );
        }
    };

    // THE MESH LAYERS' SHARED PASS (UE UHLODBuilderMeshMerge / MeshSimplify / MeshApproximate): every gathered
    // batch that casts shadows becomes one built mesh and every batch that does not becomes another (a merged mesh
    // has one CastShadows), each standing in for the union of its batches' sources, in member order. The holes and
    // counts are the gathered ones. @p shape is the builder's own settings as the geometry reads them
    // (TrianglePercent, MergeMaterials, Approximation; its Parts are filled here per mesh). An approximated shape
    // wraps the cell into ONE shell (UE UHLODBuilderMeshApproximate builds one mesh per HLOD actor): its parts are
    // not split by shadow casting, and the shell casts.
    [[nodiscard]] inline Common::ResultStr<InstancingHLOD> MergeHLODBatches( const HLODBuildContext& context,
                                                                             InstancingHLOD          gathered,
                                                                             const HLODMeshRequest&  shape,
                                                                             const HLODMeshBuilder&  geometry )
    {
        if ( gathered.Batches.empty() )
            return Common::MakeSuccess( std::move( gathered ) );
        InstancingHLOD merged;
        merged.NotInstanced   = std::move( gathered.NotInstanced );
        merged.Instances      = gathered.Instances;
        merged.FoliageLeftOut = gathered.FoliageLeftOut;
        const bool oneShell   = shape.Approximation.has_value();
        for ( const bool shadows : { true, false } )
        {
            if ( oneShell && !shadows )
                break;
            std::vector<Assets::InstancedStaticMeshComponentSer> parts;
            std::vector<std::size_t>                             sources;
            for ( const HLODBatch& batch : gathered.Batches )
            {
                if ( !oneShell && batch.Block.CastShadows.value_or( true ) != shadows )
                    continue;
                parts.push_back( batch.Block );
                sources.insert( sources.end(), batch.Sources.begin(), batch.Sources.end() );
            }
            if ( parts.empty() )
                continue;
            HLODMeshRequest request = shape;
            request.Parts           = parts;
            auto built              = geometry( request );
            if ( !built )
                return Common::MakeError<InstancingHLOD>( built.GetError() );
            HLODBatch batch;
            batch.Mesh = built.ExtractValue();
            if ( !shadows )
                batch.Mesh->CastShadows = false;
            // Sources in member order, once each (a record may feed several batches).
            for ( const std::size_t member : context.Members )
                if ( std::find( sources.begin(), sources.end(), member ) != sources.end() )
                    batch.Sources.push_back( member );
            merged.Batches.push_back( std::move( batch ) );
        }
        return Common::MakeSuccess( std::move( merged ) );
    }

    // UE ULandscapeHLODBuilder, the builder a landscape proxy names for itself: every landscape tile the gathering
    // named a hole becomes its own batch (one HLOD component per proxy), a mesh of the tile's heights at the LOD
    // it is seen at (@p tile, Core::BuildLandscapeHLODMesh), appended after the layer's batches.
    class HLODBuilderLandscape final : public IHLODBuilder
    {
    public:
        explicit HLODBuilderLandscape( LandscapeHLODBuilder tile ) : m_Tile( std::move( tile ) )
        {
        }

        [[nodiscard]] Common::ResultStr<InstancingHLOD> Build( const HLODBuildContext& context,
                                                               InstancingHLOD          gathered ) const override
        {
            std::vector<HLODNotInstanced> holes;
            for ( const HLODNotInstanced& missing : gathered.NotInstanced )
            {
                if ( missing.Reason != HLODExclusion::LandscapeTile )
                {
                    holes.push_back( missing );
                    continue;
                }
                LandscapeHLODRequest request;
                request.Records            = context.Records;
                request.World              = context.World;
                request.Tile               = missing.Record;
                request.MinVisibleDistance = context.MinVisibleDistance;
                auto built                 = m_Tile( request );
                if ( !built )
                    return Common::MakeError<InstancingHLOD>(
                         std::format( "landscape tile record {}: {}", missing.Record, built.GetError() ) );
                HLODBatch batch;
                batch.Mesh    = built.ExtractValue();
                batch.Sources = { missing.Record };
                gathered.Batches.push_back( std::move( batch ) );
            }
            gathered.NotInstanced = std::move( holes );
            return Common::MakeSuccess( std::move( gathered ) );
        }

    private:
        LandscapeHLODBuilder m_Tile;
    };

    // THE GEOMETRY OF ONE LANDSCAPE SPLINE (Core::BuildLandscapeSplineHLODMesh): its segments' bent meshes as one
    // WORLD-space StaticMesh block, read through @p registries (the mesh assets the segments lay); nullopt when the
    // spline lays no mesh. An error (a mesh that does not read) refuses the HLOD build by name.
    using LandscapeSplineHLODGeometry = std::function<Common::ResultStr<std::optional<Assets::StaticMeshComponentSer>>(
         const LandscapeSplineHLODRequest& request, std::span<const Common::Utils::AssetRegistry> registries )>;

    // UE: the USplineMeshComponents of an ALandscapeSplineActor built into its cell's HLOD. Every landscape spline
    // the gathering named a hole becomes its own batch of its bent meshes (@p splines, over the context's
    // registries), appended after the layer's batches; a spline that lays no mesh leaves no hole and no batch.
    class HLODBuilderLandscapeSplines final : public IHLODBuilder
    {
    public:
        explicit HLODBuilderLandscapeSplines( LandscapeSplineHLODGeometry splines ) : m_Splines( std::move( splines ) )
        {
        }

        [[nodiscard]] Common::ResultStr<InstancingHLOD> Build( const HLODBuildContext& context,
                                                               InstancingHLOD          gathered ) const override
        {
            std::vector<HLODNotInstanced> holes;
            for ( const HLODNotInstanced& missing : gathered.NotInstanced )
            {
                if ( missing.Reason != HLODExclusion::LandscapeSpline )
                {
                    holes.push_back( missing );
                    continue;
                }
                LandscapeSplineHLODRequest request;
                request.Records = context.Records;
                request.World   = context.World;
                request.Spline  = missing.Record;
                auto built      = m_Splines( request, context.Registries );
                if ( !built )
                    return Common::MakeError<InstancingHLOD>(
                         std::format( "landscape spline record {}: {}", missing.Record, built.GetError() ) );
                std::optional<Assets::StaticMeshComponentSer> mesh = built.ExtractValue();
                if ( !mesh.has_value() )
                    continue; // the spline lays no mesh: nothing to stand in for
                HLODBatch batch;
                batch.Mesh    = std::move( mesh );
                batch.Sources = { missing.Record };
                gathered.Batches.push_back( std::move( batch ) );
            }
            gathered.NotInstanced = std::move( holes );
            return Common::MakeSuccess( std::move( gathered ) );
        }

    private:
        LandscapeSplineHLODGeometry m_Splines;
    };

    // THE BUILDERS A COOK CAN NAME (UE: the UHLODBuilder subclasses of the loaded modules). A value: the editor's
    // Build HLODs copies the process's registry (Core::HLODBuilders) and states its own landscape builder for one
    // build (the bake's materials), and nothing else sees that copy.
    class HLODBuilderRegistry
    {
    public:
        // Registers @p builder under @p name: a built-in type's name (kBuiltInHLODBuilders) by the engine, any
        // other by a project or game module, for a Custom layer to name. Refused, by name: an empty name, no
        // builder, a name already registered (two builders under one name would make a layer's HLOD depend on who
        // registered last). Register at startup, before the first Build HLODs.
        Common::BoolResultStr Register( std::string name, std::shared_ptr<const IHLODBuilder> builder )
        {
            if ( name.empty() )
                return Common::MakeError<bool>( "an HLOD builder is registered under an empty name" );
            if ( !builder )
                return Common::MakeError<bool>(
                     std::format( "HLOD builder '{}' is registered as nothing", name ) );
            if ( m_Builders.contains( name ) )
                return Common::MakeError<bool>( std::format( "HLOD builder '{}' is registered twice", name ) );
            m_Builders.emplace( std::move( name ), std::move( builder ) );
            return Common::MakeSuccess( true );
        }

        // States @p builder for one build under a name ALREADY registered (the editor's Build HLODs gives its copy
        // of the registry the impostor builder that bakes, Core::HLODBuilderImpostor, where the process's refuses
        // by name). Refused, by name: no builder, a name nobody registered (an override must not invent a type).
        Common::BoolResultStr Override( std::string_view name, std::shared_ptr<const IHLODBuilder> builder )
        {
            if ( !builder )
                return Common::MakeError<bool>(
                     std::format( "HLOD builder '{}' is overridden with nothing", name ) );
            const auto at = m_Builders.find( name );
            if ( at == m_Builders.end() )
                return Common::MakeError<bool>(
                     std::format( "HLOD builder '{}' is overridden but was never registered", name ) );
            at->second = std::move( builder );
            return Common::MakeSuccess( true );
        }

        // The builder landscape tiles are handed to (HLODBuilderLandscape). None = every tile stays a named
        // LandscapeTile hole.
        void SetLandscapeBuilder( std::shared_ptr<const IHLODBuilder> builder )
        {
            m_Landscape = std::move( builder );
        }
        [[nodiscard]] const IHLODBuilder* LandscapeBuilder() const
        {
            return m_Landscape.get();
        }

        // The builder landscape splines are handed to (HLODBuilderLandscapeSplines). None = every spline stays a
        // named LandscapeSpline hole.
        void SetLandscapeSplineBuilder( std::shared_ptr<const IHLODBuilder> builder )
        {
            m_LandscapeSplines = std::move( builder );
        }
        [[nodiscard]] const IHLODBuilder* LandscapeSplineBuilder() const
        {
            return m_LandscapeSplines.get();
        }

        // The geometry the mesh builders build through (HLODBuildContext::Geometry). None = BuildHLODMesh over the
        // cook's registries, which bakes no material: the editor's Build HLODs states its own for one build, which
        // bakes a mesh builder's Settings.MergeMaterials and binds it (WP-HLOD-MATBAKE,
        // WorldPartitionHLODBuilder).
        void SetMeshGeometry( HLODMeshBuilder geometry )
        {
            m_MeshGeometry = std::move( geometry );
        }
        [[nodiscard]] const HLODMeshBuilder& MeshGeometry() const
        {
            return m_MeshGeometry;
        }

        // The builder @p layer is built by (HLODBuilderNameOf), or "builder '<name>' is not registered".
        // Every registered builder's name, in order (UE: the UHLODBuilder subclasses the HLOD layer's builder
        // class picker lists).
        [[nodiscard]] std::vector<std::string> Names() const
        {
            std::vector<std::string> names;
            names.reserve( m_Builders.size() );
            for ( const auto& [name, builder] : m_Builders )
                names.push_back( name );
            return names;
        }

        [[nodiscard]] Common::ResultStr<const IHLODBuilder*> BuilderOf( const HLODLayerSerialized& layer ) const
        {
            const std::string_view name = HLODBuilderNameOf( layer );
            const auto             at   = m_Builders.find( name );
            if ( at == m_Builders.end() )
                return Common::MakeError<const IHLODBuilder*>(
                     std::format( "builder '{}' is not registered", name ) );
            return Common::MakeSuccess( static_cast<const IHLODBuilder*>( at->second.get() ) );
        }

    private:
        std::map<std::string, std::shared_ptr<const IHLODBuilder>, std::less<>> m_Builders;
        std::shared_ptr<const IHLODBuilder>                                     m_Landscape;
        std::shared_ptr<const IHLODBuilder>                                     m_LandscapeSplines;
        HLODMeshBuilder                                                         m_MeshGeometry;
    };

    // Which layers the builders are asked about: every one, or only those whose builder IS registered.
    enum class HLODBuilderScope
    {
        EveryLayer,
        RegisteredOnly,
    };

    // WHAT THE BUILDERS SAY OF THE WORLD'S LAYERS (UE: a layer whose builder class is not loaded, or whose
    // settings its builder refuses, fails Build HLODs): one line per layer refused, "HLOD layer '<name>': <why>",
    // in layer order. Empty = every layer can be built. EveryLayer: the World Partition panel shows each line;
    // Build HLODs refuses with the first (WorldCells::BuildWorldHLODs). RegisteredOnly (UE: a settings object's
    // own checks, run as it is edited): only the Settings a registered builder refuses - what the editor's
    // EditWorldPartition refuses when an edit introduces it, while a builder this process has not registered
    // stays a red line and not a refusal, so a world of another module's builders is still edited.
    [[nodiscard]] inline std::vector<std::string>
    HLODLayerBuilderIssues( const WorldPartitionSerialized& partition, const HLODBuilderRegistry& builders,
                            HLODBuilderScope scope = HLODBuilderScope::EveryLayer )
    {
        std::vector<std::string> issues;
        if ( !partition.HLODLayers.has_value() )
            return issues;
        for ( const HLODLayerSerialized& layer : *partition.HLODLayers )
        {
            auto builder = builders.BuilderOf( layer );
            if ( !builder )
            {
                if ( scope == HLODBuilderScope::EveryLayer )
                    issues.push_back( std::format( "HLOD layer '{}': {}", layer.Name, builder.GetError() ) );
                continue;
            }
            const Common::Json::Value* settings = layer.Settings.has_value() ? &*layer.Settings : nullptr;
            if ( auto said = builder.GetValue()->ValidateSettings( settings ); !said )
                issues.push_back( std::format( "HLOD layer '{}': {}", layer.Name, said.GetError() ) );
        }
        return issues;
    }

    // THE HLOD OF ONE CELL BY ITS LAYER (UE UHLODBuilder::Build over the cell's actors): the Instancing pass
    // gathers the sources and the holes, the layer's builder makes the HLOD of them, the landscape builder (when
    // the registry has one) adds a batch per landscape tile and the landscape spline builder one per spline.
    // @p context.Settings is the layer's. Refused, by the layer's name, when its builder is not registered or
    // refuses; call after HLODLayerBuilderIssues found none.
    [[nodiscard]] inline Common::ResultStr<InstancingHLOD>
    BuildCellHLOD( const HLODLayerSerialized& layer, const HLODBuilderRegistry& builders,
                   const HLODBuildContext& context, const CustomShaderSource& customShader,
                   Common::Json::Issues& issues, const FoliageHLODSource& foliage )
    {
        auto builder = builders.BuilderOf( layer );
        if ( !builder )
            return Common::MakeError<InstancingHLOD>(
                 std::format( "HLOD layer '{}': {}", layer.Name, builder.GetError() ) );
        InstancingHLOD gathered =
             BuildInstancingHLOD( context.Records, context.World, context.Members, customShader, issues, foliage );
        auto built = builder.GetValue()->Build( context, std::move( gathered ) );
        if ( !built )
            return Common::MakeError<InstancingHLOD>(
                 std::format( "HLOD layer '{}': {}", layer.Name, built.GetError() ) );
        InstancingHLOD hlod = built.ExtractValue();
        for ( const IHLODBuilder* custom : { builders.LandscapeBuilder(), builders.LandscapeSplineBuilder() } )
        {
            if ( custom == nullptr )
                continue;
            auto added = custom->Build( context, std::move( hlod ) );
            if ( !added )
                return Common::MakeError<InstancingHLOD>(
                     std::format( "HLOD layer '{}': {}", layer.Name, added.GetError() ) );
            hlod = added.ExtractValue();
        }
        return Common::MakeSuccess( std::move( hlod ) );
    }
} // namespace Desert::Core::Rules
