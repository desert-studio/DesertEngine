#include "CloudsPanel.hpp"

#include "CloudDocumentOpen.hpp"

#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/OpenDocuments.hpp>
// SAFE HERE, unlike in the headers of this folder: nothing in this file spells `Core::` meaning
// Desert::Core — the two uses below are written out as `Desert::Core::Scene` and
// `Desert::Core::Formats` — so opening Desert::Editor::Core cannot rebind anything. CloudDocumentOpen.cpp
// carries the same note for the same include.
#include <Editor/Core/PanelRequests.hpp>
#include <Editor/Panels/PropertyEditor/ComponentWidgetRegistry.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/CloudTypeAsset.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/HeroCloudComponent.hpp>
#include <Engine/ECS/VolumetricCloudComponent.hpp>
#include <Engine/Graphic/Clouds/CloudMaterialValues.hpp>
#include <Engine/Graphic/Clouds/CloudPayload.hpp>
#include <Engine/Graphic/Shader.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <filesystem>
#include <optional>

namespace Desert::Editor
{
    // The editor's ImGui lives in the global namespace; unqualified `ImGui::` inside `Desert::` would
    // resolve to `Desert::ImGui`, which is the engine's own runtime UI. Every panel in this folder opens
    // with this alias for the same reason.
    namespace ImGui = ::ImGui;

    namespace
    {
        // ── THE STAGE THE DETAILS PANEL ASKED FOR ──────────────────────────────────────────────────────
        //
        // A static inbox with exactly the shape Core::PanelRequests has, and drained the same way: the
        // requester needs neither this panel's instance nor its header, and the panel consumes the request
        // once, on its own next frame. Guarded by nothing because the editor's UI is single-threaded and
        // both ends run inside the ImGui pass — the same assumption PanelRequests and SubjectOpenRequests
        // are both built on.
        std::optional<CloudStage>& PendingStage()
        {
            static std::optional<CloudStage> s_Pending;
            return s_Pending;
        }

        // How long a stage waits for the document it asked for before it says something went wrong.
        //
        // An open is QUEUED and serviced between frames, so one frame is normal; the asset behind it may
        // also still be loading. Thirty is the same "unambiguously not a transient" threshold the renderer
        // slot release uses, and past it the pane offers Try again rather than spinning for ever — a
        // refusal (the six renderer slots are spoken for) never resolves on its own, and a pane that said
        // "opening..." at it would be lying with a spinner.
        constexpr uint32_t kFramesBeforeOpenLooksStuck = 30;

        // The rail's own width. Fixed rather than a splitter: the six labels are known and short, and a
        // draggable divider on a window whose left column can never usefully change size is a control that
        // does nothing (§1.3).
        constexpr float kRailWidth = 250.0f;

        [[nodiscard]] bool IsNull( const Common::AssetHandle& handle )
        {
            return static_cast<uint64_t>( handle ) == 0u;
        }

        // WHICH AssetTypeID EACH STAGE RESOLVES TO, stated once. Two readers — the rail and the pane — and
        // they must agree: a rail row that named a `.dclayout` while the pane opened it as a material would
        // be two answers to one question, which is the shape this whole window was built to remove.
        // CloudStages.hpp takes them as plain numbers so it stays clear of Engine/Assets; the suite
        // (Desert/Tests/Editor/CloudStages) asserts they are the ones the editors are registered under.
        [[nodiscard]] CloudStageAssetTypes StageAssetTypes()
        {
            return CloudStageAssetTypes{ static_cast<uint32_t>( Assets::AssetTypeID::Material ),
                                         static_cast<uint32_t>( Assets::AssetTypeID::CloudLayout ),
                                         static_cast<uint32_t>( Assets::AssetTypeID::CloudType ),
                                         static_cast<uint32_t>( Assets::AssetTypeID::CloudNoiseVolume ),
                                         static_cast<uint32_t>( Assets::AssetTypeID::CloudModellingVolume ) };
        }
    } // namespace

    void CloudsPanel::OpenAt( const CloudStage stage )
    {
        Core::PanelRequests::Open( kPanelName );
        PendingStage() = stage;
    }

