#include "ImportOptionsDialog.hpp"

#include "CookPaths.hpp"
#include "ImportManager.hpp"
#include "ImportedAssetSource.hpp"
#include "ImportSettingsEdits.hpp"
#include "VirtualGeometryGate.hpp"

#include <Common/Content/ImportRecord.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Json/Json.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Mesh/AnimationAsset.hpp>
#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Assets/Mesh/SkeletonAsset.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/ImportRecord.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Mesh/MeshService.hpp>
#include <ImGui/imgui.h>

#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <deque>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::Editor::ImportOptions
{
    // Desert::ImGui (the editor's ImGuiLayer) shadows the library inside Desert; MSVC finds it.
    namespace ImGui = ::ImGui;

    namespace
    {
        namespace Ser = Assets::Serialization;

        constexpr const char* kWindowTitle = "Import Options###ImportOptionsWindow";

        // The window's state: the sources waiting, and the options shown (seeded from the project's last
        // confirmed ones when the window opens for a queue, then carried from source to source like UE's).
        struct QueuedSource
        {
            std::filesystem::path              Source;
            std::vector<std::function<void()>> OnImported; // run after it imported (Request's continuations)
            // What the file holds, read once when it is first shown (the window's title and sections).
            std::optional<Common::ResultStr<ImportContentKind>> Kind;
        };
        struct WindowState
        {
            std::deque<QueuedSource>     Queue;
            Assets::SourceImportSettings Shown;
            bool                         Open = false;
            // The shown source was answered (a button or the palette): the modal closes on its next draw, which
            // is the only place ImGui lets it close.
            bool Answered = false;
        };

        WindowState& Window()
        {
            static WindowState s_State;
            return s_State;
        }

        // Which fields a record's Kind shows: StaticMesh, SkinnedMesh -> SkeletalMesh, Skeleton / Animation ->
        // Animation (ReadImportRecordKind admits only the kinds an import writes).
        ImportContentKind ImportKindOf( const Common::Content::ContentKind kind )
        {
            switch ( kind )
            {
                case Common::Content::ContentKind::SkinnedMesh:
                    return ImportContentKind::SkeletalMesh;
                case Common::Content::ContentKind::Skeleton:
                case Common::Content::ContentKind::Animation:
                    return ImportContentKind::Animation;
                default:
                    return ImportContentKind::StaticMesh;
            }
        }

        // The Details section's working copy of the mesh asset at @p assetPath's source (ImportSettingsEdits).
        Common::ResultStr<ImportSettingsEdit*> SectionEditOf( const std::filesystem::path& assetPath )
        {
            const auto source = ImportSourceOfAsset( assetPath );
            if ( !source )
                return Common::MakeFormattedError<ImportSettingsEdit*>( "'{}' has no import source",
                                                                        assetPath.generic_string() );
            return EditOf( *source );
        }

        constexpr std::array<Assets::MeshSourceUpAxis, 3> kAxes = {
             Assets::MeshSourceUpAxis::FromFile, Assets::MeshSourceUpAxis::Y, Assets::MeshSourceUpAxis::Z };
        constexpr std::array<const char*, 3> kAxisLabels     = { "From File", "Y Up", "Z Up (convert to Y)" };
        constexpr std::array<Assets::MeshLodPolicy, 2> kLods = { Assets::MeshLodPolicy::Generate,
                                                                 Assets::MeshLodPolicy::None };
        constexpr std::array<const char*, 2> kLodLabels = { "Generate (authored, else simplified)", "LOD 0 only" };
        constexpr std::array<Assets::FbxSpecularMap, 4> kSpecularMaps = {
             Assets::FbxSpecularMap::FromImage, Assets::FbxSpecularMap::Specular,
             Assets::FbxSpecularMap::OcclusionRoughnessMetallic, Assets::FbxSpecularMap::RoughnessMetallic };
        constexpr std::array<const char*, 4> kSpecularMapLabels = {
             "From Image (its texels decide)", "Specular (FBX's meaning)", "Packed AO/Roughness/Metal (R/G/B)",
             "Packed Roughness/Metal (G/B, R unused)" };

        constexpr std::array<Assets::MeshFileUnit, 6> kFileUnits = {
             Assets::MeshFileUnit::FromFile, Assets::MeshFileUnit::Millimetres, Assets::MeshFileUnit::Centimetres,
             Assets::MeshFileUnit::Metres,   Assets::MeshFileUnit::Inches,      Assets::MeshFileUnit::Feet };
        constexpr std::array<const char*, 6> kFileUnitLabels = {
             "From File (OBJ: assumed cm)", "Millimetres", "Centimetres", "Metres", "Inches", "Feet" };

        // The window's title and its options section, by what the file holds (UE: Static Mesh / Skeletal Mesh /
        // Animation import).
        const char* KindTitle( const ImportContentKind kind )
        {
            switch ( kind )
            {
                case ImportContentKind::StaticMesh:
                    return ICON_MDI_FILE_IMPORT_OUTLINE "  Static Mesh import";
                case ImportContentKind::SkeletalMesh:
                    return ICON_MDI_FILE_IMPORT_OUTLINE "  Skeletal Mesh import";
                case ImportContentKind::Animation:
                    return ICON_MDI_FILE_IMPORT_OUTLINE "  Animation import";
            }
            return "";
        }
        const char* KindSection( const ImportContentKind kind )
        {
            switch ( kind )
            {
                case ImportContentKind::StaticMesh:
                    return ICON_MDI_SHAPE "  Mesh";
                case ImportContentKind::SkeletalMesh:
                    return ICON_MDI_SHAPE "  Skeletal Mesh";
                case ImportContentKind::Animation:
                    return ICON_MDI_SHAPE "  Animation";
            }
            return "";
        }

        // The window's shown options, for an edit from the palette: refused when no source waits.
        Common::ResultStr<Assets::SourceImportSettings*> ShownSettings()
        {
            WindowState& w = Window();
            if ( w.Queue.empty() || w.Answered )
                return Common::MakeError<Assets::SourceImportSettings*>(
                     "no source waits in the Import Options window" );
            return Common::MakeSuccess( &w.Shown );
        }

        // EVERY ENTITY THAT DRAWS A REIMPORTED MESH DRAWS THE NEW CONTENT FROM THE NEXT FRAME (UE:
        // FReimportManager
        // -> PostReimport recreates the render resources). A loaded mesh drops its parsed payload and its built
        // GPU mesh; the next Get re-reads the file through the path a first use takes (as a committed modeling
        // edit does, ModelingToolTargetAsset.cpp). A mesh nobody loaded has nothing to refresh. A node mesh that
        // this import no longer writes (Combine Meshes switched) is not deleted: entities placed from it keep it,
        // as UE keeps the old assets until they are deleted by hand.
        void RefreshLoadedMeshes( const std::vector<std::filesystem::path>& written )
        {
            auto* service = Runtime::ResourceRegistry::GetMeshService();
            for ( const std::filesystem::path& path : written )
            {
                // The row's handle, the number the entities hold (LoadedHandleOf: a `.skmesh` is known by its
                // GUID).
                const auto known = LoadedHandleOf( path );
                if ( !known )
                {
                    LOG_ERROR( "[Import] '{}' was reimported and its loaded mesh cannot be named: {}",
                               path.generic_string(), known.GetError() );
                    continue;
                }
                const Assets::AssetHandle handle = known.GetValue();
                if ( !service->HasAsset( handle ) )
                    continue;
                if ( auto* asset = service->GetAsset( handle ) )
                    if ( const auto unloaded = asset->Unload(); !unloaded )
                    {
                        LOG_ERROR( "[Import] '{}' was reimported but the loaded mesh was not reset: {}",
                                   path.generic_string(), unloaded.GetError() );
                        continue;
                    }
                (void)service->EvictBuilt( handle );
            }
        }

        // THE REIMPORT'S RELOADS IN FLIGHT. A LoadRequest IS the keep-alive of its read (AsyncAssetLoader.hpp):
        // dropped, its delegate never fires, so every one lives here until its delegate has run. Spent handles
        // are cleared at the next reimport.
        std::vector<Assets::LoadRequest>& ReloadsInFlight()
        {
            static std::vector<Assets::LoadRequest> s_Reloads;
            return s_Reloads;
        }

        // THE SKELETON AND THE CLIPS A REIMPORT REWROTE ARE RE-READ OFF THE FRAME, AND SWAPPED IN WHOLE (UE:
        // Reimport updates the skeleton and the animation sequences with the mesh; FAssetCompilingManager
        // rebuilds them off the game thread and the old asset serves until FinishCompilation). Their consumers
        // hold the asset itself - a skinned mesh caches its rig (SkinnedMeshAsset's skeleton dependency, the
        // MeshService entry's Rig), an Animator binds the clip's AnimationClip - so the new content must land
        // at the same address, which `Load()` on the loaded asset does but on the MAIN thread: Fox_Survey.anim
        // stopped a frame for 1657 ms (ANIM-FIX13b, [SyncLoad] IN A FRAME). So the file is read on a worker into
        // the asset's detached twin (AssetBase::MakeReloadTarget) and the twin's payload is moved in on the
        // main thread by the loader's completion (AdoptReloaded), with the revision bumps an in-place Load
        // makes - the old clip and rig play until then. NEVER `Unload()` first: that frees the Skeleton the
        // meshes and Animators still point at. Every live manager that loaded one refreshes it; one nobody
        // loaded has nothing to refresh. @p onSettled runs once per requested reload, whatever its end.
        // Returns how many reloads were requested.
        template <typename AssetType>
        std::size_t ReloadLoaded( const std::vector<std::filesystem::path>& written,
                                  const std::function<void()>&              onSettled )
        {
            std::vector<Assets::LoadRequest>& inFlight = ReloadsInFlight();
            std::erase_if( inFlight, []( const Assets::LoadRequest& request ) { return !request.IsValid(); } );
            std::size_t requested = 0;
            for ( const std::filesystem::path& path : written )
                for ( Assets::AssetManager* manager : Assets::AssetManager::LiveManagers() )
                {
                    const Assets::Asset<AssetType> asset = manager->FindByPath<AssetType>( path );
                    if ( !asset || !asset->IsReadyForUse() )
                        continue;
                    const std::shared_ptr<Assets::AssetBase> twin = asset->MakeReloadTarget();
                    if ( !twin )
                    {
                        LOG_ERROR( "[Import] '{}' was reimported but has no file to read again",
                                   path.generic_string() );
                        continue;
                    }
                    Assets::LoadRequest request = Assets::AsyncAssetLoader::Get().Request(
                         twin,
                         // An ordinary completion callback, never required noexcept (a false positive).
                         // NOLINTNEXTLINE(bugprone-exception-escape)
                         [asset, path, onSettled]( const Assets::Asset<Assets::AssetBase>& read,
                                                   const Assets::LoadOutcome outcome, const std::string& error )
                         {
                             if ( outcome != Assets::LoadOutcome::Loaded )
                             {
                                 LOG_ERROR( "[Import] '{}' was reimported but not read again: {}",
                                            path.generic_string(), error );
                             }
                             else if ( const auto adopted = asset->AdoptReloaded( *read ); !adopted )
                             {
                                 LOG_ERROR( "[Import] '{}' was read again but not swapped in: {}",
                                            path.generic_string(), adopted.GetError() );
                             }
                             onSettled();
                         },
                         // NOLINTNEXTLINE(bugprone-exception-escape): as above
                         [path, onSettled]
                         {
                             LOG_ERROR( "[Import] the reload of reimported '{}' was cancelled; the previous "
                                        "content stays",
                                        path.generic_string() );
                             onSettled();
                         } );
                    // An invalid handle is the loader's refusal, logged there with the path.
                    if ( !request.IsValid() )
                        continue;
                    inFlight.push_back( std::move( request ) );
                    ++requested;
                }
            return requested;
        }

        bool ImportOne( const std::filesystem::path& source, const Assets::SourceImportSettings& settings )
        {
            const ImportOutcome outcome = SharedImporter().ImportWithSettings( source, settings );
            // The rig first: the skinned meshes rebuilt after it read it, so they are reset when the last rig
            // reload has settled (at once when no loaded rig was rewritten). The clips need nothing after them.
            auto rigsLeft = std::make_shared<std::size_t>( 0 );
            auto meshes   = std::make_shared<const std::vector<std::filesystem::path>>( outcome.WrittenMeshes );
            *rigsLeft     = ReloadLoaded<Assets::SkeletonAsset>( outcome.WrittenSkeletons,
                                                                 [rigsLeft, meshes]
                                                                 {
                                                                 if ( --*rigsLeft == 0 )
                                                                     RefreshLoadedMeshes( *meshes );
                                                             } );
            if ( *rigsLeft == 0 )
                RefreshLoadedMeshes( *meshes );
            (void)ReloadLoaded<Assets::AnimationAsset>( outcome.WrittenClips, [] {} );
            if ( outcome.Verdict == CookVerdict::Failed || outcome.Verdict == CookVerdict::NotCookable )
            {
                LOG_ERROR( "[Import] '{}' was not imported with the chosen options (see the error above)",
                           source.generic_string() );
                return false;
            }
            return true;
        }
    } // namespace

    ImportManager& SharedImporter()
    {
        static ImportManager s_Importer;
        return s_Importer;
    }

    Common::BoolResultStr SetUniformScale( Assets::SourceImportSettings& settings, const float scale )
    {
        if ( !std::isfinite( scale ) || scale <= 0.0f )
            return Common::MakeFormattedError<bool>( "Uniform Scale {} is not a finite number above zero", scale );
        settings.Mesh.UniformScale = scale;
        return BOOLSUCCESS;
    }

    void SetUpAxis( Assets::SourceImportSettings& settings, const Assets::MeshSourceUpAxis axis )
    {
        settings.Mesh.UpAxis = axis;
    }

    void SetCombineMeshes( Assets::SourceImportSettings& settings, const bool on )
    {
        settings.CombineMeshes = on;
    }

    Common::BoolResultStr SetDisplacement( Assets::SourceImportSettings&      settings,
                                           const Assets::MeshDisplacementMode mode, const ImportContentKind kind )
    {
        // CONTRACT-DISPLACE §9.8: a skeletal mesh is displaced over its skinned pose each frame; a bake is one
        // pose.
        if ( kind == ImportContentKind::SkeletalMesh && mode == Assets::MeshDisplacementMode::Baked )
            return Common::MakeFormattedError<bool>(
                 "Displacement {}: a skeletal mesh is displaced None or Runtime",
                 Assets::MeshDisplacementModeName( mode ) );
        if ( settings.Mesh.VirtualGeometry && mode == Assets::MeshDisplacementMode::Runtime )
            return Common::MakeError<bool>(
                 "Displacement Runtime: the mesh is Virtual Geometry, whose clusters are "
                 "cooked once; a surface diced per frame has none (turn Virtual "
                 "Geometry off first)" );
        settings.Mesh.Displacement = mode;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr SetVirtualGeometry( Assets::SourceImportSettings& settings, const bool on,
                                              const ImportContentKind kind )
    {
        if ( on && kind == ImportContentKind::SkeletalMesh )
            return Common::MakeError<bool>( "Virtual Geometry: a skeletal mesh is drawn by the binned path (its "
                                            "clusters would be one pose)" );
        if ( on && settings.Mesh.Displacement == Assets::MeshDisplacementMode::Runtime )
            return Common::MakeError<bool>( "Virtual Geometry: Displacement Runtime dices the surface per frame, "
                                            "so it has no cooked clusters (choose None or Baked first)" );
        settings.Mesh.VirtualGeometry = on;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr SetShownVirtualGeometry( const bool on )
    {
        const auto shown = ShownSettings();
        if ( !shown )
            return Common::MakeError<bool>( shown.GetError() );
        QueuedSource& source = Window().Queue.front(); // the shown one (DrawWindow shows the queue's front)
        if ( !source.Kind )
            source.Kind = SharedImporter().ProbeContent( source.Source );
        if ( !*source.Kind )
            return Common::MakeError<bool>( source.Kind->GetError() );
        return SetVirtualGeometry( *shown.GetValue(), on, source.Kind->GetValue() );
    }

    Common::BoolResultStr SetShownDisplacement( const Assets::MeshDisplacementMode mode )
    {
        const auto shown = ShownSettings();
        if ( !shown )
            return Common::MakeError<bool>( shown.GetError() );
        QueuedSource& source = Window().Queue.front(); // the shown one (DrawWindow shows the queue's front)
        if ( !source.Kind )
            source.Kind = SharedImporter().ProbeContent( source.Source );
        if ( !*source.Kind )
            return Common::MakeError<bool>( source.Kind->GetError() );
        return SetDisplacement( *shown.GetValue(), mode, source.Kind->GetValue() );
    }

    Common::BoolResultStr SetShownUniformScale( const float scale )
    {
        const auto shown = ShownSettings();
        if ( !shown )
            return Common::MakeError<bool>( shown.GetError() );
        return SetUniformScale( *shown.GetValue(), scale );
    }

    Common::BoolResultStr SetShownUpAxis( const Assets::MeshSourceUpAxis axis )
    {
        const auto shown = ShownSettings();
        if ( !shown )
            return Common::MakeError<bool>( shown.GetError() );
        SetUpAxis( *shown.GetValue(), axis );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr SetShownCombineMeshes( const bool on )
    {
        const auto shown = ShownSettings();
        if ( !shown )
            return Common::MakeError<bool>( shown.GetError() );
        SetCombineMeshes( *shown.GetValue(), on );
        return BOOLSUCCESS;
    }

    void Request( const std::filesystem::path& source, std::function<void()> onImported )
    {
        WindowState& w = Window();
        for ( auto& queued : w.Queue )
            if ( queued.Source == source )
            {
                if ( onImported )
                    queued.OnImported.push_back( std::move( onImported ) );
                return;
            }
        QueuedSource entry{ source, {}, std::nullopt };
        if ( onImported )
            entry.OnImported.push_back( std::move( onImported ) );
        w.Queue.push_back( std::move( entry ) );
    }

    Common::BoolResultStr ConfirmImport( const bool all )
    {
        WindowState& w = Window();
        if ( w.Queue.empty() || w.Answered )
            return Common::MakeError<bool>( "no source waits in the Import Options window" );
        if ( auto saved = SaveLastUsed( w.Shown ); !saved )
            LOG_ERROR( "[Import] the options were not remembered for this project: {}", saved.GetError() );
        const std::size_t count = all ? w.Queue.size() : 1;
        for ( std::size_t i = 0; i < count && !w.Queue.empty(); ++i )
        {
            // Popped before its continuations run: a continuation may queue again (a drop of another new file).
            const QueuedSource entry = std::move( w.Queue.front() );
            w.Queue.pop_front();
            if ( ImportOne( entry.Source, w.Shown ) )
                for ( const auto& then : entry.OnImported )
                    then();
        }
        w.Answered = true;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr CancelImport()
    {
        WindowState& w = Window();
        if ( w.Queue.empty() || w.Answered )
            return Common::MakeError<bool>( "no source waits in the Import Options window" );
        LOG_INFO( "[Import] '{}' was not imported (Import Options cancelled)",
                  w.Queue.front().Source.generic_string() );
        w.Queue.pop_front();
        w.Answered = true;
        return BOOLSUCCESS;
    }

    bool Pending()
    {
        return !Window().Queue.empty();
    }

    std::filesystem::path LastUsedPath()
    {
        const auto& root = Common::Constants::Path::CurrentProjectRoot().ProjectDir;
        return ( root.empty() ? std::filesystem::path( "Saved" ) : root / "Saved" ) / "ImportOptions.json";
    }

    Common::ResultStr<std::optional<Assets::SourceImportSettings>> LoadLastUsed()
    {
        using Result = std::optional<Assets::SourceImportSettings>;
        std::error_code ec;
        if ( !std::filesystem::is_regular_file( LastUsedPath(), ec ) )
            return Common::MakeSuccess( Result{} );
        const auto read = Common::Json::ReadFile<Ser::SourceImportSettingsText>( LastUsedPath() );
        if ( !read )
            return Common::MakeError<Result>( read.GetError() );
        auto settings = Ser::ImportSettingsFromText( read.GetValue() );
        if ( !settings )
            return Common::MakeFormattedError<Result>( "'{}': {}", LastUsedPath().string(), settings.GetError() );
        return Common::MakeSuccess( Result{ settings.ExtractValue() } );
    }

    Common::BoolResultStr SaveLastUsed( const Assets::SourceImportSettings& settings )
    {
        std::error_code ec;
        std::filesystem::create_directories( LastUsedPath().parent_path(), ec );
        if ( ec )
            return Common::MakeFormattedError<bool>( "'{}' could not be created: {}",
                                                     LastUsedPath().parent_path().string(), ec.message() );
        // The Skeleton choice belongs to the file it was made for (its record): a remembered one would import the
        // next, unrelated file onto it.
        Assets::SourceImportSettings remembered = settings;
        remembered.Skeleton.reset();
        return Common::Json::WriteFileAtomic( LastUsedPath(), Ser::ImportSettingsToText( remembered ) );
    }

    bool DrawImportSettingsFields( Assets::SourceImportSettings& settings, const ImportContentKind kind )
    {
        using UI     = Utils::ImGuiUtilities;
        bool changed = false;
        ImGui::PushID( "ImportSettingsFields" );

        // A skinned file is one skeletal mesh whatever its nodes (UE's Skeletal Mesh import has no Combine
        // Meshes).
        if ( kind == ImportContentKind::StaticMesh )
        {
            UI::BeginPropertyRow( "Combine Meshes",
                                  "Off (UE's default): every mesh-bearing node of the file "
                                  "becomes its own static mesh. On: the whole file is one static "
                                  "mesh." );
            bool combine = settings.CombineMeshes;
            if ( ImGui::Checkbox( "##CombineMeshes", &combine ) )
            {
                SetCombineMeshes( settings, combine );
                changed = true;
            }
            UI::EndPropertyRow();
        }

        UI::BeginPropertyRow( "File Unit",
                              "What one length of the file is. From File: FBX states it, glTF is metres; an OBJ "
                              "states nothing and is taken as centimetres. Any other choice wins over the file." );
        int unit = 0;
        for ( std::size_t i = 0; i < kFileUnits.size(); ++i )
            if ( kFileUnits[i] == settings.FileUnit )
                unit = static_cast<int>( i );
        if ( ImGui::Combo( "##FileUnit", &unit, kFileUnitLabels.data(),
                           static_cast<int>( kFileUnitLabels.size() ) ) )
        {
            settings.FileUnit = kFileUnits[static_cast<std::size_t>( unit )];
            changed           = true;
        }
        UI::EndPropertyRow();

        UI::BeginPropertyRow( "Uniform Scale", "Applied on import, after the file unit: a further factor." );
        float scale = settings.Mesh.UniformScale;
        if ( ImGui::DragFloat( "##UniformScale", &scale, 0.01f, 0.001f, 1000.0f, "%.3f" ) &&
             SetUniformScale( settings, scale ) )
            changed = true;
        UI::EndPropertyRow();

        UI::BeginPropertyRow( "Up Axis", "From File keeps the exporter's own axis conversion; Z Up rotates +Z to "
                                         "the engine's +Y." );
        int axis = 0;
        for ( std::size_t i = 0; i < kAxes.size(); ++i )
            if ( kAxes[i] == settings.Mesh.UpAxis )
                axis = static_cast<int>( i );
        if ( ImGui::Combo( "##UpAxis", &axis, kAxisLabels.data(), static_cast<int>( kAxisLabels.size() ) ) )
        {
            SetUpAxis( settings, kAxes[static_cast<std::size_t>( axis )] );
            changed = true;
        }
        UI::EndPropertyRow();

        UI::BeginPropertyRow( "LODs", "Generate: the file's authored LODs, simplified ones otherwise. LOD 0 only: "
                                      "no LOD chain." );
        int lod = settings.Mesh.LodPolicy == Assets::MeshLodPolicy::None ? 1 : 0;
        if ( ImGui::Combo( "##Lods", &lod, kLodLabels.data(), static_cast<int>( kLodLabels.size() ) ) )
        {
            settings.Mesh.LodPolicy = kLods[static_cast<std::size_t>( lod )];
            changed                 = true;
        }
        UI::EndPropertyRow();

        // LOD-IMPORT: the generated chain is the mesh's own (MeshImportSettings::LodChain, UE's per-LOD Reduction
        // Settings). An edit is taken only when the chain stays one the reduction can walk
        // (Geometry::LODChainSettingsDefect); otherwise the defect is the tooltip.
        if ( settings.Mesh.LodPolicy == Assets::MeshLodPolicy::Generate )
        {
            Geometry::LODChainSettings& chain = settings.Mesh.LodChain;
            const auto                  take  = [&]( const Geometry::LODChainSettings& edited )
            {
                if ( const std::string defect = Geometry::LODChainSettingsDefect( edited ); !defect.empty() )
                {
                    ImGui::SetTooltip( "%s", defect.c_str() );
                    return;
                }
                chain   = edited;
                changed = true;
            };
            for ( std::size_t level = 0; level < chain.TriangleRatios.size(); ++level )
            {
                const std::string row = std::format( "LOD {} Triangles", level + 1 );
                UI::BeginPropertyRow( row.c_str(), "This level's share of LOD 0's triangles, below the level "
                                                   "before it; never reduced under the floor." );
                ImGui::PushID( static_cast<int>( level ) );
                float percent = chain.TriangleRatios[level] * 100.0f;
                if ( ImGui::DragFloat( "##LodRatio", &percent, 0.05f, 0.01f, 99.9f, "%.2f %%" ) )
                {
                    Geometry::LODChainSettings edited = chain;
                    edited.TriangleRatios[level]      = percent / 100.0f;
                    take( edited );
                }
                ImGui::PopID();
                UI::EndPropertyRow();
            }
            UI::BeginPropertyRow( "LOD Levels", "Add a level a quarter of the last one's triangles, or remove the "
                                                "last level." );
            if ( ImGui::Button( "Add Level" ) )
            {
                Geometry::LODChainSettings edited = chain;
                edited.TriangleRatios.push_back( chain.TriangleRatios.back() * 0.25f );
                take( edited );
            }
            ImGui::SameLine();
            if ( ImGui::Button( "Remove Level" ) && chain.TriangleRatios.size() > 1 )
            {
                Geometry::LODChainSettings edited = chain;
                edited.TriangleRatios.pop_back();
                take( edited );
            }
            UI::EndPropertyRow();
            UI::BeginPropertyRow( "LOD Floor", "No level is reduced below this many triangles; the chain ends at "
                                               "the first level that reaches it." );
            int floorTriangles = static_cast<int>( chain.MinTriangles );
            if ( ImGui::DragInt( "##LodFloor", &floorTriangles, 1.0f, 1, 100000 ) && floorTriangles > 0 )
            {
                Geometry::LODChainSettings edited = chain;
                edited.MinTriangles               = static_cast<uint32_t>( floorTriangles );
                take( edited );
            }
            UI::EndPropertyRow();
        }

        // VIRT-GEOM (contract §1, §6): a static mesh only. The import refuses it, naming the reason in the log,
        // for a mesh that draws a Masked / Translucent / Additive material (VirtualGeometryGate.hpp).
        if ( kind == ImportContentKind::StaticMesh )
        {
            UI::BeginPropertyRow( "Virtual Geometry",
                                  "On: the mesh cooks a cluster DAG (128-triangle clusters, continuous LOD); its "
                                  "LODs are cuts of that DAG. Refused for Displacement Runtime, and by the "
                                  "import for a mesh whose material is Masked or Translucent." );
            bool virtualGeometry = settings.Mesh.VirtualGeometry;
            if ( ImGui::Checkbox( "##VirtualGeometry", &virtualGeometry ) )
            {
                if ( const auto set = SetVirtualGeometry( settings, virtualGeometry, kind ); !set )
                    ImGui::SetTooltip( "%s", set.GetError().c_str() );
                else
                    changed = true;
            }
            UI::EndPropertyRow();
        }

        // THE CROWN (FIELD-WINTER, MeshCanopySettings): which material slots are the leaf cards, and the crown's
        // own snow numbers - the shell a leaf takes snow in and the clearance over the crown's peak - baked per
        // leaf vertex by the mesh build (MeshDeriver BakeCanopy), so the shader states none of them. A static mesh
        // only.
        if ( kind == ImportContentKind::StaticMesh )
        {
            UI::BeginPropertyRow(
                 "Canopy Leaf Slots",
                 "Material slot names, comma-separated, whose sections are the crown's leaf cards: "
                 "the build gives them the crown's volume normal and their depth under its top. "
                 "Empty: no canopy." );
            std::string slots;
            for ( const std::string& slot : settings.Mesh.Canopy.LeafSlots )
                slots += ( slots.empty() ? "" : ", " ) + slot;
            std::array<char, 512> buffer{};
            std::snprintf( buffer.data(), buffer.size(), "%s", slots.c_str() );
            if ( ImGui::InputText( "##CanopyLeafSlots", buffer.data(), buffer.size(),
                                   ImGuiInputTextFlags_EnterReturnsTrue ) ||
                 ImGui::IsItemDeactivatedAfterEdit() )
            {
                std::vector<std::string> parsed;
                std::string_view         rest( buffer.data() );
                while ( !rest.empty() )
                {
                    const std::size_t comma = rest.find( ',' );
                    std::string_view  name  = rest.substr( 0, comma );
                    rest                    = comma == std::string_view::npos ? "" : rest.substr( comma + 1 );
                    while ( !name.empty() && name.front() == ' ' )
                        name.remove_prefix( 1 );
                    while ( !name.empty() && name.back() == ' ' )
                        name.remove_suffix( 1 );
                    if ( !name.empty() && std::ranges::find( parsed, name ) == parsed.end() )
                        parsed.emplace_back( name );
                }
                if ( parsed != settings.Mesh.Canopy.LeafSlots )
                {
                    settings.Mesh.Canopy.LeafSlots = std::move( parsed );
                    changed                        = true;
                }
            }
            UI::EndPropertyRow();
            if ( !settings.Mesh.Canopy.LeafSlots.empty() )
            {
                UI::BeginPropertyRow( "Canopy Shell (cm)",
                                      "A leaf deeper than this under the crown's top is sheltered by the leaves "
                                      "above: its snow fades to none over the next shell. Mesh centimetres (grows "
                                      "with the placed scale)." );
                if ( ImGui::DragFloat( "##CanopyShellCm", &settings.Mesh.Canopy.ShellCm, 1.0f,
                                       Assets::kMeshCanopyShellMinCm, Assets::kMeshCanopyShellMaxCm, "%.0f",
                                       ImGuiSliderFlags_AlwaysClamp ) )
                    changed = true;
                UI::EndPropertyRow();
                UI::BeginPropertyRow( "Canopy Clearance (texels)",
                                      "How far above the crown's own peak a leaf reads the sky, in texels of the "
                                      "view's precipitation map: its texel is wider than the crown's 25 cm top "
                                      "cells, so a leaf on the flank would read the peak beside it as its roof." );
                if ( ImGui::DragFloat( "##CanopyClearTexels", &settings.Mesh.Canopy.ClearTexels, 0.05f, 0.0f,
                                       Assets::kMeshCanopyClearTexelsMax, "%.2f", ImGuiSliderFlags_AlwaysClamp ) )
                    changed = true;
                UI::EndPropertyRow();
            }
        }

        // CONTRACT-DISPLACE §9.7: the mode and the bake edge length live in the mesh's import settings. A skeletal
        // mesh offers None/Runtime only (§9.8: displaced over the skin cache each frame; a bake would be one pose)
        // and no edge length (it has no baked surface: its collision is its physics asset).
        const bool skeletal = kind == ImportContentKind::SkeletalMesh;
        UI::BeginPropertyRow( "Displacement",
                              skeletal ? "Sections whose material assigns its Height move along the surface. "
                                         "Runtime: diced per frame by screen size over the skinned pose."
                                       : "Sections whose material assigns its Height move along the surface. "
                                         "Runtime: diced per frame by screen size. Baked: a dense mesh cooked at "
                                         "the edge length below, drawn with its own LODs. Collision uses the "
                                         "baked surface in both modes." );
        static constexpr std::array<const char*, 3> kDisplacementLabels = { "None", "Runtime", "Baked" };
        static constexpr std::array<Assets::MeshDisplacementMode, 3> kDisplacementModes = {
             Assets::MeshDisplacementMode::None, Assets::MeshDisplacementMode::Runtime,
             Assets::MeshDisplacementMode::Baked };
        const int modeCount = skeletal ? 2 : static_cast<int>( kDisplacementLabels.size() );
        int       mode      = 0;
        for ( std::size_t i = 0; i < kDisplacementModes.size(); ++i )
            if ( kDisplacementModes[i] == settings.Mesh.Displacement )
                mode = static_cast<int>( i );
        if ( ImGui::Combo( "##Displacement", &mode, kDisplacementLabels.data(), modeCount ) )
        {
            if ( const auto set =
                      SetDisplacement( settings, kDisplacementModes[static_cast<std::size_t>( mode )], kind );
                 !set )
                ImGui::SetTooltip( "%s", set.GetError().c_str() );
            else
                changed = true;
        }
        UI::EndPropertyRow();

        if ( !skeletal && settings.Mesh.Displacement != Assets::MeshDisplacementMode::None )
        {
            UI::BeginPropertyRow( "Bake Edge Length",
                                  "Centimetres per diced edge of the baked surface (Baked draw and collision). "
                                  "Clusters that cannot dice this fine bake at their densest level." );
            float edge = settings.Mesh.DisplacementBake.EdgeLengthCm;
            if ( ImGui::DragFloat( "##DisplacementEdge", &edge, 0.05f, 0.05f, 100.0f, "%.2f cm" ) &&
                 edge > 0.0f && edge != settings.Mesh.DisplacementBake.EdgeLengthCm )
            {
                settings.Mesh.DisplacementBake.EdgeLengthCm = edge;
                changed                                     = true;
            }
            UI::EndPropertyRow();
        }

        UI::BeginPropertyRow(
             "FBX Specular Map",
             "What the FBX Specular map holds. From Image (default): read per map from its texels - R 0 "
             "everywhere with G/B in use is Packed Roughness/Metal, anything else Specular; the import logs "
             "what it read. Specular: a specular-colour image, as FBX defines "
             "it (the surface template has no input for it; the import says so). Packed AO/Roughness/"
             "Metal: AO in R, roughness in G, metalness in B. Packed Roughness/Metal: roughness in "
             "G, metalness in B, R holds no occlusion (Lumberyard Bistro / ORCA, Falcor's "
             "metal-rough). Written into new materials only: delete an imported .demat to re-make "
             "it." );
        int specular =
             static_cast<int>( std::ranges::find( kSpecularMaps, settings.SpecularMap ) - kSpecularMaps.begin() );
        if ( ImGui::Combo( "##SpecularMap", &specular, kSpecularMapLabels.data(),
                           static_cast<int>( kSpecularMapLabels.size() ) ) )
        {
            settings.SpecularMap = kSpecularMaps[static_cast<std::size_t>( specular )];
            changed              = true;
        }
        UI::EndPropertyRow();

        // UE's Skeleton field of the skeletal mesh / animation import: an existing skeleton the rig is imported
        // onto, or none (the default) - then the one skeleton whose bones state the rig's, else a new one
        // (ImportManager ExistingSkeletonFor). Listed from the registry's Skeleton rows, loading nothing.
        if ( kind != ImportContentKind::StaticMesh )
        {
            UI::BeginPropertyRow( "Skeleton",
                                  "None: the one registered skeleton with the file's bones, else a new "
                                  ".skeleton. A chosen skeleton must hold every bone the mesh and the "
                                  "clips need, or the import is refused." );
            const auto rows   = Assets::ContentRegistry::Rows( Common::Content::ContentKind::Skeleton );
            const auto nameOf = []( const Assets::ContentRegistry::PickerRow& row )
            { return row.DisplayName.empty() ? row.Path.stem().string() : row.DisplayName; };
            std::string preview = "None (new or matching skeleton)";
            if ( settings.Skeleton )
            {
                preview =
                     std::format( "Missing skeleton {}", Common::Content::AssetGuidToText( *settings.Skeleton ) );
                for ( const auto& row : rows )
                    if ( row.Guid && *row.Guid == *settings.Skeleton )
                        preview = nameOf( row );
            }
            if ( ImGui::BeginCombo( "##Skeleton", preview.c_str() ) )
            {
                if ( ImGui::Selectable( "None (new or matching skeleton)", !settings.Skeleton ) )
                {
                    settings.Skeleton.reset();
                    changed = true;
                }
                for ( const auto& row : rows )
                {
                    if ( !row.Guid )
                        continue;
                    ImGui::PushID( row.Key.c_str() );
                    const bool selected = settings.Skeleton && *settings.Skeleton == *row.Guid;
                    if ( ImGui::Selectable( nameOf( row ).c_str(), selected ) )
                    {
                        settings.Skeleton = *row.Guid;
                        changed           = true;
                    }
                    if ( ImGui::IsItemHovered() )
                        ImGui::SetTooltip( "%s", row.Key.c_str() );
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            UI::EndPropertyRow();
        }

        ImGui::PopID();
        return changed;
    }

    void DrawWindow()
    {
        WindowState& w = Window();
        if ( !w.Open )
        {
            w.Answered = false;
            if ( w.Queue.empty() )
                return;
            // A new queue starts from the project's last confirmed options (UE: the import UI's saved config).
            auto last = LoadLastUsed();
            if ( !last )
                LOG_ERROR( "[Import] {}; the window starts from the defaults", last.GetError() );
            w.Shown = Assets::SourceImportSettings{};
            if ( last )
                if ( const auto& saved = last.GetValue(); saved )
                    w.Shown = *saved;
            ImGui::OpenPopup( kWindowTitle );
            w.Open = true;
        }

        ImGui::SetNextWindowSize( ImVec2( 520.0f, 0.0f ), ImGuiCond_Appearing );
        if ( !ImGui::BeginPopupModal( kWindowTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
        {
            w.Open = false;
            return;
        }
        // Answered from the palette since the last draw: close here, where ImGui allows it. The next source, if
        // any, opens its own window on the next frame, from the options just confirmed.
        if ( w.Answered || w.Queue.empty() )
        {
            ImGui::CloseCurrentPopup();
            w.Open = false;
            ImGui::EndPopup();
            return;
        }

        QueuedSource& shown = w.Queue.front();
        if ( !shown.Kind )
            shown.Kind = SharedImporter().ProbeContent( shown.Source );
        const std::filesystem::path source = shown.Source;
        if ( !*shown.Kind )
        {
            // Not readable: said in the window, and Import will fail with the same file named.
            ImGui::TextUnformatted( ICON_MDI_FILE_IMPORT_OUTLINE "  Import" );
            ImGui::TextWrapped( "%s", shown.Kind->GetError().c_str() );
        }
        else
            ImGui::TextUnformatted( KindTitle( shown.Kind->GetValue() ) );
        ImGui::Separator();
        ImGui::TextUnformatted( std::format( "File: {}", source.filename().string() ).c_str() );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "%s", source.generic_string().c_str() );
        if ( w.Queue.size() > 1 )
            ImGui::TextDisabled(
                 "%s", std::format( "{} more file(s) wait after this one", w.Queue.size() - 1 ).c_str() );
        ImGui::Spacing();

        const ImportContentKind kind = *shown.Kind ? shown.Kind->GetValue() : ImportContentKind::StaticMesh;
        if ( *shown.Kind && Utils::ImGuiUtilities::SectionHeader( KindSection( kind ) ) )
        {
            Utils::ImGuiUtilities::ResetPropertyRows();
            (void)DrawImportSettingsFields( w.Shown, kind );
        }
        ImGui::Spacing();
        ImGui::Separator();

        if ( ImGui::Button( "Import", ImVec2( 110.0f, 0.0f ) ) )
            (void)ConfirmImport( false );
        ImGui::SameLine();
        if ( ImGui::Button( "Import All", ImVec2( 110.0f, 0.0f ) ) )
            (void)ConfirmImport( true );
        ImGui::SameLine();
        if ( ImGui::Button( "Cancel", ImVec2( 110.0f, 0.0f ) ) )
            (void)CancelImport();
        if ( w.Answered )
        {
            ImGui::CloseCurrentPopup();
            w.Open = false;
        }
        ImGui::EndPopup();
    }

    std::optional<std::filesystem::path> ImportSourceOfAsset( const std::filesystem::path& assetPath )
    {
        // A skinned import's file is traced to its source by what the import wrote (ImportedAssetSource::
        // SkinnedAssetSource): a clip's own `Import` (`<stem>_<clip>.anim` has no inverse), the record beside a
        // mesh or a rig (it states the extension the name cannot).
        if ( CookPaths::IsSkinnedAssetFile( assetPath ) )
        {
            const auto stated = ImportedAssetSource::SkinnedAssetSource( assetPath );
            if ( !stated )
                return std::nullopt;
            const std::optional<std::filesystem::path>& source = stated.GetValue();
            std::error_code                             ec;
            if ( !source.has_value() || !std::filesystem::is_regular_file( source.value(), ec ) )
                return std::nullopt;
            return source;
        }
        if ( auto beside = Common::Content::MeshSourceBeside( assetPath ) )
            return beside;
        if ( assetPath.extension() != ".stmesh" ) // only a static node mesh names its source inside (IMPT)
            return std::nullopt;
        std::error_code ec;
        if ( !std::filesystem::is_regular_file( assetPath, ec ) )
            return std::nullopt;
        const auto asset = Assets::ReadMeshSourceAssetFile( assetPath );
        if ( !asset || asset.GetValue().Import.Provenance != Assets::MeshSourceProvenance::Imported ||
             asset.GetValue().Import.SourceFile.empty() )
            return std::nullopt;
        std::filesystem::path source = Common::AssetHandle::PathForStableKey( asset.GetValue().Import.SourceFile );
        if ( !std::filesystem::is_regular_file( source, ec ) )
            return std::nullopt;
        return source;
    }

    Common::ResultStr<ImportContentKind> RecordedImportKind( const std::filesystem::path& source )
    {
        const auto kind = Ser::ReadImportRecordKind( source );
        if ( !kind )
            return Common::MakeError<ImportContentKind>( kind.GetError() );
        return Common::MakeSuccess( ImportKindOf( kind.GetValue() ) );
    }

    void DrawImportSettingsSection( const std::filesystem::path& assetPath )
    {
        const auto source = ImportSourceOfAsset( assetPath );
        if ( !source )
            return;
        const std::string key   = source->generic_string();
        const auto        found = EditOf( *source );
        if ( !found )
        {
            if ( Utils::ImGuiUtilities::SectionHeader( ICON_MDI_FILE_IMPORT_OUTLINE "  Import Settings" ) )
                ImGui::TextWrapped( "%s", found.GetError().c_str() );
            return;
        }
        ImportSettingsEdit& edit = *found.GetValue();

        if ( !Utils::ImGuiUtilities::SectionHeader( ICON_MDI_FILE_IMPORT_OUTLINE "  Import Settings" ) )
            return;
        ImGui::PushID( key.c_str() );
        // The source is a FACT, not a setting (UE's AssetImportData "Source File"): its full path in the
        // read-only table, whose declared columns cannot clip it down to the extension.
        if ( Utils::ImGuiUtilities::BeginFactTable( "##ImportSource" ) )
        {
            std::error_code ec;
            const auto      full = std::filesystem::absolute( *source, ec );
            Utils::ImGuiUtilities::FactRow( "Source File",
                                            ( ec ? *source : full ).lexically_normal().generic_string() );
            Utils::ImGuiUtilities::EndFactTable();
        }
        Utils::ImGuiUtilities::ResetPropertyRows();
        (void)DrawImportSettingsFields( edit.Edit, ImportKindOf( edit.Kind ) );

        const bool edited = edit.Recorded != edit.Edit;
        if ( ImGui::Button( ICON_MDI_RELOAD "  Reimport" ) )
        {
            if ( const auto reimported = Reimport( assetPath ); !reimported )
                LOG_ERROR( "[Import] {}", reimported.GetError() );
            ImGui::PopID();
            return;
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Imports '%s' again with these options%s", source->filename().string().c_str(),
                               edited ? " (changed, not applied yet)" : "" );
        if ( edited )
        {
            ImGui::SameLine();
            if ( ImGui::Button( "Revert" ) )
                edit.Edit = edit.Recorded;
        }
        ImGui::PopID();
    }

    Common::BoolResultStr Reimport( const std::filesystem::path& assetPath )
    {
        const auto source = ImportSourceOfAsset( assetPath );
        if ( !source )
            return Common::MakeFormattedError<bool>( "'{}' has no import source to reimport from",
                                                     assetPath.generic_string() );
        // The ONE working copy, read again when the record on disk is newer than it (ImportSettingsEdits).
        const auto edit = EditOf( *source );
        if ( !edit )
            return Common::MakeError<bool>( edit.GetError() );
        const Assets::SourceImportSettings settings = edit.GetValue()->Edit;
        if ( !ImportOne( *source, settings ) )
            return Common::MakeFormattedError<bool>( "'{}' was not reimported (see the error above)",
                                                     source->generic_string() );
        DropEdit( *source ); // the record states what was imported now; the next ask reads it
        return BOOLSUCCESS;
    }

    Common::BoolResultStr ReimportWithNewFile( const std::filesystem::path& assetPath,
                                               const std::filesystem::path& newFile )
    {
        const auto source = ImportSourceOfAsset( assetPath );
        if ( !source )
            return Common::MakeFormattedError<bool>( "'{}' has no import source to replace",
                                                     assetPath.generic_string() );
        std::error_code ec;
        if ( !std::filesystem::is_regular_file( newFile, ec ) )
            return Common::MakeFormattedError<bool>( "'{}' is not a file", newFile.generic_string() );
        const auto lower = []( std::string text )
        {
            std::transform( text.begin(), text.end(), text.begin(),
                            []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
            return text;
        };
        if ( lower( newFile.extension().string() ) != lower( source->extension().string() ) )
            return Common::MakeFormattedError<bool>( "'{}' is a {} file; '{}' reimports from {} only",
                                                     newFile.generic_string(), newFile.extension().string(),
                                                     assetPath.generic_string(), source->extension().string() );
        if ( !std::filesystem::equivalent( newFile, *source, ec ) &&
             !std::filesystem::copy_file( newFile, *source, std::filesystem::copy_options::overwrite_existing,
                                          ec ) )
            return Common::MakeFormattedError<bool>( "'{}' could not replace '{}': {}", newFile.generic_string(),
                                                     source->generic_string(), ec.message() );
        return Reimport( assetPath );
    }

    Common::BoolResultStr SetSectionUniformScale( const std::filesystem::path& assetPath, const float scale )
    {
        const auto edit = SectionEditOf( assetPath );
        if ( !edit )
            return Common::MakeError<bool>( edit.GetError() );
        return SetUniformScale( edit.GetValue()->Edit, scale );
    }

    Common::BoolResultStr SetSectionUpAxis( const std::filesystem::path&   assetPath,
                                            const Assets::MeshSourceUpAxis axis )
    {
        const auto edit = SectionEditOf( assetPath );
        if ( !edit )
            return Common::MakeError<bool>( edit.GetError() );
        SetUpAxis( edit.GetValue()->Edit, axis );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr SetStoredVirtualGeometry( const std::filesystem::path& assetPath, const bool on )
    {
        if ( assetPath.extension() != ".stmesh" )
            return Common::MakeFormattedError<bool>( "'{}' is not a static mesh asset",
                                                     assetPath.generic_string() );
        // A mesh whose source sits beside it under its own name stores what its import made: the flag is the
        // import's (its record), so it is set on the section's working copy and the mesh reimported from it.
        if ( Common::Content::MeshSourceBeside( assetPath ) && !Assets::IsEditedImportedMesh( assetPath ) )
        {
            const auto source = ImportSourceOfAsset( assetPath );
            if ( !source )
                return Common::MakeFormattedError<bool>( "'{}' has no import source", assetPath.generic_string() );
            const auto kind = RecordedImportKind( *source );
            if ( !kind )
                return Common::MakeError<bool>( kind.GetError() );
            const auto edit = EditOf( *source );
            if ( !edit )
                return Common::MakeError<bool>( edit.GetError() );
            if ( const auto set = SetVirtualGeometry( edit.GetValue()->Edit, on, kind.GetValue() ); !set )
                return set;
            return Reimport( assetPath );
        }

        // THE ASSET IS THE FILE (a node of a split import, an edited import, a modeling output): UE's Asset
        // Actions > Nanite > Enable - the stored flag changes and the mesh is derived again from the geometry the
        // file holds (the DDC key states the flag, MeshDerivedData), with no source file read.
        auto asset = Assets::ReadMeshSourceAssetFile( assetPath );
        if ( !asset )
            return Common::MakeError<bool>( asset.GetError() );
        Assets::MeshSourceAsset stored = asset.ExtractValue();
        if ( stored.Kind != Common::Content::ContentKind::StaticMesh || stored.Source.Skin.has_value() )
            return Common::MakeFormattedError<bool>(
                 "'{}': Virtual Geometry: a skinned mesh is drawn by the binned "
                 "path (its clusters would be one pose)",
                 assetPath.generic_string() );
        if ( stored.Import.Settings.VirtualGeometry == on )
            return BOOLSUCCESS;
        Assets::MeshImportSettings settings = stored.Import.Settings;
        settings.VirtualGeometry            = on;
        // The materials are judged where the import wrote them (MaterialAdoption: beside the import source); a
        // mesh with no import source names its own folder.
        const std::filesystem::path materialsOf =
             stored.Import.Provenance == Assets::MeshSourceProvenance::Imported &&
                       !stored.Import.SourceFile.empty()
                  ? Common::AssetHandle::PathForStableKey( stored.Import.SourceFile )
                  : assetPath;
        if ( const auto refused = VirtualGeometryRefusal( settings, stored.Source.MaterialSlots, materialsOf ) )
            return Common::MakeFormattedError<bool>( "'{}': Virtual Geometry refused: {}",
                                                     assetPath.generic_string(), *refused );
        stored.Import.Settings = settings;
        if ( auto written = Assets::WriteMeshSourceAssetFile( assetPath, stored ); !written )
            return Common::MakeFormattedError<bool>( "'{}' could not be written: {}", assetPath.generic_string(),
                                                     written.GetError() );
        Assets::ContentRegistry::NoteFile( assetPath );
        // Every entity that draws it draws the new derivation from now (ModelingToolTargetAsset's commit): the
        // parsed payload and the built GPU mesh are dropped and read again now, the user waiting on the command.
        const auto row = Assets::ContentRegistry::RowAtPath( Common::Content::ContentKind::StaticMesh, assetPath );
        if ( !row )
            return Common::MakeFormattedError<bool>( "'{}' was written but the registry has no row for it",
                                                     assetPath.generic_string() );
        auto* service = Runtime::ResourceRegistry::GetMeshService();
        if ( auto* loaded = service->GetAsset( row->Handle ); loaded != nullptr )
        {
            if ( auto unloaded = loaded->Unload(); !unloaded.IsSuccess() )
                return Common::MakeFormattedError<bool>( "'{}' was written but not reloaded: {}",
                                                         assetPath.generic_string(), unloaded.GetError() );
            service->EvictBuilt( row->Handle );
            if ( !service->LoadNow( row->Handle ) )
                return Common::MakeFormattedError<bool>(
                     "'{}' was written but could not be rebuilt (the log names "
                     "why)",
                     assetPath.generic_string() );
        }
        LOG_INFO( "[Import] '{}': Virtual Geometry {}", assetPath.generic_string(), on ? "on" : "off" );
        return BOOLSUCCESS;
    }
} // namespace Desert::Editor::ImportOptions
