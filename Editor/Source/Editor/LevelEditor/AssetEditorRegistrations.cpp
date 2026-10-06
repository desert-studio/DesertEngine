#include "Editor/LevelEditor/AssetEditorRegistrations.hpp"

#include "Editor/LevelEditor/DocumentHost.hpp"
#include "Editor/LevelEditor/SceneWorkspace.hpp"
#include "Editor/Core/AssetOpen.hpp"
#include "Editor/Core/EditorSubject.hpp"
#include "Editor/Core/IconsMaterialDesignIcons.hpp"
#include "Editor/Core/SubjectEditorRegistry.hpp"
#include "Editor/Panels/Animation/AnimGraphPanel.hpp"
#include "Editor/Panels/AnimationEditor/AnimationEditorDocument.hpp"
#include "Editor/Panels/Clouds/CloudDocumentOpen.hpp"
#include "Editor/Panels/Clouds/CloudLayoutPanel.hpp"
#include "Editor/Panels/Clouds/CloudModellingVolumePanel.hpp"
#include "Editor/Panels/Clouds/CloudNoiseVolumePanel.hpp"
#include "Editor/Panels/Clouds/CloudTypePanel.hpp"
#include "Editor/Panels/MaterialEditor/MaterialDocumentOpen.hpp"
#include "Editor/Panels/MaterialEditor/MaterialEditorPanel.hpp"
#include "Editor/Panels/NodeGraph/NodeGraphPanel.hpp"
#include "Editor/Panels/NodeGraph/ShaderGraphDocumentOpen.hpp"
#include "Editor/Panels/Particles/ParticleEditorPanel.hpp"
#include "Editor/Panels/Sequencer/SequencerPanel.hpp"
#include "Editor/Panels/SkyboxViewer/SkyboxViewerDocument.hpp"
#include "Editor/Panels/StaticMeshViewer/StaticMeshViewerDocument.hpp"
#include "Editor/Panels/TextureViewer/TextureViewerDocument.hpp"
#include "Editor/Panels/ControlRig/ControlRigDocument.hpp"
#include "Editor/Panels/UI/UIEditorPanel.hpp"
#include <Common/Content/ContentKinds.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Animation/AnimationLibrary.hpp>
#include <Engine/Animation/Timeline/Hosts.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/CloudLayout.hpp>
#include <Engine/Assets/CloudModellingVolume.hpp>
#include <Engine/Assets/CloudNoiseVolume.hpp>
#include <Engine/Assets/CloudTypeData.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Assets/Serialization/ShaderGraph.hpp>
#include <Engine/Assets/ShaderGraphAsset.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>

#include <memory>
#include <string>

namespace Desert::Editor
{
    namespace
    {
        // Does the entity @p owner in the ACTIVE scene carry component T? The presence test every
        // component-subject registration is built from (SubjectEditorRegistry::Registration::Exists) —
        // written once, templated, because five copies of the selection-to-entity-to-component dance is
        // how one of them comes to be missing the null check.
        template <typename ComponentT>
        [[nodiscard]] bool ActiveEntityHasComponent( const SceneWorkspace& workspace, const Common::UUID& owner )
        {
            if ( !workspace.ActiveScene() || owner.IsNull() )
                return false;
            const auto entOpt = workspace.ActiveScene()->FindEntityByID( owner );
            return entOpt && entOpt->get().HasComponent<ComponentT>();
        }
    } // namespace