    CloudsPanel::CloudsPanel( const std::shared_ptr<Desert::Core::Scene>&  scene,
                              const std::shared_ptr<Assets::AssetManager>& assets, const OpenDocuments& documents )
         : IPanel( kPanelName, /*showPanel=*/false ), m_Scene( scene ), m_Assets( assets ),
           m_Documents( &documents ), m_UI( std::make_unique<UI::UIHelper>() )
    {
    }

    CloudsPanel::~CloudsPanel() = default;

    void CloudsPanel::SetScene( const std::shared_ptr<Desert::Core::Scene>& scene )
    {
        m_Scene = scene;
        // The selections are indices into THAT scene's chain, so they cannot survive it: slot 3 of one
        // sky is not slot 3 of another, and a hero index is meaningless against a different entity list.
        m_TypeSlot           = 0;
        m_Hero               = 0;
        m_Requested          = SubjectId{};
        m_FramesSinceRequest = 0;
        m_Showed             = SubjectId{};
    }

    std::string CloudsPanel::AssetLabel( const Common::AssetHandle& handle ) const
    {
        if ( IsNull( handle ) )
            return {};
        if ( !m_Assets )
            return "(no asset manager)";
        if ( const auto* metadata = m_Assets->FindMetadataByHandle( Assets::AssetHandle( handle ) ) )
            return std::filesystem::path( metadata->Filepath.string() ).stem().string();
        // A handle the manager does not know is NOT an empty slot, and saying "None" for it would hide a
        // broken reference behind an unauthored one (§1.4).
        return "(missing)";
    }

