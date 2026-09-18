#include "AnimGraphPanel.hpp"

#include <Engine/Assets/AnimGraphAsset.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Editor/Panels/PanelContext.hpp>

#include <Editor/Core/GraphCanvas/GraphCanvasView.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Panels/Animation/AnimGraphCanvasPlan.hpp>

#include <Common/Core/Logger.hpp>

#include <Engine/Animation/AnimationLibrary.hpp>
#include <Engine/Animation/Graph/AnimGraph.hpp>
#include <Engine/Animation/Graph/AnimGraphValidation.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Entity.hpp>

#include <imgui-node-editor/imgui_node_editor.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdint>
#include <memory>

namespace ed = ax::NodeEditor;

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;
    namespace G     = Animation::Graph;

    namespace
    {
        // A live parameter write can now be REFUSED (see Evaluator::SetBool and friends), and a panel that
        // dropped the result would be the silence those refusals exist to remove. It can only happen when
        // the parameter was renamed in the same frame the slider moved, so this is a diagnostic and not a
        // modal — but it is a diagnostic that exists.
        void ReportParamWrite( const Common::BoolResultStr& result )
        {
            if ( !result.IsSuccess() )
            {
                LOG_ERROR( "[AnimGraphPanel] {}", result.GetError() );
            }
        }

        const char* kTypeNames[] = { "Bool", "Int", "Float" };

        // THE COMBO IS AS WIDE AS ITS WIDEST LABEL, AND THAT IS A DERIVATION RATHER THAN A NUMBER.
        // It was 55 px flat: the arrow button alone is a frame height, so the text got roughly 28 px and
        // "Bool" only just fitted while "Float" was cut — reported from the editor, not from a test,
        // because nothing here asserts that a label is legible. A hard number also cannot survive the
        // next entry in kTypeNames: whoever adds "Trigger" would have to remember this line, and the
        // failure would again be silent and visual. Asking ImGui for the widest label cannot drift.
        float TypeComboWidth()
        {
            float widest = 0.0F;
            for ( const char* name : kTypeNames )
            {
                // Plain comparison rather than ImMax: that one lives in imgui_internal.h, which this
                // file does not include, and reaching for it is how IM_PI broke the launcher build.
                const float width = ImGui::CalcTextSize( name ).x;
                if ( width > widest )
                {
                    widest = width;
                }
            }
            // text + both frame paddings + the arrow button, which is square and a frame tall
            return widest + ( ImGui::GetStyle().FramePadding.x * 2.0F ) + ImGui::GetFrameHeight();
        }
        const char* kOpNames[] = { ">", "<", ">=", "<=", "==", "!=", "is true", "is false" };

        /// One float, drawn as whatever the graph DECLARED it to be. Used by the `def` control; the
        /// `live` one below cannot share it, because each of its three arms calls a different, refusable
        /// setter on the evaluator rather than writing a float.
        ///
        /// The store is a float for both — that is the evaluator's uniform store and not a claim about
        /// the type — so a Bool default is 0/1 and an Int default is a whole number, exactly as
        /// `Parameter::Default` documents.
        bool DrawTypedValue( const char* id, Animation::Graph::ParamType type, float& value )
        {
            if ( type == Animation::Graph::ParamType::Bool )
            {
                bool flag = value != 0.0f;
                if ( !ImGui::Checkbox( id, &flag ) )
                {
                    return false;
                }
                value = flag ? 1.0f : 0.0f;
                return true;
            }
            if ( type == Animation::Graph::ParamType::Int )
            {
                auto whole = static_cast<int>( std::lround( value ) );
                if ( !ImGui::DragInt( id, &whole, 1.0f ) )
                {
                    return false;
                }
                value = static_cast<float>( whole );
                return true;
            }
            return ImGui::DragFloat( id, &value, 0.05f );
        }
    } // namespace

    AnimGraphPanel::AnimGraphPanel( const SubjectId& subject, const std::string& displayName,
                                    const std::shared_ptr<::Desert::Core::Scene>& scene,
                                    const Animation::AnimationLibrary*            library,
                                    Assets::AssetManager*                         assetManager )
         : ISubjectDocument( displayName, subject ), m_Scene( scene ), m_Library( library ),
           m_AssetManager( assetManager )
    {
        ed::Config config;
        config.SettingsFile = nullptr; // node positions live in the graph (State.X/Y), not a stray json
        m_Context           = ed::CreateEditor( &config );
    }

    AnimGraphPanel::~AnimGraphPanel()
    {
        if ( m_Context )
            ed::DestroyEditor( m_Context );
    }

    // THE STATIC RequestOpen INBOX IS GONE, and its absence is half the point of U7. It was a file-static
    // bool that meant "reveal the one Anim Graph window", which is all a singleton can be asked. A document
    // is asked for BY SUBJECT — Core::SubjectOpenRequests::Request( AnimGraphPanel::SubjectFor( entity ) ) —
    // and the Details button beside the Animation component sends exactly that.

    ECS::AnimationComponent* AnimGraphPanel::ResolveComponent() const
    {
        const auto scene = m_Scene.lock();
        if ( !scene )
            return nullptr; // the scene this document was opened over has been closed

        const auto entOpt = scene->FindEntityByID( Subject().Owner );
        if ( !entOpt )
            return nullptr; // the entity was deleted

        auto& entity = entOpt->get();
        if ( !entity.HasComponent<ECS::AnimationComponent>() )
            return nullptr; // the component was removed from under the window

        return &entity.GetComponent<ECS::AnimationComponent>();
    }

    Assets::Asset<Assets::AnimGraphAsset> AnimGraphPanel::ResolveAsset() const
    {
        const ECS::AnimationComponent* anim = ResolveComponent();
        if ( !anim || !anim->GraphAsset || m_AssetManager == nullptr )
            return nullptr;
        return m_AssetManager->FindByHandle<Assets::AnimGraphAsset>( anim->GraphAsset );
    }

    void AnimGraphPanel::MarkEdited()
    {
        if ( const auto asset = ResolveAsset() )
        {
            asset->MarkEdited();
            return;
        }
        // NOT SILENT. An edit that reaches no asset is an edit no evaluator will ever be told about: the
        // canvas would show the new shape while every character on this graph went on playing the old one,
        // which is the exact class of defect — a comment or a control promising something the tree does
        // not do — that this project keeps paying for.
        m_Status        = "this graph has no asset behind it; the edit will not reach the running character";
        m_StatusIsError = true;
    }

    void AnimGraphPanel::SaveGraph()
    {
        const auto asset = ResolveAsset();
        if ( !asset )
        {
            m_Status        = "nothing to save: this window's entity names no loaded anim graph";
            m_StatusIsError = true;
            return;
        }

        const auto graph = asset->GetGraph();
        if ( !graph )
        {
            m_Status        = "nothing to save: the asset holds no graph";
            m_StatusIsError = true;
            return;
        }

        // THE SUBJECT'S OWN FILE, on the shader graph's terms: a path composed from the graph's Name would
        // be a silent Save As the first time somebody renamed one.
        if ( const auto written = Assets::AnimGraphAsset::Save( asset->GetMetadata().Filepath, *graph ); !written )
        {
            m_Status        = "NOT saved: " + written.GetError();
            m_StatusIsError = true;
            return;
        }
        m_Status        = "Saved " + asset->GetMetadata().Filepath.filename().string();
        m_StatusIsError = false;
        // What is on disk is now what is in memory. The dot goes out here and nowhere else.
        m_SavedRevision = asset->GetRevision();
    }

    void AnimGraphPanel::AddState()
    {
        ECS::AnimationComponent* anim = ResolveComponent();
        if ( anim == nullptr || !anim->Graph )
        {
            m_Status        = "no graph to add a state to";
            m_StatusIsError = true;
            return;
        }

        G::State ns;
        // UNIQUE BY CONSTRUCTION. "State_" + size() collides the moment a state is deleted and another
        // added, and two states sharing a name is not cosmetic: `Entry`, `Transition::To` and
        // `Evaluator::FindState` all resolve by string and all take the FIRST match, so the second one is
        // unreachable and plays the first one's clip with nothing said.
        ns.Name = Graph::MakeUniqueStateName( *anim->Graph,
                                              "State_" + std::to_string( anim->Graph->States.size() ), -1 );
        // AND NOT (0, 0), which is where every new state used to land: the second one covered the first
        // exactly, and a node under another node cannot be clicked, renamed, given a clip or deleted. The
        // rule is in `AnimGraphCanvasPlan` because that unit has no ImGui in it and can therefore be
        // measured; `AnimGraphValidation` compiles it and asserts the separation.
        const Graph::StatePosition where = Graph::NextStatePosition( *anim->Graph );
        ns.X                             = where.X;
        ns.Y                             = where.Y;
        anim->Graph->States.push_back( ns );
        MarkEdited();
    }

    void AnimGraphPanel::AddParameter()
    {
        ECS::AnimationComponent* anim = ResolveComponent();
        if ( anim == nullptr || !anim->Graph )
        {
            m_Status        = "no graph to add a parameter to";
            m_StatusIsError = true;
            return;
        }

        // UNIQUE BY CONSTRUCTION, for the reason `+ State` is: this pushed the literal "Param" every
        // time, so two presses left two parameters that no condition can tell apart — the second one's
        // declared type is never consulted, because `Evaluator::FindParameter` takes the first match,
        // while its default still overwrites the first one's in `Evaluator::Reset`.
        anim->Graph->Parameters.push_back( { Graph::MakeUniqueParameterName( *anim->Graph, "Param", -1 ),
                                             static_cast<int>( G::ParamType::Float ), 0.0f } );
        MarkEdited();
    }

    std::vector<std::string> AnimGraphPanel::ResolveClipNames( const ECS::AnimationComponent& anim ) const
    {
        std::vector<std::string> names;
        if ( anim.Animator == nullptr || m_Library == nullptr )
        {
            return names; // no skeleton to ask about yet; NOT the same fact as "this skeleton has none"
        }
        // The SAME rule AnimationECSSystem resolves the chosen name with.
        for ( const auto& asset : m_Library->GetForSkeleton( anim.Animator->GetSkeleton() ) )
        {
            names.push_back( asset->GetClip().AnimationName );
        }
        return names;
    }

    std::vector<ISubjectDocument::DocumentAction> AnimGraphPanel::Actions()
    {
        // The SAME function the toolbar button calls. A second code path here would be a second behaviour
        // to keep in step, and the point of the entry is that what a client drives is what a person
        // presses.
        //  is here for the reason  is: this machine refuses synthetic input, so a view
        // control that exists only as a toolbar button is a view control no test and no script can reach.
        std::vector<DocumentAction> actions{
             { "Save", [this] { SaveGraph(); } },
             { "Frame All", [this] { Graph::FrameAll( m_Context ); } },
             { "Frame Selection", [this] { Graph::FrameSelection( m_Context ); } },
             // AND `+ State`, FOR THE SAME REASON `Save` IS HERE. It is the one authoring action of this
             // window that creates something, and a toolbar button is unreachable to every client and
             // every check on this machine -- which is exactly why "a new state lands on top of its
             // neighbour" survived: nothing but a person with a mouse could produce one.
             { "Add State", [this] { AddState(); } },
             // AND `+ Parameter`, on the same terms. It is the other authoring action of this window
             // that creates something, and until it was one there was no way for a check or a client to
             // press it — which is why "two presses make two parameters nothing can tell apart" had
             // survived as long as the state one had.
             { "Add Parameter", [this] { AddParameter(); } },
        };

        // ── ONE ENTRY PER FINDING, AND IT IS THE SAME CALL THE STRIP'S CLICK MAKES ────────────────────
        //
        // `Save`, `+ State` and `+ Parameter` are here because a toolbar button is unreachable to every
        // client and every check on this machine — synthetic input is closed — and a control nothing can
        // drive is a control nothing can photograph. A line in the ⚠ strip is exactly such a control, and
        // it is also the one that is HARDEST to reach by hand: the reader has to find the line first.
        //
        // The label carries the finding's own sentence rather than an ordinal, because the ordinal moves
        // the moment the graph does, and the palette matches labels exactly. Findings are recomputed here
        // rather than cached from the last frame: a document action can be run while the window is docked
        // behind another one and has not drawn this frame.
        ECS::AnimationComponent* anim = ResolveComponent();
        if ( anim == nullptr || !anim->Graph )
        {
            return actions;
        }

        const std::vector<std::string> clipNames = ResolveClipNames( *anim );
        const G::ClipSet               clips{ anim->Animator != nullptr && m_Library != nullptr, clipNames };
        for ( const auto& warning : G::Validate( *anim->Graph, clips ) )
        {
            // NOT A LAMBDA, and that is a finding rather than a style: `bugprone-exception-escape` fires
            // on a parameter-less lambda in this tree, and `DocumentAction::Run` takes no parameters, so
            // there is no lambda here the check accepts. `EditorLayer::RunDocumentAction` records the
            // same one at the other end of this very wire. `bind_front` binds the member directly.
            actions.push_back(
                 { "Reveal: " + warning.Text, std::bind_front( &AnimGraphPanel::RevealFinding, this, warning ) } );
        }

        return actions;
    }

    void AnimGraphPanel::OnUIRender()
    {
        // NO "SELECT AN ENTITY" EMPTY STATE any more: this window is about one entity for its whole life.
        // A null here is a subject that has just died, and the editor closes the document for it on the
        // same frame (EditorLayer::CloseDocumentsWhoseSubjectIsGone) — so the message says what happened
        // rather than asking the user to fix it.
        ECS::AnimationComponent* anim = ResolveComponent();
        if ( !anim )
        {
            ImGui::TextDisabled( "This entity, its Animation component or its scene is gone — closing." );
            return;
        }

        if ( !anim->Graph )
        {
            // NO "Create AnimGraph" BUTTON HERE ANY MORE, and its absence is the point. A graph is a FILE
            // now, and creating one means writing it, registering it and pointing this entity's slot at
            // it — three steps that can each fail and that belong where the SLOT is, in Details. A second
            // creator here would be a second way to make a graph and the two would drift; what stood here
            // could only ever make an unsaved one, which is precisely the storage §5.1 removed.
            ImGui::TextWrapped( "This entity names no anim graph, or the file it names is not loaded. "
                                "Pick or create one in Details > Animation > AnimGraph." );
            return;
        }

        // Clip names available for this skeleton (for the clip picker) — from the (lazily built) Animator.
        const std::vector<std::string> clipNames = ResolveClipNames( *anim );

        // THE ● OF THE §8.2 HEADER. The first frame records what is on disk rather than claiming the
        // document is already dirty; every bump of the asset's revision after that is an edit nobody has
        // written yet.
        const auto asset = ResolveAsset();
        if ( asset && !m_SavedRevision )
        {
            m_SavedRevision = asset->GetRevision();
        }
        const bool unsaved = asset && m_SavedRevision && asset->GetRevision() != *m_SavedRevision;

        // Toolbar.
        if ( ImGui::Button( ICON_MDI_CONTENT_SAVE "  Save" ) )
            SaveGraph();
        Utils::ImGuiUtilities::Tooltip( "Write this graph back to its .danimgraph" );
        if ( unsaved )
        {
            // NOT A COSMETIC DOT. Dragging a state is an edit to the FILE (§7.2: the drag authors
            // State.X/Y and they travel to the graph file), and until this appeared the only signal that
            // a layout would be lost on close was that it was lost. There is no close prompt on these
            // documents; this is what stands in for one, and its absence is what made "dragged it,
            // closed it, lost it" silent.
            ImGui::SameLine();
            ImGui::TextColored( ImVec4( 1.0f, 0.78f, 0.25f, 1.0f ), ICON_MDI_CIRCLE_MEDIUM );
            Utils::ImGuiUtilities::Tooltip( "This graph has edits that are not on disk yet" );
        }
        ImGui::SameLine();
        if ( ImGui::Button( "+ State" ) )
        {
            AddState();
        }
        ImGui::SameLine();
        Graph::DrawViewButtons( m_Context );
        if ( const auto* cur = anim->GraphEvaluator ? anim->GraphEvaluator->CurrentState() : nullptr )
        {
            ImGui::SameLine();

            // ЖИВОЙ ПЕРЕХОД, а не только его исход — строка макета §8.2. Пока кроссфейд идёт, состояние
            // на экране («Run») не описывает того, что видит аниматор: поза в этот момент есть смесь
            // двух, и без второго имени и доли заголовок утверждает больше, чем знает.
            //
            // Три источника, и каждый знает ровно свою часть: откуда пришли — вычислитель графа (он
            // запоминает это там, где переход срабатывает); куда — он же; НАСКОЛЬКО — только Animator,
            // потому что длительность объявлена в графе, а прогресс ведут часы проигрывания.
            const float alpha = anim->Animator ? anim->Animator->BlendAlpha() : 0.0F;
            const auto* prev  = anim->GraphEvaluator->PreviousState();
            if ( alpha > 0.0F && prev != nullptr )
            {
                ImGui::TextDisabled( "| Live: %s " ICON_MDI_ARROW_RIGHT " %s  %.0f%%", prev->Name.c_str(),
                                     cur->Name.c_str(), alpha * 100.0F );
            }
            else
            {
                ImGui::TextDisabled( "| Active: %s", cur->Name.c_str() );
            }
        }

        // The window's ONE error channel — a save that failed, or an edit that reached no asset. It has to
        // be on screen, because the alternative is a log line nobody authoring a graph is reading.
        Graph::DrawStatusLine( m_Status, m_StatusIsError );

        // THE CANVAS IS NOT WRAPPED IN A CHILD WINDOW, and that is the fix to "a docked Anim Graph drew an
        // empty rectangle". See `Graph::DeferredFrameAll` for the measurement: a child sized exactly to
        // the canvas gives `imgui-node-editor`'s lazy first-frame init nowhere to put its throwaway dummy
        // widget, and the canvas is refused for that frame. The shader graph never had a child here and
        // survived by four pixels. It gets an explicit WIDTH instead, which is the only thing the child
        // was really doing.
        constexpr float kSideW  = 300.0f;
        const float     canvasW = std::max( 160.0f, ImGui::GetContentRegionAvail().x - kSideW );

        // WHAT IS WRONG WITH THIS GRAPH, DECIDED BY A UNIT WITH NO IMGUI IN IT. The clip list is handed
        // over as "Known" only when there was an Animator to ask: an entity whose skeleton has not been
        // resolved yet would otherwise have every one of its states reported as naming a missing clip,
        // for the frames before it is true.
        const G::ClipSet                   clips{ anim->Animator != nullptr && m_Library != nullptr, clipNames };
        const std::vector<G::GraphWarning> warnings = G::Validate( *anim->Graph, clips );

        const float stripH  = WarningStripHeight( warnings.size() );
        const float canvasH = std::max( 80.0f, ImGui::GetContentRegionAvail().y - stripH );
        DrawCanvas( *anim, canvasW, canvasH );

        ImGui::SameLine();
        ImGui::BeginGroup();
        // THE SAME HEIGHT THE CANVAS GOT, and not `0` meaning "the rest of the window". A height-0 child
        // here reaches the bottom of the document, so it swallowed the space reserved for the warning
        // strip and the strip was laid out BELOW the visible area: computed every frame, drawn nowhere.
        // Found in the editor, on the frame that was supposed to photograph the strip -- which is the
        // whole argument for taking the frame.
        DrawSidePanel( *anim, clipNames, canvasH );
        ImGui::EndGroup();

        DrawWarningStrip( *anim->Graph, warnings );
    }

    float AnimGraphPanel::WarningStripHeight( size_t count )
    {
        if ( count == 0 )
        {
            return 0.0f;
        }
        // Every finding is reachable — the strip SCROLLS rather than truncating. A strip that showed
        // "and 7 more" would be a control that hides a defect, which is the one thing a validator may
        // not do; a strip that grew to thirty lines would eat the canvas it is about.
        constexpr size_t kMaxVisibleLines = 3;
        const auto       lines            = static_cast<float>( std::min( count, kMaxVisibleLines ) );
        return ImGui::GetTextLineHeightWithSpacing() * lines + ImGui::GetStyle().ItemSpacing.y * 2.0f;
    }

    void AnimGraphPanel::DrawWarningStrip( const G::AnimGraph&                 graph,
                                           const std::vector<G::GraphWarning>& warnings )
    {
        if ( warnings.empty() )
        {
            return; // a graph with nothing wrong with it gets no strip at all, not an empty one
        }

        ImGui::Separator();
        ImGui::BeginChild( "##agWarnings", ImVec2( 0.0f, WarningStripHeight( warnings.size() ) ), false );
        ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 1.0f, 0.78f, 0.25f, 1.0f ) );
        for ( int i = 0; i < static_cast<int>( warnings.size() ); ++i )
        {
            const auto& warning = warnings[static_cast<size_t>( i )];
            ImGui::PushID( i );

            // ── THE LINE IS A CONTROL, AND UNTIL IT WAS, HALF OF EVERY FINDING WAS UNREADABLE ──────────
            //
            // `GraphWarning` has carried `State` and `Transition` since the validator landed and NOTHING
            // read them: the strip drew `Text` and stopped. So a reader told that «Run» names no clip
            // still had to find `Run` by eye on a canvas the whole of §8 exists to make readable at
            // thirty states -- which is the same as not being told, and it is the shape no frame can
            // show is missing, because what is missing is a reader for a field.
            //
            // Clicking selects the element on the canvas and moves the view to it, which ALSO puts it in
            // the side panel's inspector on the same frame: the state's clip picker, or the transition's
            // conditions. The distance from "what is wrong" to "the control that fixes it" is one click
            // rather than a search.
            // THE LINE IS COMPOSED ONCE AND MEASURED AS WHAT IS DRAWN. Measuring `warning.Text` while
            // drawing the icon plus two spaces in front of it is how a hit box ends up one line shorter
            // than its sentence: the icon is what pushes a borderline sentence onto a second line, and
            // the bottom line would then not be clickable while looking exactly as if it were.
            const std::string line     = std::string( ICON_MDI_ALERT "  " ) + warning.Text;
            const float       avail    = ImGui::GetContentRegionAvail().x;
            const ImVec2      textSize = ImGui::CalcTextSize( line.c_str(), nullptr, false, avail );
            const ImVec2      before   = ImGui::GetCursorPos();

            // SELECTABLE UNDER THE TEXT RATHER THAN AROUND IT, because `Selectable` does not wrap and
            // `TextWrapped` is not a control. Un-wrapped the sentence ran off the document's right edge
            // and the half that names the clip or the parameter -- the only actionable half -- was cut,
            // which is a warning that reports a problem without saying which one. Measured from the
            // editor: the first frame ever taken of this strip showed exactly that. So the hit box is
            // sized to the WRAPPED text and the text is drawn back over it.
            const bool clicked =
                 ImGui::Selectable( "##agw", false, ImGuiSelectableFlags_None, ImVec2( 0.0f, textSize.y ) );
            Utils::ImGuiUtilities::Tooltip( "Select this on the canvas" );

            const ImVec2 after = ImGui::GetCursorPos();
            ImGui::SetCursorPos( before );
            ImGui::TextWrapped( "%s", line.c_str() );
            // Back to where the Selectable left it, so a sentence that wrapped to two lines and a hit box
            // that was sized for two lines cannot advance the cursor by different amounts.
            ImGui::SetCursorPos( after );

            if ( clicked )
            {
                RevealWarning( graph, warning );
            }
            ImGui::PopID();
        }
        ImGui::PopStyleColor();
        ImGui::EndChild();
    }

    void AnimGraphPanel::RevealFinding( const G::GraphWarning& warning )
    {
        // RE-RESOLVED, not captured: the component can be gone by the time an entry built for the palette
        // is run, and a graph captured by reference would then be a dangling one.
        ECS::AnimationComponent* now = ResolveComponent();
        if ( now != nullptr && now->Graph )
        {
            RevealWarning( *now->Graph, warning );
        }
    }

    void AnimGraphPanel::RevealWarning( const G::AnimGraph& graph, const G::GraphWarning& warning )
    {
        // WHICH ELEMENT, ASKED OF THE PLAN — the same seam every other "which thing is this" question in
        // this window goes through, and for the same reason: the nth transition of a state is not the nth
        // link out of it, because a transition to a name no state carries is drawn as no link at all.
        const Graph::WarningTarget target = Graph::WarningTargetOf( m_Canvas, graph, warning );
        if ( !target.Valid() )
        {
            return;
        }

        ed::SetCurrentEditor( m_Context );
        ed::ClearSelection();
        if ( target.Link != Graph::ElementId::Invalid )
        {
            ed::SelectLink( ed::LinkId( Graph::Raw( target.Link ) ) );
        }
        else
        {
            ed::SelectNode( ed::NodeId( Graph::Raw( target.Node ) ) );
        }
        ed::SetCurrentEditor( nullptr );

        // The SAME navigation the `Frame Sel` button runs, and not a second spelling of it: two ways to
        // move this view would be two behaviours to keep in step, and the button's own rule (an empty
        // selection frames everything) is the right one here too -- a selection this function failed to
        // make must not leave the reader staring at an empty rectangle.
        Graph::FrameSelection( m_Context );
    }

    void AnimGraphPanel::DrawCanvas( ECS::AnimationComponent& anim, float width, float height )
    {
        auto& graph = *anim.Graph;
        bool  dirty = false;

        // EVERY ID ON THIS CANVAS, AND WHAT IT NAMES — decided before a single ImGui call, by a unit with
        // no ImGui in it. `NodeId( i ) = i + 1` used to live here.
        m_Canvas = Graph::PlanAnimGraph( graph, m_Ids );

        // The size the canvas is actually drawn at, which is also what `DeferredFrameAll` waits to see
        // stop changing. Height 0 means "the rest of the window" to the node editor, so it is resolved
        // here rather than guessed at.
        const ImVec2 canvasSize( width, height );

        ed::SetCurrentEditor( m_Context );
        ed::Begin( "##animGraph", canvasSize );

        int activeIndex = -1;
        if ( anim.GraphEvaluator && anim.GraphEvaluator->CurrentState() )
        {
            const std::string& activeName = anim.GraphEvaluator->CurrentState()->Name;
            for ( int i = 0; i < static_cast<int>( graph.States.size() ); ++i )
                if ( graph.States[i].Name == activeName )
                {
                    activeIndex = i;
                    break;
                }
        }

        // --- State nodes ---
        for ( int i = 0; i < static_cast<int>( graph.States.size() ); ++i )
        {
            auto&                     s       = graph.States[i];
            const Graph::PlannedNode& planned = m_Canvas.Plan.Nodes[static_cast<size_t>( i )];

            Graph::PushNodePosition( planned );

            ed::BeginNode( ed::NodeId( Graph::Raw( planned.Id ) ) );

            const bool   isEntry  = ( graph.Entry == s.Name );
            const bool   isActive = ( i == activeIndex );
            const ImVec4 titleCol = isActive  ? ImVec4( 1.0f, 0.65f, 0.2f, 1.0f )
                                    : isEntry ? ImVec4( 0.4f, 0.85f, 1.0f, 1.0f )
                                              : ImVec4( 0.9f, 0.9f, 0.95f, 1.0f );
            ImGui::TextColored( titleCol, "%s%s", s.Name.c_str(), isEntry ? "  (entry)" : "" );
            ImGui::TextDisabled( "%s", s.Clip.empty() ? "<no clip>" : s.Clip.c_str() );

            ImGui::BeginGroup();
            ed::BeginPin( ed::PinId( Graph::Raw( m_Canvas.StateInPins[static_cast<size_t>( i )] ) ),
                          ed::PinKind::Input );
            ImGui::TextUnformatted( "-> in" );
            ed::EndPin();
            ImGui::EndGroup();

            ImGui::SameLine( 0.0f, 30.0f );

            ImGui::BeginGroup();
            ed::BeginPin( ed::PinId( Graph::Raw( m_Canvas.StateOutPins[static_cast<size_t>( i )] ) ),
                          ed::PinKind::Output );
            ImGui::TextUnformatted( "out ->" );
            ed::EndPin();
            ImGui::EndGroup();

            ed::EndNode();

            // Persist user drags back into the model, by ELEMENT and not by index. The canvas used to be
            // asked for `NodeId( i )` after a deletion had shifted every later state down one, so it
            // handed back the neighbour's position and this line wrote it into the wrong state.
            //
            // A DRAG IS AN EDIT TO THE FILE, and the `(void)` that used to stand here said otherwise.
            // `State.X/Y` are serialized into the .danimgraph (§7.2 names the drag as authoring them),
            // so a layout the user arranged and never saved is a layout that dies with the window, with
            // nothing on screen to say so. The fear behind the `(void)` — "merely looking at a graph
            // would ask to be saved" — is answered by the return value rather than by dropping it:
            // `PullNodePosition` is false unless the position actually MOVED, so an untouched graph
            // never dirties. Measured in the editor, not assumed: see the report's frame.
            dirty |= Graph::PullNodePosition( planned, s.X, s.Y );
        }

        // --- Transition links ---
        for ( const auto& link : m_Canvas.Plan.Links )
        {
            ed::Link( ed::LinkId( Graph::Raw( link.Id ) ), ed::PinId( link.FromPin ), ed::PinId( link.ToPin ),
                      ImVec4( 0.6f, 0.8f, 0.6f, 1.0f ), 2.0f );
        }

        // --- Create transitions by dragging out -> in ---
        if ( ed::BeginCreate() )
        {
            ed::PinId a, b;
            if ( ed::QueryNewLink( &a, &b ) && a && b )
            {
                const uint64_t pa  = a.Get();
                const uint64_t pb  = b.Get();
                int            src = Graph::StateOfOutPin( m_Canvas, pa );
                int            dst = Graph::StateOfInPin( m_Canvas, pb );
                if ( src < 0 && dst < 0 ) // dragged the other direction
                {
                    src = Graph::StateOfOutPin( m_Canvas, pb );
                    dst = Graph::StateOfInPin( m_Canvas, pa );
                }

                const bool valid = src >= 0 && dst >= 0 && src != dst;
                const bool dup =
                     valid &&
                     std::any_of( graph.States[src].Transitions.begin(), graph.States[src].Transitions.end(),
                                  [&]( const G::Transition& tr ) { return tr.To == graph.States[dst].Name; } );
                if ( !valid || dup )
                    ed::RejectNewItem( ImVec4( 1.0f, 0.4f, 0.4f, 1.0f ), 2.0f );
                else if ( ed::AcceptNewItem( ImVec4( 0.5f, 1.0f, 0.5f, 1.0f ), 3.0f ) )
                {
                    G::Transition tr;
                    tr.To = graph.States[dst].Name;
                    graph.States[src].Transitions.push_back( tr );
                    dirty = true;
                }
            }
        }
        ed::EndCreate();

        // --- Deletion ---
        if ( ed::BeginDelete() )
        {
            ed::LinkId dl;
            while ( ed::QueryDeletedLink( &dl ) )
            {
                if ( ed::AcceptDeletedItem() )
                {
                    // WHICH TRANSITION, ASKED OF THE PLAN. `( id - kLink ) / 4096` was an index pair
                    // decoded out of an id, so deleting one transition renumbered the rest and the next
                    // deletion took a different one than the user had selected.
                    const Graph::TransitionRef ref =
                         Graph::TransitionOfLink( m_Canvas, static_cast<Graph::ElementId>( dl.Get() ) );
                    if ( ref.Valid() )
                    {
                        auto& transitions = graph.States[ref.State].Transitions;
                        transitions.erase( transitions.begin() + ref.Index );
                        dirty = true;
                    }
                }
            }
            ed::NodeId dn;
            while ( ed::QueryDeletedNode( &dn ) )
            {
                if ( ed::AcceptDeletedItem() )
                {
                    const int ni = Graph::StateOfNode( m_Canvas, static_cast<Graph::ElementId>( dn.Get() ) );
                    if ( ni >= 0 )
                    {
                        const std::string gone = graph.States[ni].Name;
                        graph.States.erase( graph.States.begin() + ni );
                        for ( auto& st : graph.States )
                            std::erase_if( st.Transitions,
                                           [&]( const G::Transition& tr ) { return tr.To == gone; } );
                        if ( graph.Entry == gone )
                            graph.Entry = graph.States.empty() ? "" : graph.States.front().Name;
                        dirty = true;
                    }
                }
            }
        }
        ed::EndDelete();

        ed::End();
        ed::SetCurrentEditor( nullptr );

        if ( m_FrameAll.Tick( canvasSize.x, canvasSize.y ) )
            Graph::FrameAll( m_Context );

        if ( dirty )
            MarkEdited();
    }

    void AnimGraphPanel::DrawSidePanel( ECS::AnimationComponent& anim, const std::vector<std::string>& clipNames,
                                        float height )
    {
        auto& graph = *anim.Graph;
        auto* eval  = anim.GraphEvaluator.get();
        bool  dirty = false;

        ImGui::BeginChild( "##agSide", ImVec2( 290.0f, height ), true );

        // ---- Parameters (with live value controls) ----
        ImGui::TextUnformatted( "Parameters" );
        for ( int i = 0; i < static_cast<int>( graph.Parameters.size() ); ++i )
        {
            auto&      p            = graph.Parameters[i];
            const auto declaredType = static_cast<G::ParamType>( p.Type );
            ImGui::PushID( i );
            ImGui::SetNextItemWidth( 90 );
            // InputText, NOT Property. `Property` is the two-COLUMN row helper: it prints the name with
            // TextUnformatted and calls NextColumn twice, so "##pn" — ImGui's "hide this label" spelling —
            // was drawn on screen as the literal text `##pn`, the NextColumn calls ran outside any columns
            // block, and its own PushItemWidth( -1 ) overrode the SetNextItemWidth( 90 ) one line above.
            // Three symptoms, one misuse. `InputText` takes the id AS the ImGui label, so `##` hides it the
            // way it is meant to. Reported from the editor by the owner; nothing here asserts that a row is
            // legible, which is why it survived.
            // EDITED THROUGH A COPY, AND COMMITTED BY `RenameParameter`. Writing straight into `p.Name`
            // is what made 07 §17.4: the name moved and every `Condition` naming it stayed behind, reading
            // 0.0 through the deliberately tolerant `Evaluator::GetFloat` and comparing against that — so
            // `Speed > 3` went permanently false and `Speed < 3` permanently TRUE, with nothing logged
            // either way. The state rename thirty lines below has carried its new name into `Entry` and
            // every `Transition::To` all along; this is the same job on the other half of the graph, and
            // it lives in `AnimGraphCanvasPlan` so a suite with no ImGui in it can mutate it.
            std::string typed = p.Name;
            if ( Utils::ImGuiUtilities::InputText( typed, "##pn" ) )
            {
                Graph::RenameParameter( graph, i, typed );
                dirty = true;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth( TypeComboWidth() );
            dirty |= ImGui::Combo( "##pt", &p.Type, kTypeNames, IM_ARRAYSIZE( kTypeNames ) );
            ImGui::SameLine();
            if ( ImGui::SmallButton( "x" ) )
            {
                graph.Parameters.erase( graph.Parameters.begin() + i );
                dirty = true;
                ImGui::PopID();
                break;
            }

            // ── def AND live, ON THE ROW BELOW, AND THEY ARE NOT THE SAME KIND OF THING ───────────────
            //
            // `def` is AUTHORED: it is `Parameter::Default`, it travels into the .danimgraph, and
            // `Evaluator::Reset` / `SyncGraph` seed the live value from it. `live` is the value in THIS
            // session's evaluator and is written to no file.
            //
            // THE `def` CONTROL IS NEW, AND ITS ABSENCE WAS NOT COSMETIC (07 §3.1, §17.3). The field has
            // existed in the model and in the file format all along with nothing anywhere able to set
            // it, so every parameter of every shipped graph started at 0. For a `Speed > 3` condition
            // that means a built game's state machine cannot leave its entry state by any route except
            // exit-time — the one row in §8.3 marked "currently unreachable".
            ImGui::TextDisabled( "def" );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( 70 );
            dirty |= DrawTypedValue( "##pd", declaredType, p.Default );

            ImGui::SameLine();
            ImGui::TextDisabled( "live" );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( 70 );
            float live = eval ? eval->GetFloat( p.Name ) : p.Default;

            // THE CONTROL MATCHES THE DECLARED TYPE, all three of them. An `Int` parameter was drawn
            // as a float drag and pushed through SetFloat, which the evaluator accepted because its
            // setters did no checking at all; now it would be refused, and the honest fix is the control
            // the type always deserved. The live value is still stored as one float — that is the
            // evaluator's uniform store, not a type.
            if ( declaredType == G::ParamType::Bool )
            {
                bool b = live != 0.0f;
                if ( ImGui::Checkbox( "##pv", &b ) && eval != nullptr )
                {
                    ReportParamWrite( eval->SetBool( p.Name, b ) );
                }
            }
            else if ( declaredType == G::ParamType::Int )
            {
                auto whole = static_cast<int>( std::lround( live ) );
                if ( ImGui::DragInt( "##pv", &whole, 1.0f ) && eval != nullptr )
                {
                    ReportParamWrite( eval->SetInt( p.Name, whole ) );
                }
            }
            else if ( ImGui::DragFloat( "##pv", &live, 0.05f ) && eval != nullptr )
            {
                ReportParamWrite( eval->SetFloat( p.Name, live ) );
            }
            ImGui::PopID();
        }
        if ( ImGui::SmallButton( "+ Parameter" ) )
            AddParameter();

        ImGui::Separator();

        // ---- Selected node / link editor ----
        ed::SetCurrentEditor( m_Context );
        ed::NodeId selNode;
        ed::LinkId selLink;
        const bool haveNode = ed::GetSelectedNodes( &selNode, 1 ) > 0;
        const bool haveLink = ed::GetSelectedLinks( &selLink, 1 ) > 0;
        ed::SetCurrentEditor( nullptr );

        if ( haveNode )
        {
            // WHICH STATE, ASKED OF THE PLAN. `selNode.Get() - 1` decoded an index out of an id, so after
            // a deletion the inspector edited the state next to the one that was highlighted.
            const int si = Graph::StateOfNode( m_Canvas, static_cast<Graph::ElementId>( selNode.Get() ) );
            if ( si >= 0 )
            {
                auto& s = graph.States[si];
                ImGui::TextUnformatted( "State" );
                const std::string oldName = s.Name;
                if ( Utils::ImGuiUtilities::Property( "Name", s.Name ) )
                {
                    // A RENAME THAT COLLIDES IS A STATE THAT DISAPPEARS: every reference in this graph
                    // resolves by name and takes the first match. Renaming to an occupied name is
                    // therefore answered with a free one rather than accepted silently.
                    s.Name = Graph::MakeUniqueStateName( graph, s.Name, si );
                    if ( graph.Entry == oldName )
                        graph.Entry = s.Name;
                    for ( auto& st : graph.States )
                        for ( auto& tr : st.Transitions )
                            if ( tr.To == oldName )
                                tr.To = s.Name;
                    dirty = true;
                }
                const char* preview = s.Clip.empty() ? "Select Clip" : s.Clip.c_str();
                if ( ImGui::BeginCombo( "Clip", preview ) )
                {
                    for ( const auto& name : clipNames )
                        if ( ImGui::Selectable( name.c_str(), s.Clip == name ) )
                        {
                            s.Clip = name;
                            dirty  = true;
                        }
                    ImGui::EndCombo();
                }
                dirty |= ImGui::Checkbox( "Loop", &s.Loop );
                ImGui::SameLine();
                ImGui::SetNextItemWidth( 80 );
                dirty |= ImGui::DragFloat( "Speed", &s.Speed, 0.01f, 0.0f, 5.0f );
                if ( graph.Entry != s.Name && ImGui::SmallButton( "Set as Entry" ) )
                {
                    graph.Entry = s.Name;
                    dirty       = true;
                }
            }
        }
        else if ( haveLink )
        {
            // Same question, same answer: the plan knows which transition this link is.
            const Graph::TransitionRef ref =
                 Graph::TransitionOfLink( m_Canvas, static_cast<Graph::ElementId>( selLink.Get() ) );
            if ( ref.Valid() )
            {
                const int si = ref.State;
                auto&     tr = graph.States[si].Transitions[ref.Index];
                ImGui::Text( "Transition %s -> %s", graph.States[si].Name.c_str(), tr.To.c_str() );
                ImGui::SetNextItemWidth( 90 );
                dirty |= ImGui::DragFloat( "Blend", &tr.Blend, 0.01f, 0.0f, 2.0f );
                dirty |= ImGui::Checkbox( "Exit time", &tr.HasExitTime );
                if ( tr.HasExitTime )
                {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth( 80 );
                    dirty |= ImGui::DragFloat( "##exit", &tr.ExitTime, 0.01f, 0.0f, 1.0f );
                }
                ImGui::TextDisabled( "Conditions (all must hold)" );
                for ( int ci = 0; ci < static_cast<int>( tr.Conditions.size() ); ++ci )
                {
                    auto& c = tr.Conditions[ci];
                    ImGui::PushID( ci );
                    ImGui::SetNextItemWidth( 85 );
                    const char* cp = c.Parameter.empty() ? "param" : c.Parameter.c_str();
                    if ( ImGui::BeginCombo( "##cp", cp ) )
                    {
                        for ( const auto& p : graph.Parameters )
                            if ( ImGui::Selectable( p.Name.c_str(), c.Parameter == p.Name ) )
                            {
                                c.Parameter = p.Name;
                                dirty       = true;
                            }
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth( 60 );
                    dirty |= ImGui::Combo( "##co", &c.Op, kOpNames, IM_ARRAYSIZE( kOpNames ) );
                    const auto op = static_cast<G::CompareOp>( c.Op );
                    if ( op != G::CompareOp::IsTrue && op != G::CompareOp::IsFalse )
                    {
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth( 55 );
                        dirty |= ImGui::DragFloat( "##cv", &c.Value, 0.05f );
                    }
                    ImGui::SameLine();
                    if ( ImGui::SmallButton( "x" ) )
                    {
                        tr.Conditions.erase( tr.Conditions.begin() + ci );
                        dirty = true;
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
                if ( ImGui::SmallButton( "+ Condition" ) )
                {
                    tr.Conditions.push_back( {} );
                    dirty = true;
                }
            }
        }
        else
        {
            ImGui::TextDisabled( "Select a state or transition to edit it.\nDrag out -> in to connect." );
        }

        ImGui::EndChild();

        if ( dirty )
            MarkEdited();
    }

} // namespace Desert::Editor