    void RegisterAssetEditors( DocumentHost& documents, SceneWorkspace& workspace,
                               std::shared_ptr<Assets::AssetManager>&              assetManager,
                               const std::unique_ptr<Animation::AnimationLibrary>& animationLibrary )
    {
        // ── WHICH EDITOR OPENS WHICH KIND OF SUBJECT ──────────────────────────────────────────────────
        //
        // Double-clicking a `.demat` in the browser opens ONE window bound to THAT material, and a second
        // material is a second window; the four cloud formats follow the same rule, and so — since U7 — do
        // the two kinds of subject that are not files at all. Adding the next kind is another block here
        // rather than another branch in FileExplorerPanel and another file-static inbox beside it. See
        // Editor/Core/SubjectEditorRegistry.hpp.
        //
        // THE NAME AND THE ICON ARE PART OF THE REGISTRATION. They used to be two hand-written tables in
        // EditorLayer.cpp keyed on Assets::AssetTypeID, so a new kind meant three edits and only one of them was
        // here; the two that were not are the ones that fell behind.
        using Registration = SubjectEditorRegistry::Registration;

        documents.SubjectEditors().Register(
             AssetSubjectType( static_cast<uint32_t>( Assets::AssetTypeID::Material ) ),
             Registration{
                  "Material", ICON_MDI_PALETTE_SWATCH,
                  [&assetManager]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument>
                  {
                      // THE LOAD LIVES HERE, not in the routes that ask: a handle from a Details
                      // field may name a material that is only a record, and every route ends here.
                      const Assets::AssetHandle handle( subject.Owner );
                      auto                      ready = assetManager
                                                             ? EnsureMaterialLoaded( *assetManager, handle )
                                                             : Common::MakeFormattedError<Assets::Asset<Assets::SurfaceMaterialAsset>>(
                                               "no asset manager" );
                      if ( !ready.IsSuccess() )
                      {
                          LOG_ERROR( "[MaterialEditor] {} — no window opened.", ready.GetError() );
                          return nullptr;
                      }
                      // The window draws through the per-slot route, which resolves through the
                      // material service; registered LAZILY (the shell only), the first Get builds it.
                      if ( auto* materialService = Runtime::ResourceRegistry::GetMaterialService() )
                      {
                          if ( materialService->Get( handle ) == nullptr )
                              materialService->RegisterAsset( ready.GetValue() );
                      }
                      return std::make_unique<Editor::MaterialEditorPanel>( handle, assetManager );
                  },
                  [&assetManager]( const SubjectId& subject ) {
                      return assetManager &&
                             assetManager->FindMetadataByHandle( Assets::AssetHandle( subject.Owner ) ) != nullptr;
                  } } );

        // THE TEXTURE VIEWER. Read-only, no renderer slot: it draws the image the texture service owns.
        documents.SubjectEditors().Register(
             AssetSubjectType( static_cast<uint32_t>( Assets::AssetTypeID::Texture2D ) ),
             Registration{
                  "Texture2D", ICON_MDI_IMAGE,
                  [&assetManager]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument>
                  {
                      return std::make_unique<Editor::TextureViewerDocument>( Assets::AssetHandle( subject.Owner ),
                                                                              assetManager.get() );
                  },
                  [&assetManager]( const SubjectId& subject ) {
                      return assetManager &&
                             assetManager->FindMetadataByHandle( Assets::AssetHandle( subject.Owner ) ) != nullptr;
                  } } );

        // THE CONTROL RIG EDITOR (07_panels_design §11.2): elements, the Forwards-solve canvas, the inspector.
        documents.SubjectEditors().Register(
             AssetSubjectType( static_cast<uint32_t>( Assets::AssetTypeID::ControlRig ) ),
             Registration{
                  "Control Rig", ICON_MDI_HUMAN_HANDSUP,
                  [&assetManager]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument>
                  {
                      return std::make_unique<Editor::ControlRigDocument>( Assets::AssetHandle( subject.Owner ),
                                                                           assetManager.get() );
                  },
                  [&assetManager]( const SubjectId& subject ) {
                      return assetManager &&
                             assetManager->FindMetadataByHandle( Assets::AssetHandle( subject.Owner ) ) != nullptr;
                  } } );

        // THE SKYBOX VIEWER. Claims a renderer slot (a PreviewViewport), so it lives under the slot census below.
        documents.SubjectEditors().Register(
             AssetSubjectType( static_cast<uint32_t>( Assets::AssetTypeID::Skybox ) ),
             Registration{
                  "Skybox", ICON_MDI_IMAGE_FILTER_HDR,
                  [&assetManager]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument>
                  {
                      return std::make_unique<Editor::SkyboxViewerDocument>( Assets::AssetHandle( subject.Owner ),
                                                                             assetManager.get() );
                  },
                  [&assetManager]( const SubjectId& subject ) {
                      return assetManager &&
                             assetManager->FindMetadataByHandle( Assets::AssetHandle( subject.Owner ) ) != nullptr;
                  } } );

        // THE MESH EDITORS. Also renderer-slot claimants. AssetTypeID::Mesh covers `.stmesh` and `.skmesh`: the
        // static one opens the static mesh viewer, the skinned one Persona's Mesh mode (Core::PersonaModeFor).
        documents.SubjectEditors().Register(
             AssetSubjectType( static_cast<uint32_t>( Assets::AssetTypeID::Mesh ) ),
             Registration{
                  "StaticMesh", ICON_MDI_CUBE_OUTLINE,
                  [&documents, &assetManager]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument>
                  {
                      const Assets::AssetHandle handle( subject.Owner );
                      const auto* meta = assetManager ? assetManager->FindMetadataByHandle( handle ) : nullptr;
                      if ( meta != nullptr && Core::PersonaModeFor( *meta ) == Core::PersonaMode::Mesh )
                          return std::make_unique<Editor::AnimationEditorDocument>(
                               handle, Core::PersonaMode::Mesh, assetManager.get(), &documents.SubjectEditors() );
                      return std::make_unique<Editor::StaticMeshViewerDocument>( handle, assetManager.get() );
                  },
                  [&assetManager]( const SubjectId& subject ) {
                      return assetManager &&
                             assetManager->FindMetadataByHandle( Assets::AssetHandle( subject.Owner ) ) != nullptr;
                  } } );

        // THE ANIMATION EDITOR (ANV1a) — Persona's Animation mode. A renderer-slot claimant like the mesh viewer.
        documents.SubjectEditors().Register(
             AssetSubjectType( static_cast<uint32_t>( Assets::AssetTypeID::Animation ) ),
             Registration{
                  "Animation", ICON_MDI_RUN,
                  [&documents, &assetManager]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument>
                  {
                      return std::make_unique<Editor::AnimationEditorDocument>(
                           Assets::AssetHandle( subject.Owner ), Core::PersonaMode::Animation, assetManager.get(),
                           &documents.SubjectEditors() );
                  },
                  [&assetManager]( const SubjectId& subject ) {
                      return assetManager &&
                             assetManager->FindMetadataByHandle( Assets::AssetHandle( subject.Owner ) ) != nullptr;
                  } } );

        // PERSONA'S SKELETON MODE (ANV1f): the same window, about the rig.
        documents.SubjectEditors().Register(
             AssetSubjectType( static_cast<uint32_t>( Assets::AssetTypeID::Skeleton ) ),
             Registration{
                  "Skeleton", ICON_MDI_RUN,
                  [&documents, &assetManager]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument>
                  {
                      return std::make_unique<Editor::AnimationEditorDocument>(
                           Assets::AssetHandle( subject.Owner ), Core::PersonaMode::Skeleton, assetManager.get(),
                           &documents.SubjectEditors() );
                  },
                  [&assetManager]( const SubjectId& subject ) {
                      return assetManager &&
                             assetManager->FindMetadataByHandle( Assets::AssetHandle( subject.Owner ) ) != nullptr;
                  } } );

        // THE FOUR CLOUD DOCUMENTS. Each takes the raw AssetManager pointer the panels already held, so the
        // move from singleton to document changed the panels' ownership of their subject and nothing about
        // how they reach their assets.
        documents.SubjectEditors().Register(
             AssetSubjectType( static_cast<uint32_t>( Assets::AssetTypeID::CloudNoiseVolume ) ),
             Registration{
                  "CloudNoiseVolume", ICON_MDI_GRID,
                  [&assetManager]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument>
                  {
                      return std::make_unique<Editor::CloudNoiseVolumePanel>( Assets::AssetHandle( subject.Owner ),
                                                                              assetManager.get() );
                  },
                  [&assetManager]( const SubjectId& subject ) {
                      return assetManager &&
                             assetManager->FindMetadataByHandle( Assets::AssetHandle( subject.Owner ) ) != nullptr;
                  } } );
        documents.SubjectEditors().Register(
             AssetSubjectType( static_cast<uint32_t>( Assets::AssetTypeID::CloudType ) ),
             Registration{
                  "CloudType", ICON_MDI_WEATHER_CLOUDY,
                  [&assetManager]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument> {
                      return std::make_unique<Editor::CloudTypePanel>( Assets::AssetHandle( subject.Owner ),
                                                                       assetManager.get() );
                  },
                  [&assetManager]( const SubjectId& subject ) {
                      return assetManager &&
                             assetManager->FindMetadataByHandle( Assets::AssetHandle( subject.Owner ) ) != nullptr;
                  } } );
        documents.SubjectEditors().Register(
             AssetSubjectType( static_cast<uint32_t>( Assets::AssetTypeID::CloudModellingVolume ) ),
             Registration{
                  "CloudModellingVolume", ICON_MDI_CUBE_OUTLINE,
                  [&assetManager]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument>
                  {
                      return std::make_unique<Editor::CloudModellingVolumePanel>(
                           Assets::AssetHandle( subject.Owner ), assetManager.get() );
                  },
                  [&assetManager]( const SubjectId& subject ) {
                      return assetManager &&
                             assetManager->FindMetadataByHandle( Assets::AssetHandle( subject.Owner ) ) != nullptr;
                  } } );
        // The layout document also READS the active scene's cloud layer for its preview numbers — the scene
        // is an input, never a second subject, and SetScene keeps it following the focused viewport exactly
        // as the singleton did.
        documents.SubjectEditors().Register(
             AssetSubjectType( static_cast<uint32_t>( Assets::AssetTypeID::CloudLayout ) ),
             Registration{
                  "CloudLayout", ICON_MDI_IMAGE_FILTER_HDR,
                  [&workspace, &assetManager]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument>
                  {
                      return std::make_unique<Editor::CloudLayoutPanel>(
                           Assets::AssetHandle( subject.Owner ), workspace.ActiveScene(), assetManager.get() );
                  },
                  [&assetManager]( const SubjectId& subject ) {
                      return assetManager &&
                             assetManager->FindMetadataByHandle( Assets::AssetHandle( subject.Owner ) ) != nullptr;
                  } } );

        // THE SHADER GRAPH. The seventh asset document and the last window in this editor to become one;
        // the display name is the graph's own `Name` (the file's stem when it has none), resolved here
        // because a document must not need the asset manager to know what it is called.
        documents.SubjectEditors().Register(
             Editor::NodeGraphPanel::SubjectType(),
             Registration{
                  "ShaderGraph", ICON_MDI_GRAPH,
                  [&assetManager]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument>
                  {
                      const Assets::AssetHandle handle( subject.Owner );
                      std::string               name = "Shader Graph";
                      if ( assetManager )
                      {
                          if ( const auto graph = assetManager->FindByHandle<Assets::ShaderGraphAsset>( handle ) )
                          {
                              name = graph->GetDisplayName();
                          }
                      }
                      return std::make_unique<Editor::NodeGraphPanel>( handle, name, assetManager );
                  },
                  [&assetManager]( const SubjectId& subject ) {
                      return assetManager &&
                             assetManager->FindMetadataByHandle( Assets::AssetHandle( subject.Owner ) ) != nullptr;
                  } } );

        // ── THE THREE DOCUMENTS WHOSE SUBJECT IS NOT A FILE ───────────────────────────────────────────
        //
        // This is what U7 bought. All three were singleton panels that drew "whatever entity is selected"
        // (or, for the UI editor, whichever canvas the registry listed first), and all three are now opened
        // FROM the component that holds their data, by a button in Details — which is the thing the owner
        // asked for and the thing the old asset-keyed seam could not express.
        //
        // THE FACTORY TAKES THE SCENE THAT IS ACTIVE AT THE MOMENT OF THE OPEN, and that is deliberate:
        // the subject is an entity UUID, and a UUID belongs to ONE registry. Captured by reference to the
        // member so a document opened from the second scene view binds to the second scene — and then
        // never follows the fanout again (AnimGraphPanel::SetScene is a documented no-op).
        //
        // The DISPLAY name is the entity's, resolved once here rather than by the document: the document
        // must not need a scene to know what it is called, and a name is a label while the subject is the
        // identity — renaming the entity does not open a second window.
        documents.SubjectEditors().Register(
             Editor::AnimGraphPanel::SubjectType(),
             Registration{ Editor::AnimGraphPanel::kComponentTypeName, ICON_MDI_STATE_MACHINE,
                           [&documents, &workspace, &assetManager,
                            &animationLibrary]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument>
                           {
                               return std::make_unique<Editor::AnimGraphPanel>(
                                    subject, documents.SubjectEntityName( subject, "Anim Graph" ),
                                    workspace.ActiveScene(), animationLibrary.get(), assetManager.get() );
                           },
                           [&workspace]( const SubjectId& subject ) {
                               return ActiveEntityHasComponent<ECS::AnimationComponent>( workspace,
                                                                                         subject.Owner );
                           } } );
        documents.SubjectEditors().Register(
             Editor::ParticleEditorPanel::SubjectType(),
             Registration{
                  Editor::ParticleEditorPanel::kComponentTypeName, ICON_MDI_CREATION,
                  [&documents, &workspace]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument>
                  {
                      return std::make_unique<Editor::ParticleEditorPanel>(
                           subject, documents.SubjectEntityName( subject, "Particles" ), workspace.ActiveScene() );
                  },
                  [&workspace]( const SubjectId& subject ) {
                      return ActiveEntityHasComponent<ECS::ParticleEmitterComponent>( workspace, subject.Owner );
                  } } );
        // THE UI CANVAS. Its window owns a Framebuffer and a Render2D rather than a SceneRenderer, so it
        // takes none of the six renderer slots and says so (UIEditorPanel::ClaimsView) — a document
        // that renders is not automatically a document that costs a slot.
        documents.SubjectEditors().Register(
             Editor::UIEditorPanel::SubjectType(),
             Registration{
                  Editor::UIEditorPanel::kComponentTypeName, ICON_MDI_VIEW_DASHBOARD,
                  [&documents, &workspace]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument>
                  {
                      return std::make_unique<Editor::UIEditorPanel>(
                           subject, documents.SubjectEntityName( subject, "UI" ), workspace.ActiveScene() );
                  },
                  [&workspace]( const SubjectId& subject )
                  { return ActiveEntityHasComponent<ECS::UICanvasComponent>( workspace, subject.Owner ); } } );

        // THE TWO TIMELINES. One class, two subject types, and the argument for why they are two and not
        // one is written out at the top of SequencerPanel.hpp: the rig timeline keys BONE POSES and is
        // therefore about the SkinnedMeshComponent, while the anim graph next door keys states and
        // transitions and is about the AnimationComponent — two editors under one key is refused by this
        // registry by name, which is what made the question get answered.
        //
        // THE PRESENCE TEST ASKS FOR BOTH COMPONENTS, not just the one the subject is named after: a rig
        // with no AnimationComponent has no clip to pick and no animator to pose, so offering the entry
        // would open a window with nothing in it. It is the same predicate SequencerPanel::IsSubjectAlive
        // answers with, asked of the current scene rather than of the document's own — see
        // SubjectEditorRegistry::Registration::Exists for why those are two questions.
        documents.SubjectEditors().Register(
             Editor::SequencerPanel::SkeletalSubjectType(),
             Registration{
                  Editor::SequencerPanel::kSkeletalComponentTypeName, ICON_MDI_CHART_TIMELINE,
                  [&documents, &workspace, &assetManager,
                   &animationLibrary]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument>
                  {
                      return std::make_unique<Editor::SequencerPanel>(
                           subject, documents.SubjectEntityName( subject, "Sequencer" ),
                           Editor::SequencerPanel::Timeline::Skeletal, workspace.ActiveScene(),
                           animationLibrary.get(), assetManager.get() );
                  },
                  [&workspace]( const SubjectId& subject )
                  {
                      return ActiveEntityHasComponent<ECS::SkinnedMeshComponent>( workspace, subject.Owner ) &&
                             ActiveEntityHasComponent<ECS::AnimationComponent>( workspace, subject.Owner );
                  } } );
        documents.SubjectEditors().Register(
             Editor::SequencerPanel::UISubjectType(),
             Registration{ Editor::SequencerPanel::kUIComponentTypeName, ICON_MDI_CHART_TIMELINE_VARIANT,
                           [&documents, &workspace, &assetManager,
                            &animationLibrary]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument>
                           {
                               return std::make_unique<Editor::SequencerPanel>(
                                    subject, documents.SubjectEntityName( subject, "UI Timeline" ),
                                    Editor::SequencerPanel::Timeline::UI, workspace.ActiveScene(),
                                    animationLibrary.get(), assetManager.get() );
                           },
                           [&workspace]( const SubjectId& subject ) {
                               return ActiveEntityHasComponent<ECS::UIAnimComponent>( workspace, subject.Owner );
                           } } );

        // THE LEVEL SEQUENCE (ANIM-LSEQ) — UE: double-clicking a Level Sequence opens the Sequencer over it.
        // Its subject is the ASSET; the scene it previews is the main scene, given back when it closes.
        documents.SubjectEditors().Register(
             AssetSubjectType( static_cast<uint32_t>( Assets::AssetTypeID::LevelSequence ) ),
             Registration{
                  "LevelSequence", ICON_MDI_MOVIE_OPEN,
                  [&workspace, &assetManager,
                   &animationLibrary]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument>
                  {
                      const auto* meta =
                           assetManager
                                ? assetManager->FindMetadataByHandle( Assets::AssetHandle( subject.Owner ) )
                                : nullptr;
                      return std::make_unique<Editor::SequencerPanel>(
                           subject,
                           meta != nullptr ? meta->Filepath.stem().string() : std::string( "Level Sequence" ),
                           Editor::SequencerPanel::Timeline::Level, workspace.ActiveScene(),
                           animationLibrary.get(), assetManager.get() );
                  },
                  [&assetManager]( const SubjectId& subject ) {
                      return assetManager &&
                             assetManager->FindMetadataByHandle( Assets::AssetHandle( subject.Owner ) ) != nullptr;
                  } } );

        // ── AND HOW A PATH BECOMES ONE OF THEM ────────────────────────────────────────────────────────
        //
        // The asset browser's double-click used to carry a chain of `else if` over the file types, one arm
        // per kind of document, and EditorLayer.cpp carried a second copy of the same chain. Registered here
        // instead, beside the editors they feed, so the browser asks once and a new format is a line in
        // this block rather than an edit in two files somebody has to remember exist.
        //
        // AND WHICH EXTENSIONS EACH ONE ANSWERS FOR. Taken from the format's own constant, never spelled
        // again here: the palette ENUMERATES the project's openable files against this list, so a literal
        // that drifted from the resolver's would produce a list of entries the resolver then refuses.
        documents.SubjectEditors().RegisterPathOpener(
             { std::string( Common::Constants::Extensions::MATERIAL_EXTENSION ) },
             [&documents, &assetManager]( const std::string& path )
             {
                 switch ( RequestMaterialDocument( assetManager.get(), path, documents.SubjectEditors() ) )
                 {
                     case MaterialDocumentRequest::NotAMaterialPath:
                         return SubjectEditorRegistry::PathOpenOutcome::NotMine;
                     case MaterialDocumentRequest::Failed:
                         return SubjectEditorRegistry::PathOpenOutcome::Failed;
                     case MaterialDocumentRequest::Requested:
                         return SubjectEditorRegistry::PathOpenOutcome::Requested;
                 }
                 return SubjectEditorRegistry::PathOpenOutcome::NotMine;
             } );
        documents.SubjectEditors().RegisterPathOpener(
             { std::string( Assets::kCloudNoiseVolumeExtension ), std::string( Assets::kCloudTypeExtension ),
               std::string( Assets::kCloudModellingVolumeExtension ),
               std::string( Assets::kCloudLayoutExtension ) },
             [&assetManager]( const std::string& path )
             {
                 switch ( RequestCloudDocument( assetManager.get(), path ) )
                 {
                     case CloudDocumentRequest::NotACloudPath:
                         return SubjectEditorRegistry::PathOpenOutcome::NotMine;
                     case CloudDocumentRequest::Failed:
                         return SubjectEditorRegistry::PathOpenOutcome::Failed;
                     case CloudDocumentRequest::Requested:
                         return SubjectEditorRegistry::PathOpenOutcome::Requested;
                 }
                 return SubjectEditorRegistry::PathOpenOutcome::NotMine;
             } );
        documents.SubjectEditors().RegisterPathOpener(
             { std::string( Assets::kTextureAssetExtension ) },
             [&documents, &assetManager]( const std::string& path )
             {
                 // ONE opener per extension (OpenPath's rule), so the `.detex` split is made here: a panorama
                 // whose header says Skybox opens the skybox viewer, every other `.detex` the texture viewer.
                 if ( const auto sky =
                           RequestSkyboxDocument( assetManager.get(), path, documents.SubjectEditors() );
                      sky != SubjectEditorRegistry::PathOpenOutcome::NotMine )
                     return sky;
                 return RequestTextureDocument( assetManager.get(), path, documents.SubjectEditors() );
             } );
        documents.SubjectEditors().RegisterPathOpener(
             { std::string( Animation::Timeline::kLevelSequenceExtension ) },
             [&documents, &assetManager]( const std::string& path ) {
                 return Editor::RequestLevelSequenceDocument( assetManager.get(), path,
                                                              documents.SubjectEditors() );
             } );
        documents.SubjectEditors().RegisterPathOpener(
             { std::string( Common::Constants::Extensions::STATIC_MESH ) },
             [&documents, &assetManager]( const std::string& path )
             { return RequestStaticMeshDocument( assetManager.get(), path, documents.SubjectEditors() ); } );
        documents.SubjectEditors().RegisterPathOpener(
             { std::string( Editor::kAnimationClipExtension ),
               std::string( Common::Content::KindSpec( Common::Content::ContentKind::SkinnedMesh ).Extension ),
               std::string( Common::Content::KindSpec( Common::Content::ContentKind::Skeleton ).Extension ) },
             [&documents, &assetManager]( const std::string& path )
             { return RequestAnimationEditorDocument( assetManager.get(), path, documents.SubjectEditors() ); } );
        documents.SubjectEditors().RegisterPathOpener(
             { std::string( Assets::Serialization::kControlRigExtension ) },
             [&documents, &assetManager]( const std::string& path )
             { return RequestControlRigDocument( assetManager.get(), path, documents.SubjectEditors() ); } );
        documents.SubjectEditors().RegisterPathOpener(
             { std::string( Assets::Serialization::ShaderGraph::kShaderGraphExtension ) },
             [&assetManager]( const std::string& path )
             {
                 switch ( RequestShaderGraphDocument( assetManager.get(), path ) )
                 {
                     case ShaderGraphDocumentRequest::NotAGraphPath:
                         return SubjectEditorRegistry::PathOpenOutcome::NotMine;
                     case ShaderGraphDocumentRequest::Failed:
                         return SubjectEditorRegistry::PathOpenOutcome::Failed;
                     case ShaderGraphDocumentRequest::Requested:
                         return SubjectEditorRegistry::PathOpenOutcome::Requested;
                 }
                 return SubjectEditorRegistry::PathOpenOutcome::NotMine;
             } );
    }
} // namespace Desert::Editor