    CloudChain CloudsPanel::GatherChain() const
    {
        CloudChain chain;
        chain.SelectedTypeSlot = m_TypeSlot;
        chain.SelectedHero     = m_Hero;

        if ( !m_Scene )
            return chain;

        // ── THE HERO BODIES ARE INDEPENDENT OF THE LAYER ───────────────────────────────────────────────
        //
        // Gathered whatever the layer says, because a HeroCloudComponent is on its own entity and a scene
        // may hold one before anybody has added a cloud layer. They still read as empty when there is no
        // layer, because CloudStageSubject answers null for every stage then — which is the honest
        // arrangement: a hero body with no sky around it renders nothing.
        auto& registry = m_Scene->GetRegistry();
        auto  heroView = registry.view<ECS::HeroCloudComponent>();
        for ( auto handle : heroView )
        {
            ECS::Entity   entity( handle, registry );
            CloudHeroBody body;
            body.EntityName = entity.HasComponent<ECS::TagComponent>()
                                   ? entity.GetComponent<ECS::TagComponent>().Tag
                                   : std::string( "hero cloud" );
            body.Volume     = entity.GetComponent<ECS::HeroCloudComponent>().Data.Volume;
            if ( entity.HasComponent<ECS::UUIDComponent>() )
                body.Entity = entity.GetComponent<ECS::UUIDComponent>().UUID;
            chain.HeroBodies.push_back( std::move( body ) );
        }

        auto view = registry.view<ECS::VolumetricCloudComponent>();
        if ( view.begin() == view.end() )
            return chain;

        // THE FIRST CLOUD LAYER, and there is only ever one: the renderer collects a single layer per scene
        // (VolumetricCloudECSSystem), so a second component would be a scene defect rather than a second
        // sky, and picking the first is what every other reader of this component already does.
        ECS::Entity layerEntity( *view.begin(), registry );
        const auto& data = layerEntity.GetComponent<ECS::VolumetricCloudComponent>().Data;

        chain.HasLayer = true;
        chain.Material = data.Material;
        if ( layerEntity.HasComponent<ECS::UUIDComponent>() )
            chain.LayerEntity = layerEntity.GetComponent<ECS::UUIDComponent>().UUID;
        chain.LayerName = layerEntity.HasComponent<ECS::TagComponent>()
                               ? layerEntity.GetComponent<ECS::TagComponent>().Tag
                               : std::string( "the cloud layer" );

        // ── THE LOOK IS THE MATERIAL'S SINCE O1, resolved exactly as the renderer resolves it ──────────
        //
        // Schema defaults first, the `.demat` chain over them. The same three lines the Cloud Layout panel
        // runs, and for the same reason: what this rail names has to be what the sky is rendering, whichever
        // `.demat` the component names and even when it names none.
        const Desert::Core::Formats::ShaderProgramMeta* schema = nullptr;
        if ( const auto shaderService = Runtime::ResourceRegistry::GetShaderService() )
        {
            if ( const auto marchShader = shaderService->GetByName( Graphic::kCloudMaterialShaderName ) )
                schema = &marchShader->GetProgramMeta();
        }
        Graphic::MaterialOverrides overrides;
        if ( !IsNull( data.Material ) )
        {
            if ( auto* materialService = Runtime::ResourceRegistry::GetMaterialService() )
                materialService->ResolveOverrides( data.Material, overrides );
        }
        const Graphic::CloudMaterialValues material = Graphic::BuildCloudMaterialValues( schema, overrides );

        Assets::AssetHandle slots[ECS::kCloudTypeSlots];
        material.TypeSlots( slots );
        static_assert( static_cast<uint32_t>( ECS::kCloudTypeSlots ) == kCloudStageTypeSlots,
                       "the rail's slot count and the engine's must be the same number" );
        for ( uint32_t i = 0; i < kCloudStageTypeSlots; ++i )
            chain.CloudTypes[i] = slots[i];

        chain.LayoutPattern = material.LayoutPattern;
        chain.LayoutMask    = material.LayoutMask;

        // ── STAGE 5 IS NAMED BY STAGE 4, AND THE TYPE HAS ALREADY ANSWERED ─────────────────────────────
        //
        // The noise volume is NOT a material parameter — a `.decloudtype` names its own, as a path inside
        // the file (CloudTypeData::NoiseVolume). That is why the rail's fifth row follows the fourth's
        // selection instead of standing beside it, and it is the correction O9 made to the drawn sheet,
        // which had drawn a Noise Volume slot the material does not have.
        //
        // THE PATH IS NOT RE-RESOLVED HERE, AND THAT IS THE WHOLE OF THE FIX. This used to read the path
        // out of the type's data and hand it to `FindByPath` verbatim. The path is stored RELATIVE TO THE
        // ASSETS ROOT — `Clouds/CloudNoise_FineWisp.dcnv` — while the registry keys a file on
        // AssetHandle::StableKeyForPath, which resolves a relative spelling against the WORKING DIRECTORY
        // and not against that root. So the bare spelling minted the untagged key
        // `Clouds/CloudNoise_FineWisp.dcnv` while the preloader had registered the same file under
        // `assets:Clouds/CloudNoise_FineWisp.dcnv`: two identities for one file, and stage 5 drew "the
        // built-in volume" for the one shipped type that names one. Asking the ASSET for the handle it
        // already bound (CloudTypeAsset::ResolveDependencies does the single join, and the renderer reads
        // the same answer through CloudTypeService::GetNoiseVolume) is one source of truth instead of a
        // second, weaker derivation. Desert/Tests/Engine/AssetPathIdentity pins the mechanism and the
        // CloudStages census pins that nobody re-derives it.
        if ( m_Assets && m_TypeSlot < kCloudStageTypeSlots && !IsNull( chain.CloudTypes[m_TypeSlot] ) )
        {
            const auto type = m_Assets->FindByHandle<Assets::CloudTypeAsset>(
                 Common::UUID( static_cast<uint64_t>( chain.CloudTypes[m_TypeSlot] ) ) );
            if ( type && type->IsReadyForUse() )
            {
                // A null handle here is the BUILT-IN volume, which is not a file and therefore not a
                // document. The pane says so; it does not report a missing asset. A type that NAMES a
                // volume the registry does not hold has already been reported by name, with both
                // spellings, from ResolveDependencies.
                chain.NoiseVolume = type->GetNoiseVolume();
            }
        }

        return chain;
    }

    void CloudsPanel::OnUIRender()
    {
        if ( const auto requested = PendingStage() )
        {
            m_Stage = *requested;
            PendingStage().reset();
            // A jump from Details lands on a stage the window may not have been showing, so the previous
            // stage's open request is no longer what this pane is waiting for.
            m_Requested          = SubjectId{};
            m_FramesSinceRequest = 0;
            m_Showed             = SubjectId{};
        }

        const CloudChain chain = GatherChain();

        DrawHeader( chain );
        ImGui::Separator();

        ImGui::BeginChild( "##clouds_rail", ImVec2( kRailWidth, 0.0f ), true );
        DrawRail( chain );
        ImGui::Separator();
        DrawChainSummary( chain );
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild( "##clouds_stage", ImVec2( 0.0f, 0.0f ), true );
        DrawStagePane( chain );
        ImGui::EndChild();
    }

    void CloudsPanel::DrawHeader( const CloudChain& chain )
    {
        ImGui::TextUnformatted( ICON_MDI_WEATHER_CLOUDY );
        ImGui::SameLine();

        if ( !chain.HasLayer )
        {
            // NOT the same sentence as "the layer is unauthored", and the difference is the whole of §1.4:
            // an empty rail on a scene with no cloud entity reads as an editor that failed to load
            // something.
            ImGui::TextDisabled( "This scene has no cloud layer. Add a Volumetric Cloud component to an "
                                 "entity and the six stages below fill in." );
            return;
        }

        ImGui::Text( "Clouds \xe2\x80\x94 %s", chain.LayerName.c_str() );
        ImGui::SameLine();

        const std::string material = AssetLabel( chain.Material );
        uint32_t          species  = 0;
        for ( uint32_t i = 0; i < kCloudStageTypeSlots; ++i )
            species += IsNull( chain.CloudTypes[i] ) ? 0u : 1u;

        ImGui::TextDisabled( "\xc2\xb7 material %s \xc2\xb7 %u type slot%s filled \xc2\xb7 %zu hero bod%s",
                             material.empty() ? "(schema defaults)" : material.c_str(), species,
                             species == 1u ? "" : "s", chain.HeroBodies.size(),
                             chain.HeroBodies.size() == 1u ? "y" : "ies" );
    }

    void CloudsPanel::DrawRail( const CloudChain& chain )
    {
        ImGui::TextDisabled( "THE SKY, IN BUILD ORDER" );
        ImGui::Spacing();

        const CloudStageAssetTypes types = StageAssetTypes();

        for ( uint32_t i = 0; i < kCloudStageCount; ++i )
        {
            const auto stage = static_cast<CloudStage>( i );

            std::string label    = std::to_string( i + 1 ) + "  " + CloudStageName( stage );
            const bool  selected = ( stage == m_Stage );
            if ( ImGui::Selectable( ( label + "##stage" + std::to_string( i ) ).c_str(), selected,
                                    ImGuiSelectableFlags_None, ImVec2( 0.0f, 34.0f ) ) )
            {
                m_Stage              = stage;
                m_Requested          = SubjectId{};
                m_FramesSinceRequest = 0;
                m_Showed             = SubjectId{};
            }

            // The subtitle is drawn INSIDE the selectable's rectangle rather than after it, so the whole
            // two-line row is one hit target — a row whose second line is not clickable reads as broken.
            const ImVec2 min = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText(
                 ImVec2( min.x + ImGui::GetStyle().ItemSpacing.x, min.y + ImGui::GetTextLineHeight() + 2.0f ),
                 ImGui::GetColorU32( ImGuiCol_TextDisabled ), CloudStageSubtitle( stage ) );

            // WHAT IS IN THE STAGE, on the row. This is the sentence the whole window exists for: six
            // places that never mentioned each other, named together, in the order they feed each other.
            const SubjectId subject = CloudStageSubject( stage, chain, types );
            std::string     what;
            if ( stage == CloudStage::Layer )
                what = chain.HasLayer ? chain.LayerName : std::string();
            else if ( !subject.IsNull() )
                what = AssetLabel( Common::AssetHandle( subject.Owner ) );

            ImGui::Indent( ImGui::GetStyle().ItemSpacing.x );
            if ( what.empty() )
                ImGui::TextDisabled( "nothing yet" );
            else
                ImGui::TextUnformatted( what.c_str() );
            ImGui::Unindent( ImGui::GetStyle().ItemSpacing.x );
            ImGui::Spacing();
        }
    }

    void CloudsPanel::DrawChainSummary( const CloudChain& chain )
    {
        (void)chain;

        // ── THE ONE PLACE THE WHOLE CHAIN IS STATED ────────────────────────────────────────────────────
        //
        // O7's finding, in six lines. Before this window an artist could open any one of the six editors
        // and it would tell them nothing about the other five; this says what reads what, so the rail's
        // order is an argument rather than a list.
        ImGui::TextDisabled( "WHAT FEEDS WHAT" );
        ImGui::Spacing();
        ImGui::TextWrapped( "The Layer holds budgets and geometry, and names one thing: its Material." );
        ImGui::TextWrapped( "The Material is the whole look, and names the Layout, the four Cloud Types and "
                            "nothing else." );
        ImGui::TextWrapped( "A Cloud Type names its own Noise volume \xe2\x80\x94 the noise belongs to the "
                            "type, not to the material." );
        ImGui::TextWrapped( "A Hero body is placed on its own entity and sits over all of it." );
    }

    void CloudsPanel::DrawStagePane( const CloudChain& chain )
    {
        const CloudStageAssetTypes types = StageAssetTypes();

        const SubjectId subject = CloudStageSubject( m_Stage, chain, types );

        // The stage's own header: which stage, and what it is editing right now.
        ImGui::Text( "%u \xc2\xb7 %s", static_cast<uint32_t>( m_Stage ) + 1u, CloudStageName( m_Stage ) );
        ImGui::SameLine();
        if ( !subject.IsNull() && m_Stage != CloudStage::Layer )
        {
            const std::string name = AssetLabel( Common::AssetHandle( subject.Owner ) );
            ImGui::TextDisabled( "\xe2\x80\x94 %s", name.c_str() );
        }
        else if ( m_Stage == CloudStage::Layer && chain.HasLayer )
        {
            ImGui::TextDisabled( "\xe2\x80\x94 on %s", chain.LayerName.c_str() );
        }

        // ── THE TWO STAGES THAT ARE A CHOICE BEFORE THEY ARE AN EDITOR ────────────────────────────────
        //
        // A layer has FOUR type slots and a scene may have several hero bodies, so those two rails rows
        // stand for a set rather than for one thing. The chooser is here, above the editor, and it moves
        // the SELECTION only — binding a slot is the material's own Inputs table (stage 2), which is the
        // setter every widget already calls.
        if ( m_Stage == CloudStage::Types && chain.HasLayer )
        {
            for ( uint32_t i = 0; i < kCloudStageTypeSlots; ++i )
            {
                if ( i != 0 )
                    ImGui::SameLine();
                const std::string label = IsNull( chain.CloudTypes[i] )
                                               ? "Slot " + std::to_string( i + 1 ) + ": empty"
                                               : AssetLabel( chain.CloudTypes[i] );
                if ( ImGui::RadioButton( ( label + "##typeslot" + std::to_string( i ) ).c_str(),
                                         m_TypeSlot == i ) )
                {
                    m_TypeSlot           = i;
                    m_Requested          = SubjectId{};
                    m_FramesSinceRequest = 0;
                    m_Showed             = SubjectId{};
                }
            }
            ImGui::TextDisabled( "Which kind of cloud sits in a slot is bound in the material's own Inputs "
                                 "table \xe2\x80\x94 stage 2 of this window." );
            if ( ImGui::SmallButton( "Go to 2 \xc2\xb7 Material" ) )
                m_Stage = CloudStage::Material;
        }
        else if ( m_Stage == CloudStage::HeroBodies && !chain.HeroBodies.empty() )
        {
            for ( uint32_t i = 0; i < static_cast<uint32_t>( chain.HeroBodies.size() ); ++i )
            {
                if ( i != 0 )
                    ImGui::SameLine();
                if ( ImGui::RadioButton(
                          ( chain.HeroBodies[i].EntityName + "##hero" + std::to_string( i ) ).c_str(),
                          m_Hero == i ) )
                {
                    m_Hero               = i;
                    m_Requested          = SubjectId{};
                    m_FramesSinceRequest = 0;
                    m_Showed             = SubjectId{};
                }
            }
        }

        ImGui::Separator();

        if ( m_Stage == CloudStage::Layer )
        {
            DrawLayerStage( chain );
            return;
        }

        if ( subject.IsNull() )
        {
            DrawEmptyStage( chain );
            return;
        }

        DrawEmbeddedDocument( chain, subject );
    }

    void CloudsPanel::DrawLayerStage( const CloudChain& chain )
    {
        if ( !chain.HasLayer || !m_Scene )
        {
            ImGui::TextWrapped( "No entity in this scene carries a Volumetric Cloud component, so there is "
                                "no layer to edit. Add one in the Scene Outliner and this stage fills in." );
            return;
        }

        // ── THE SAME CODE DETAILS RUNS, NOT A COPY OF IT ───────────────────────────────────────────────
        //
        // The layer is a COMPONENT, so there is no document to embed — nothing is registered to open one
        // over it, and inventing one would be a second editor for fields Details already draws. The
        // component's own registered entry is looked up and called instead, so the two windows cannot come
        // to disagree about what a cloud layer has in it.
        const ComponentEditorEntry* entry = ComponentWidgetRegistry::Get().Find( kVolumetricCloudComponentEditor );
        if ( !entry || !entry->Draw )
        {
            // Said out loud rather than drawn as an empty pane: a missing registration is a programming
            // error, and a blank stage reads as a layer with no settings.
            ImGui::TextWrapped( "The '%s' component editor is not registered, so this stage cannot draw the "
                                "layer's fields. This is a defect, not an empty layer.",
                                kVolumetricCloudComponentEditor );
            return;
        }

        const auto found = m_Scene->FindEntityByID( chain.LayerEntity );
        if ( !found )
        {
            ImGui::TextWrapped( "The entity that carried this cloud layer is gone from the scene." );
            return;
        }

        ECS::Entity entity = found->get();

        ComponentEditContext ctx;
        ctx.AssetManager = m_Assets;
        ctx.UIHelper     = m_UI.get();
        // The cloud entry offers "Open in Clouds" and "In Clouds" buttons for Details' benefit. From here
        // they would take the user to the window they are standing in — see ComponentEditContext.
        ctx.AllowPanelJumps = false;
        entry->Draw( entity, m_Scene.get(), ctx );
    }

    void CloudsPanel::DrawEmptyStage( const CloudChain& chain )
    {
        // A STAGE WITH NOTHING IN IT OFFERS A WAY IN, not a blank — the Light Mixer's own move. What it
        // offers is deliberately NOT a picker: binding a slot happens in the material's Inputs table, and
        // a second writer of one value is the second execution path the contract forbids.
        switch ( m_Stage )
        {
            case CloudStage::Material:
                ImGui::TextWrapped( "This layer names no material, so the sky is the CloudRaymarch schema's "
                                    "own defaults \xe2\x80\x94 a working sky, not a broken one. Give it one "
                                    "from the layer's Material row in stage 1." );
                if ( ImGui::Button( "Go to 1 \xc2\xb7 Layer" ) )
                    m_Stage = CloudStage::Layer;
                return;

            case CloudStage::Layout:
                ImGui::TextWrapped( "No painted layout. The sky is placed procedurally, and Weather Patch "
                                    "Strength decides which parts of it are busy. Drop a .dclayout into "
                                    "Global Pattern in the material's Inputs table to paint where the "
                                    "clouds are." );
                break;

            case CloudStage::Types:
                ImGui::TextWrapped(
                     "Slot %u is empty. All four empty means the layer uses the engine's built-in cumulus "
                     "congestus \xe2\x80\x94 a sky, not an absence of one.",
                     m_TypeSlot + 1u );
                break;

            case CloudStage::Noise:
                if ( chain.HasLayer && m_TypeSlot < kCloudStageTypeSlots &&
                     !IsNull( chain.CloudTypes[m_TypeSlot] ) )
                {
                    ImGui::TextWrapped( "This cloud type names no noise volume, so its edge is cut from the "
                                        "engine's built-in one. That is a file nobody has to author." );
                }
                else
                {
                    ImGui::TextWrapped( "The noise belongs to a CLOUD TYPE, not to the material. Pick a "
                                        "filled slot in stage 4 and this stage shows the volume it names." );
                }
                break;

            case CloudStage::HeroBodies:
                ImGui::TextWrapped( "No hero clouds in this scene. A hero body is a sculpted cloud placed by "
                                    "hand: add a Hero Cloud component to an entity and give it a .dcmv." );
                break;

            case CloudStage::Layer:
            case CloudStage::Count:
                break;
        }

        if ( m_Stage != CloudStage::Material )
        {
            if ( ImGui::Button( "Go to 2 \xc2\xb7 Material" ) )
                m_Stage = CloudStage::Material;
        }
    }

    void CloudsPanel::DrawEmbeddedDocument( const CloudChain& chain, const SubjectId& subject )
    {
        (void)chain;

        // ── THE SAME OBJECT THE DOCUMENT WELL HAS ──────────────────────────────────────────────────────
        //
        // Asked of the OWNER, never built here. This one line is the whole of why a `.demat` cannot get a
        // second working copy out of this window: there is nothing to build with, and OpenDocuments::Open
        // would refuse a duplicate even if there were.
        ISubjectDocument* document = m_Documents->Find( subject );

        if ( !document )
        {
            // A CLOSE MEANS A CLOSE. If this pane HAD the document and no longer does, somebody closed it —
            // from the well's tab, from the palette, or because its subject went away — and reopening it
            // behind them would make the close a button that does nothing. Measured before it was fixed:
            // Close was accepted, logged, and the document was open again in the same reply.
            if ( m_Showed == subject )
            {
                ImGui::TextWrapped( "You closed this editor. The sky still names it \xe2\x80\x94 stage %u "
                                    "is unchanged; only its window is gone.",
                                    static_cast<uint32_t>( m_Stage ) + 1u );
                if ( ImGui::Button( "Open it again" ) )
                    m_Showed = SubjectId{}; // the request below runs on the next frame
                return;
            }

            // ASKED ONCE, NOT EVERY FRAME. An open is queued and serviced between frames, so a request per
            // frame would be collapsed by the queue — but a REFUSED open (the six renderer slots are spoken
            // for) never resolves, and re-requesting it would re-raise the refusal dialog for ever.
            if ( !( m_Requested == subject ) )
            {
                m_Requested          = subject;
                m_FramesSinceRequest = 0;
                QueueCloudSubjectOpen( Assets::AssetHandle( subject.Owner ),
                                       static_cast<Assets::AssetTypeID>( subject.Facet ) );
            }
            else
            {
                ++m_FramesSinceRequest;
            }

            if ( m_FramesSinceRequest < kFramesBeforeOpenLooksStuck )
            {
                ImGui::TextDisabled( "Opening \xe2\x80\xa6" );
            }
            else
            {
                // "Still opening" and "it was refused" are different facts and a spinner cannot say the
                // second one. The reason is in the log because that is where the refusal was written with
                // its census of what is holding the six slots.
                ImGui::TextWrapped( "This stage's editor did not open. The log says why \xe2\x80\x94 the "
                                    "usual reason is that all six renderer slots are in use, in which case "
                                    "closing a document window frees one." );
                if ( ImGui::Button( "Try again" ) )
                {
                    m_Requested          = SubjectId{};
                    m_FramesSinceRequest = 0;
                }
            }
            return;
        }

        m_Requested          = SubjectId{};
        m_FramesSinceRequest = 0;
        m_Showed             = subject;

        // ── DRAWN HERE AND, IF ITS TAB IS UP, IN THE WELL TOO — IN THE SAME FRAME ─────────────────────
        //
        // Measured before it was relied on; the class comment carries the result and the mechanism. What
        // this line must NOT forget is to report the draw: the renderer-slot release counts frames in which
        // NO view drew a document, and a material shown only here would otherwise have its preview renderer
        // taken away underneath this pane after thirty frames.
        m_Documents->NoteDrawn( subject );

        ImGui::PushID( subject.ToString().c_str() );
        document->OnUIRender();
        ImGui::PopID();
    }
} // namespace Desert::Editor
