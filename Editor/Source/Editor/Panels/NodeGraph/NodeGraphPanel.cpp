#include "NodeGraphPanel.hpp"

#include <Editor/Core/SubjectOpenRequest.hpp>
#include <Editor/Panels/MaterialEditor/MaterialShaderRebuild.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Assets/Shader/ShaderAsset.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <imgui-node-editor/imgui_node_editor.h>

#include <rflcpp/rfl/json.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>

namespace ed = ax::NodeEditor;

namespace Desert::Editor
{
    // Inside Desert::* the unqualified name ImGui resolves to the engine's Desert::ImGui wrapper —
    // alias the real Dear ImGui back in (same trick the other panels use).
    namespace ImGui = ::ImGui;

    namespace
    {
        namespace SG = ShaderGraph;

        // One colour per pin type, kept as a TABLE — the four rows are meant to be read side by side, and
        // clang-format 18 splits each of them into two lines the moment one is touched. Fenced for the
        // same reason the node catalogue in ShaderGraph.cpp is.
        // clang-format off
        ImU32 PinColor( int type )
        {
            switch ( static_cast<SG::ValueType>( type ) )
            {
                case SG::ValueType::Float: return IM_COL32( 145, 210, 130, 255 );
                case SG::ValueType::Vec2:  return IM_COL32( 240, 200, 90, 255 );
                case SG::ValueType::Color: return IM_COL32( 235, 120, 120, 255 );
                // Distinct from Color on purpose: a vec3 and a vec4 do not link to each other, so two pins
                // an artist cannot join must not be the same colour on the canvas.
                case SG::ValueType::Vec3:  return IM_COL32( 150, 170, 240, 255 );
            }
            return IM_COL32_WHITE;
        }
        // clang-format on

        std::filesystem::path GraphsDirectory()
        {
            return Common::Constants::Path::ASSETS_PATH / "ShaderGraphs";
        }

        std::filesystem::path CompiledShaderPath( const std::string& name )
        {
            return Common::Constants::Path::SHADERDIR_PATH / "Programs" / "Graph" / ( name + ".shader" );
        }

        // The scratch material a graph's shader is previewed on. One per graph, next to the graph itself,
        // so it is an ordinary project asset the artist can also drop onto a mesh.
        std::filesystem::path PreviewMaterialPath( const std::string& name )
        {
            return Common::Constants::Path::MATERIAL_PATH / ( name + "_Preview.demat" );
        }

        // Fills @p doc with a minimal, compiles-as-is starter graph for the domain: Surface gets
        // BaseColor -> Surface Output; PostProcess gets Scene Color -> Post Process Output (a
        // passthrough). Shared by the "New" button and the File Explorer's create action.
        void PopulateStarter( SG::Document& doc, SG::Domain domain )
        {
            if ( domain == SG::Domain::Volume )
            {
                // A CLOUD SAMPLE AND AN OUTPUT WITH NOTHING WIRED — which compiles to the SHIPPED medium
                // exactly, because every unconnected pin emits the engine's own default. So a brand new
                // Volume graph, assigned to a layer, changes not one pixel of the sky; the artist then
                // wires what they want to change. A starter that emitted a plausible-looking formula
                // would put a different sky on screen the moment the material was assigned and leave
                // them guessing which half of it was theirs.
                auto sample = SG::MakeNode( doc, "CloudSample" );
                sample.X    = 0.0f;
                sample.Y    = 60.0f;

                auto output = SG::MakeNode( doc, "VolumeOutput" );
                output.X    = 360.0f;
                output.Y    = 60.0f;

                doc.Nodes.push_back( std::move( sample ) );
                doc.Nodes.push_back( std::move( output ) );
                return;
            }

            if ( domain == SG::Domain::PostProcess )
            {
                auto scene = SG::MakeNode( doc, "SceneColor" );
                scene.X    = 0.0f;
                scene.Y    = 60.0f;

                auto output = SG::MakeNode( doc, "PostProcessOutput" );
                output.X    = 320.0f;
                output.Y    = 60.0f;

                doc.Links.push_back( { doc.NextId++, scene.Outputs[0].Id, output.Inputs[0].Id } );
                doc.Nodes.push_back( std::move( scene ) );
                doc.Nodes.push_back( std::move( output ) );
                return;
            }

            auto param      = SG::MakeNode( doc, "ColorParam" );
            param.ParamName = "BaseColor";
            param.Value     = { 0.8f, 0.4f, 0.1f, 1.0f };
            param.X         = 0.0f;
            param.Y         = 60.0f;

            auto output = SG::MakeNode( doc, "SurfaceOutput" );
            output.X    = 320.0f;
            output.Y    = 40.0f;

            doc.Links.push_back( { doc.NextId++, param.Outputs[0].Id, output.Inputs[0].Id } );
            doc.Nodes.push_back( std::move( param ) );
            doc.Nodes.push_back( std::move( output ) );
        }
    } // namespace

    NodeGraphPanel::NodeGraphPanel( const std::shared_ptr<Assets::AssetManager>& assetManager )
         : IPanel( "Node Graph", /*showPanel=*/false ), m_AssetManager( assetManager )
    {
        ed::Config config;
        config.SettingsFile = nullptr; // node positions live in the .dgraph, not a stray json
        m_Context           = ed::CreateEditor( &config );

        NewGraph();
    }

    NodeGraphPanel::~NodeGraphPanel()
    {
        if ( m_Context )
            ed::DestroyEditor( m_Context );
    }

    void NodeGraphPanel::NewGraph()
    {
        // A fresh graph keeps the domain you were working in.
        const SG::Domain domain = m_Doc.DomainEnum();
        m_Doc                   = {};
        m_Doc.Domain            = static_cast<int>( domain );
        PopulateStarter( m_Doc, domain );

        m_ApplyPositions = true;
        m_Status.clear();

        // A graph that was just loaded or created has not been EDITED, so it must not look dirty to the
        // auto-compile. Seeding the fingerprint here is what makes the debounce fire on a change rather
        // than on merely opening the panel — which cost a ~370 ms rebuild for nothing, and was visible as
        // a GraphShader.shader written the moment the window appeared.
        m_LastFingerprint = StructuralFingerprint( m_Doc );
        m_DirtySince      = {};
    }

    void NodeGraphPanel::ChangeDomain( SG::Domain domain )
    {
        if ( m_Doc.DomainEnum() == domain )
            return;
        m_Doc.Domain = static_cast<int>( domain );

        auto ownsPin = []( const SG::Node& n, uint64_t pin )
        {
            for ( const auto& p : n.Inputs )
                if ( p.Id == pin )
                    return true;
            for ( const auto& p : n.Outputs )
                if ( p.Id == pin )
                    return true;
            return false;
        };

        // Drop nodes that don't belong to the new domain (the old output, Scene Color, Tile UV, ...)
        // and any links touching their pins; the shared core (math / Time / params) stays.
        std::erase_if( m_Doc.Nodes,
                       [&]( const SG::Node& n )
                       {
                           const SG::NodeSpec* spec = SG::FindSpec( n.Kind );
                           const bool          drop = spec && !SG::SpecInDomain( *spec, domain );
                           if ( drop )
                               std::erase_if( m_Doc.Links, [&]( const SG::Link& l )
                                              { return ownsPin( n, l.From ) || ownsPin( n, l.To ); } );
                           return drop;
                       } );

        // Guarantee the graph still terminates in the new domain.
        const char* outKind = SG::OutputKind( domain );
        const bool  hasOut  = std::any_of( m_Doc.Nodes.begin(), m_Doc.Nodes.end(),
                                           [&]( const SG::Node& n ) { return n.Kind == outKind; } );
        if ( !hasOut )
        {
            auto output = SG::MakeNode( m_Doc, outKind );
            output.X    = 320.0f;
            output.Y    = 60.0f;
            m_Doc.Nodes.push_back( std::move( output ) );
        }

        m_ApplyPositions = true;
        m_Status.clear();
    }

    const SG::Pin* NodeGraphPanel::FindPin( uint64_t id ) const
    {
        for ( const auto& node : m_Doc.Nodes )
        {
            for ( const auto& pin : node.Inputs )
                if ( pin.Id == id )
                    return &pin;
            for ( const auto& pin : node.Outputs )
                if ( pin.Id == id )
                    return &pin;
        }
        return nullptr;
    }

    bool NodeGraphPanel::IsInputPin( uint64_t id ) const
    {
        for ( const auto& node : m_Doc.Nodes )
            for ( const auto& pin : node.Inputs )
                if ( pin.Id == id )
                    return true;
        return false;
    }

    void NodeGraphPanel::SaveGraph()
    {
        // Node positions are canvas state — pull them into the document before writing.
        ed::SetCurrentEditor( m_Context );
        for ( auto& node : m_Doc.Nodes )
        {
            const ImVec2 pos = ed::GetNodePosition( ed::NodeId( node.Id ) );
            node.X           = pos.x;
            node.Y           = pos.y;
        }
        ed::SetCurrentEditor( nullptr );

        std::error_code ec;
        std::filesystem::create_directories( GraphsDirectory(), ec );
        const auto path = GraphsDirectory() / ( m_Doc.Name + ".dgraph" );
        // The status line is the only feedback this panel has, and it used to say "Saved <file>" for a
        // write nobody had checked — while the same file's LOAD and COMPILE paths below both report
        // their failures properly.
        if ( const auto written =
                  Common::Utils::FileSystem::WriteContentToFileAtomic( path, SG::Serialize( m_Doc ) );
             !written )
        {
            m_Status        = "NOT saved: " + written.GetError();
            m_StatusIsError = true;
            return;
        }
        m_Status        = "Saved " + path.filename().string();
        m_StatusIsError = false;
    }

    void NodeGraphPanel::LoadGraph( const std::string& fileName )
    {
        LoadGraphFromPath( ( GraphsDirectory() / fileName ).string() );
    }

    void NodeGraphPanel::LoadGraphFromPath( const std::string& fullPath )
    {
        // NO `std::filesystem::exists` GUARD. One stood here and returned silently — so a graph offered
        // by the popup above and then not loaded left the panel showing the previous document with no
        // word anywhere about why. It was also the second half of the same defect the popup had: it
        // consults the DISK only, so a graph the shared enumeration found inside a mounted .dpak was
        // listed and then refused without a message. The read below already answers a missing or
        // unreadable file through the status line, which is this panel's own error channel.
        const auto raw = Common::Utils::FileSystem::ReadFileContent( fullPath );
        if ( !raw )
        {
            m_Status        = raw.GetError();
            m_StatusIsError = true;
            return;
        }

        auto parsed = SG::Deserialize( raw.GetValue() );
        if ( !parsed )
        {
            m_Status        = parsed.GetError();
            m_StatusIsError = true;
            return;
        }
        m_Doc            = parsed.GetValue().Doc;
        m_ApplyPositions = true;
        m_Status         = "Loaded " + std::filesystem::path( fullPath ).filename().string();
        m_StatusIsError  = false;

        // A migration is never silent (contract §4.7): it says which file, and how many pins moved.
        // The graph is NOT written back here — a load that rewrites the artist's file before they
        // have looked at it is worse than one that waits for Save, and the document in memory is
        // already the new form, so nothing downstream sees the old one.
        if ( const int migrated = parsed.GetValue().MigratedPins; migrated > 0 )
        {
            LOG_INFO( "[ShaderGraph] '{}' was written against an older node catalogue: {} pin(s) "
                      "appended to bring it up to date. Save the graph to persist them.",
                      fullPath, migrated );
            m_Status += std::format( " (+{} new pin(s) from the current catalogue)", migrated );
        }

        // Loaded, not edited — see the note in NewGraph.
        m_LastFingerprint = StructuralFingerprint( m_Doc );
        m_DirtySince      = {};
    }

    std::string NodeGraphPanel::CreateNewGraphFile( const std::string& directory, SG::Domain domain )
    {
        // Unique name: NewShaderGraph, NewShaderGraph1, ... (also used as the shader name, so it
        // must stay a valid identifier).
        std::string           name = "NewShaderGraph";
        std::filesystem::path path;
        for ( int i = 0; i < 256; ++i )
        {
            const std::string candidate = i == 0 ? name : name + std::to_string( i );
            path                        = std::filesystem::path( directory ) / ( candidate + ".dgraph" );
            if ( !std::filesystem::exists( path ) )
            {
                name = candidate;
                break;
            }
        }

        ShaderGraph::Document doc;
        doc.Name   = name;
        doc.Domain = static_cast<int>( domain );
        PopulateStarter( doc, domain );

        // The returned path IS the proof the file exists — the caller opens it. An unwritten graph came
        // back as a path all the same, and the panel then opened nothing and said nothing.
        if ( const auto written =
                  Common::Utils::FileSystem::WriteContentToFileAtomic( path, ShaderGraph::Serialize( doc ) );
             !written )
        {
            LOG_ERROR( "[ShaderGraph] '{}' was not created: {}", path.string(), written.GetError() );
            return {};
        }
        return path.string();
    }

    // One pending request is plenty — the last double-click wins.
    static std::string s_PendingOpenRequest;

    void NodeGraphPanel::RequestOpen( const std::string& dgraphPath )
    {
        s_PendingOpenRequest = dgraphPath;
    }

    void NodeGraphPanel::OnPreUpdate()
    {
        if ( !s_PendingOpenRequest.empty() )
        {
            LoadGraphFromPath( s_PendingOpenRequest );
            s_PendingOpenRequest.clear();
            GetVisibility()   = true; // double-click opens the panel even if it was hidden
            m_LastFingerprint = StructuralFingerprint( m_Doc );
            m_DirtySince      = {};
        }

        AutoCompileIfSettled();
    }

    // A cheap structural signature of the graph: what it MEANS, not where it sits. Node positions are
    // excluded deliberately -- dragging a node around changes no generated GLSL, and recompiling for it
    // would burn ~370 ms of shaderc to produce byte-identical output.
    uint64_t NodeGraphPanel::StructuralFingerprint( const ShaderGraph::Document& doc )
    {
        uint64_t   h   = 1469598103934665603ull; // FNV-1a
        const auto mix = [&h]( uint64_t v )
        {
            h ^= v;
            h *= 1099511628211ull;
        };
        const auto mixStr = [&mix]( const std::string& s )
        {
            for ( const char c : s )
                mix( static_cast<uint64_t>( static_cast<unsigned char>( c ) ) );
        };

        mixStr( doc.Name );
        mix( static_cast<uint64_t>( doc.DomainEnum() ) );
        for ( const auto& n : doc.Nodes )
        {
            mix( n.Id );
            mixStr( n.Kind );
            mixStr( n.ParamName );
            for ( const float v : n.Value )
                mix( static_cast<uint64_t>( static_cast<int64_t>( v * 100000.0f ) ) );
            // n.X / n.Y deliberately absent — see the note above.
        }
        for ( const auto& l : doc.Links )
        {
            mix( l.From );
            mix( l.To );
        }
        return h;
    }

    // Recompile once the artist has STOPPED editing, not on every touch.
    //
    // The constant comes from a measurement rather than a guess. A cold editor start costs ~15.1 s more
    // than a warm one and writes 123 SPIR-V modules, so a module is ~123 ms here, and a Surface graph emits
    // three (vertex + fragment, plus the depth pass) -- call it ~370 ms per rebuild. The SPIR-V cache does
    // not help: its key is the compiled text, and a preview recompile happens precisely BECAUSE that text
    // just changed, so every one is a miss. Compiling on each keystroke would therefore queue rebuilds
    // faster than they complete and the editor would never catch up.
    //
    // The delay is set at the cost of one rebuild, so a settled graph is on screen in about the time a
    // rebuild takes and a dragged slider produces exactly one compile at the end instead of a dozen.
    // PARAMETER values are not in this path at all -- they are uniform-buffer fields, edited live in the
    // preview window with no recompile whatsoever.
    void NodeGraphPanel::AutoCompileIfSettled()
    {
        if ( !m_AutoCompile )
            return;

        const uint64_t fingerprint = StructuralFingerprint( m_Doc );
        const auto     now         = std::chrono::steady_clock::now();

        if ( fingerprint != m_LastFingerprint )
        {
            m_LastFingerprint = fingerprint;
            m_DirtySince      = now; // the clock restarts on every edit: this is a debounce, not a timer
            return;
        }

        if ( m_DirtySince == std::chrono::steady_clock::time_point{} )
            return; // nothing pending

        if ( now - m_DirtySince < kAutoCompileDelay )
            return;

        m_DirtySince = {};
        Compile();
    }

    void NodeGraphPanel::Compile()
    {
        const auto source = SG::CompileToDShader( m_Doc );
        if ( !source )
        {
            m_Status        = "Compile error: " + source.GetError();
            m_StatusIsError = true;
            return;
        }

        const auto path = CompiledShaderPath( m_Doc.Name );
        std::error_code ec;
        std::filesystem::create_directories( path.parent_path(), ec );
        // A compile that produced correct source and could not store it is still a failed compile: the
        // status line below promises "hot reload applies it", and hot reload reads this file.
        if ( const auto written = Common::Utils::FileSystem::WriteContentToFileAtomic( path, source.GetValue() );
             !written )
        {
            m_Status        = "Compiled, but NOT written: " + written.GetError();
            m_StatusIsError = true;
            return;
        }

        const auto shaderService = Runtime::ResourceRegistry::GetShaderService();
        if ( shaderService->GetByName( m_Doc.Name ) )
        {
            // Already registered: the overwrite is picked up by the shader hot-reload poll.
            m_Status        = m_Doc.Name + ".shader recompiled — hot reload applies it";
            m_StatusIsError = false;
            PublishToPreview();
            return;
        }

        // First compile: register the new shader so it shows up in the material picker right away.
        const auto asset =
             m_AssetManager->CreateAsset<Assets::ShaderAsset>( Assets::AssetPriority::Medium, path );
        if ( !asset || !asset->IsReadyForUse() )
        {
            m_Status        = "wrote " + path.string() + " but shader failed to load (see log)";
            m_StatusIsError = true;
            return;
        }
        if ( const auto registered = shaderService->Register( asset ); !registered )
        {
            m_Status        = registered.GetError();
            m_StatusIsError = true;
            return;
        }
        m_Status        = m_Doc.DomainEnum() == SG::Domain::Volume
                               ? m_Doc.Name + " compiled + registered — drop it on a cloud material's Medium slot"
                               : m_Doc.Name + " compiled + registered — pick it in Material \\ Shader";
        m_StatusIsError = false;
        PublishToPreview();
    }

    Assets::AssetHandle NodeGraphPanel::EnsurePreviewMaterial()
    {
        if ( static_cast<uint64_t>( m_PreviewMaterial ) != 0 )
            return m_PreviewMaterial;
        if ( !m_AssetManager )
            return Assets::AssetHandle( static_cast<uint64_t>( 0 ) );

        const auto path = PreviewMaterialPath( m_Doc.Name );

        std::error_code ec;
        std::filesystem::create_directories( path.parent_path(), ec );

        // Reuse the file if a previous compile already made one for this graph, so recompiling does not
        // keep minting scratch materials. CreateAsset loads an existing .demat and produces an empty one
        // otherwise; either way the asset itself is what writes the file, through the material canon,
        // rather than this panel hand-rolling .demat JSON.
        auto asset =
             m_AssetManager->CreateAsset<Assets::SurfaceMaterialAsset>( Assets::AssetPriority::Medium, path );
        if ( !asset )
        {
            LOG_ERROR( "[NodeGraph] preview material '{}' could not be created", path.string() );
            return Assets::AssetHandle( static_cast<uint64_t>( 0 ) );
        }

        // ShaderName is the ONLY thing that makes this material the graph's; a material left over from an
        // earlier compile keeps whatever shader it had, so set it every time.
        asset->Data().ShaderName = m_Doc.Name;
        auto& materialId         = asset->Data().MaterialId;
        if ( !materialId.has_value() || materialId->IsNull() )
        {
            materialId = Common::UUID::Generate();
        }
        // Logged, and then carried on with deliberately: the preview material lives in memory for this
        // session and the registration below is what makes the preview render. The file only matters to
        // the NEXT session, so a failed write costs a stale preview material next launch — worth saying,
        // not worth refusing a preview for.
        const auto serialized = asset->Save();
        if ( !serialized )
        {
            LOG_ERROR( "[NodeGraph] the preview material '{}' was not written: {}", path.string(),
                       serialized.GetError() );
        }
        else if ( const auto written =
                       Common::Utils::FileSystem::WriteContentToFileAtomic( path, serialized.GetValue() );
                  !written )
        {
            LOG_ERROR( "[NodeGraph] the preview material '{}' was not written: {} — the preview works "
                       "this session, but the file will be stale on the next one.",
                       path.string(), written.GetError() );
        }

        if ( auto* materialService = Runtime::ResourceRegistry::GetMaterialService() )
            materialService->Register( asset );

        m_PreviewMaterial = asset->GetMetadata().Handle;
        return m_PreviewMaterial;
    }

    void NodeGraphPanel::PublishToPreview()
    {
        // Tell every Material Editor window on this shader to drop the pipelines it built from the OLD
        // modules. The shader object reloads itself, but pipelines are cached per SceneRenderer and the
        // hot-reload path only invalidates the main scene's cache — so without this a window would keep
        // drawing the previous compile while the viewport drew the new one.
        MaterialShaderRebuild::Publish( m_Doc.Name );

        // NO SCRATCH MATERIAL FOR A CLOUD MEDIUM, and this is a refusal rather than an omission. The
        // preview material below is a SurfaceMaterialAsset whose ShaderName is this graph — which is
        // exactly what a medium is not: it declares no stages, nothing draws it, and a `.demat` naming it
        // would be a material the mesh path refuses by name (ShaderProgramMeta::DrawnByMeshPath). A
        // medium is seen by assigning it to the Medium slot of a cloud material and looking at the sky,
        // which is the whole viewport rather than a preview sphere; O1_DESIGN §9 п.3 kept the volume
        // preview as its own stage for exactly that reason.
        if ( m_Doc.DomainEnum() == SG::Domain::Volume )
            return;

        // Open-or-focus the document for this graph's scratch material. One window per material now, so a
        // recompile brings the SAME window forward rather than re-pointing a shared one.
        if ( const auto material = EnsurePreviewMaterial(); static_cast<uint64_t>( material ) != 0 )
            Core::SubjectOpenRequests::Request(
                 AssetSubject( material, static_cast<uint32_t>( Assets::AssetTypeID::Material ) ) );
    }

    void NodeGraphPanel::DrawToolbar()
    {
        char nameBuf[64];
        std::snprintf( nameBuf, sizeof( nameBuf ), "%s", m_Doc.Name.c_str() );
        ImGui::SetNextItemWidth( 160.0f );
        if ( ImGui::InputText( "##graphName", nameBuf, sizeof( nameBuf ) ) )
            m_Doc.Name = nameBuf;

        ImGui::SameLine();
        if ( ImGui::Button( "New" ) )
            NewGraph();
        ImGui::SameLine();
        if ( ImGui::Button( "Save" ) )
            SaveGraph();

        ImGui::SameLine();
        if ( ImGui::Button( "Load" ) )
            ImGui::OpenPopup( "##loadGraph" );
        if ( ImGui::BeginPopup( "##loadGraph" ) )
        {
            // THROUGH THE ONE ENUMERATION (Common::Utils::FileSystem::ListFilesRecursive), which returns
            // both halves of the content world. A raw directory walk stood here, and a project served
            // from a mounted .dpak offered "no saved graphs" for a folder full of them.
            //
            // NAMED BY THE PATH RELATIVE TO THE GRAPHS FOLDER, not by the file name. The shared
            // enumeration is RECURSIVE where the old walk was one level deep, so a graph in a subfolder
            // is now offered — and it has to be addressable, or the menu would list a name that
            // LoadGraph could not resolve. Sorted, because neither half of the enumeration promises an
            // order and a menu that reshuffles between sessions is one nobody can learn.
            std::vector<std::string> graphs;
            for ( const std::filesystem::path& file :
                  Common::Utils::FileSystem::ListFilesRecursive( GraphsDirectory() ) )
            {
                if ( file.extension() != ".dgraph" )
                    continue;
                std::error_code   relEc;
                const std::string relative =
                     std::filesystem::relative( file, GraphsDirectory(), relEc ).generic_string();
                graphs.push_back( relEc || relative.empty() ? file.filename().generic_string() : relative );
            }
            std::sort( graphs.begin(), graphs.end() );

            for ( const std::string& graph : graphs )
            {
                if ( ImGui::MenuItem( graph.c_str() ) )
                    LoadGraph( graph );
            }
            if ( graphs.empty() )
                ImGui::TextDisabled( "no saved graphs" );
            ImGui::EndPopup();
        }

        // Domain picks the output node, the vertex contract and the palette (Material Domain / Mode).
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 130.0f );
        // SIZED FROM THE ARRAY, and it used to be a literal 2 sitting beside a live enum — so adding the
        // Volume domain would have compiled, offered two of three, and told nobody.
        const char* kDomains[] = { "Surface", "Post Process", "Cloud Medium" };
        int         domainIdx  = m_Doc.Domain;
        if ( ImGui::Combo( "##domain", &domainIdx, kDomains, static_cast<int>( std::size( kDomains ) ) ) )
            ChangeDomain( static_cast<SG::Domain>( domainIdx ) );

        // The scene's shading model — Surface only. The tooltip used to carry a third paragraph naming
        // what ticking this box did NOT give ("cascaded shadows from scene geometry"), because ticking
        // it produced a Lambert of the graph compiler's own and an artist could only learn the gap from
        // the picture. Д20 made ShadowFactor one shared text and the gap closed, so the paragraph is
        // gone rather than left to describe a difference that no longer exists.
        if ( m_Doc.DomainEnum() == SG::Domain::Surface )
        {
            ImGui::SameLine();
            ImGui::Checkbox( "Lit", &m_Doc.Lit );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Shade this surface with the engine's standard model: the baked sky "
                                   "(image-based ambient), the sun and every point/spot light through "
                                   "the same BRDF a PBR material uses, and both occluders of the sun — "
                                   "cascaded shadows from scene geometry and the cloud layer.\n\n"
                                   "Feed Metallic / Roughness / Occlusion on the Surface Output node; "
                                   "unwired they default to a standard material's 0 / 0.5 / 1." );
        }

        ImGui::SameLine();
        if ( ImGui::Button( "Compile" ) )
            Compile();

        ImGui::SameLine();
        if ( ImGui::Button( "Frame" ) )
        {
            ed::SetCurrentEditor( m_Context );
            ed::NavigateToContent( 0.4f );
            ed::SetCurrentEditor( nullptr );
        }

        if ( !m_Status.empty() )
        {
            ImGui::SameLine();
            ImGui::TextColored( m_StatusIsError ? ImVec4( 1.0f, 0.45f, 0.4f, 1.0f )
                                                : ImVec4( 0.5f, 0.9f, 0.5f, 1.0f ),
                                "%s", m_Status.c_str() );
        }
    }

    void NodeGraphPanel::DrawCanvas()
    {
        ed::SetCurrentEditor( m_Context );
        ed::Begin( "##shaderGraph", ImVec2( 0.0f, 0.0f ) );

        // --- Nodes ---
        for ( auto& node : m_Doc.Nodes )
        {
            const SG::NodeSpec* spec = SG::FindSpec( node.Kind );

            if ( m_ApplyPositions )
                ed::SetNodePosition( ed::NodeId( node.Id ), ImVec2( node.X, node.Y ) );

            ed::BeginNode( ed::NodeId( node.Id ) );

            const ImU32 header = spec ? spec->HeaderColor : IM_COL32_WHITE;
            ImGui::TextColored( ImGui::ColorConvertU32ToFloat4( header ), "%s",
                                spec ? spec->Title : node.Kind.c_str() );
            ImGui::Dummy( ImVec2( 0.0f, 2.0f ) );

            // Editable payload (param name / value) — inline, no popups (canvas-safe).
            if ( spec && spec->HasParamName )
            {
                char buf[48];
                std::snprintf( buf, sizeof( buf ), "%s", node.ParamName.c_str() );
                ImGui::SetNextItemWidth( 140.0f );
                if ( ImGui::InputText( ( "##pn" + std::to_string( node.Id ) ).c_str(), buf,
                                       sizeof( buf ) ) )
                    node.ParamName = buf;
            }
            if ( spec && spec->HasColorValue )
            {
                ImGui::SetNextItemWidth( 200.0f );
                ImGui::DragFloat4( ( "##cv" + std::to_string( node.Id ) ).c_str(), node.Value.data(),
                                   0.01f, 0.0f, 4.0f, "%.2f" );
            }
            if ( spec && spec->HasFloatValue )
            {
                ImGui::SetNextItemWidth( 100.0f );
                ImGui::DragFloat( ( "##fv" + std::to_string( node.Id ) ).c_str(), node.Value.data(),
                                  0.01f, -64.0f, 64.0f, "%.2f" );
            }

            ImGui::BeginGroup(); // inputs
            for ( const auto& pin : node.Inputs )
            {
                ed::BeginPin( ed::PinId( pin.Id ), ed::PinKind::Input );
                ImGui::TextColored( ImGui::ColorConvertU32ToFloat4( PinColor( pin.Type ) ), "-> %s",
                                    pin.Name.c_str() );
                ed::EndPin();
            }
            if ( node.Inputs.empty() )
                ImGui::Dummy( ImVec2( 1.0f, 1.0f ) );
            ImGui::EndGroup();

            ImGui::SameLine( 0.0f, 40.0f );

            ImGui::BeginGroup(); // outputs
            for ( const auto& pin : node.Outputs )
            {
                ed::BeginPin( ed::PinId( pin.Id ), ed::PinKind::Output );
                ImGui::TextColored( ImGui::ColorConvertU32ToFloat4( PinColor( pin.Type ) ), "%s ->",
                                    pin.Name.c_str() );
                ed::EndPin();
            }
            if ( node.Outputs.empty() )
                ImGui::Dummy( ImVec2( 1.0f, 1.0f ) );
            ImGui::EndGroup();

            ed::EndNode();
        }
        if ( m_ApplyPositions )
            ed::NavigateToContent( 0.0f ); // fresh/new/loaded graph: frame it
        m_ApplyPositions = false;

        // --- Links (coloured by the source pin's type) ---
        for ( const auto& link : m_Doc.Links )
        {
            const SG::Pin* from = FindPin( link.From );
            const ImU32    col  = from ? PinColor( from->Type ) : IM_COL32( 200, 200, 200, 255 );
            ed::Link( ed::LinkId( link.Id ), ed::PinId( link.From ), ed::PinId( link.To ),
                      ImGui::ColorConvertU32ToFloat4( col ), 2.0f );
        }

        // --- Link creation with type checking ---
        if ( ed::BeginCreate() )
        {
            ed::PinId a, b;
            if ( ed::QueryNewLink( &a, &b ) && a && b )
            {
                uint64_t from = a.Get(), to = b.Get();
                if ( IsInputPin( from ) )
                    std::swap( from, to );

                const SG::Pin* fromPin = FindPin( from );
                const SG::Pin* toPin   = FindPin( to );

                const bool kindsOk = fromPin && toPin && !IsInputPin( from ) && IsInputPin( to );
                const bool typesOk = kindsOk && fromPin->Type == toPin->Type;
                const bool freeOk =
                     typesOk && std::none_of( m_Doc.Links.begin(), m_Doc.Links.end(),
                                              [&]( const SG::Link& l ) { return l.To == to; } );

                if ( !kindsOk )
                    ed::RejectNewItem( ImVec4( 1.0f, 0.3f, 0.3f, 1.0f ), 2.0f );
                else if ( !typesOk || !freeOk )
                    ed::RejectNewItem( ImVec4( 1.0f, 0.5f, 0.2f, 1.0f ), 2.0f );
                else if ( ed::AcceptNewItem( ImVec4( 0.5f, 1.0f, 0.5f, 1.0f ), 3.0f ) )
                    m_Doc.Links.push_back( { m_Doc.NextId++, from, to } );
            }
        }
        ed::EndCreate();

        // --- Deletion ---
        if ( ed::BeginDelete() )
        {
            ed::LinkId deletedLink;
            while ( ed::QueryDeletedLink( &deletedLink ) )
            {
                if ( ed::AcceptDeletedItem() )
                    std::erase_if( m_Doc.Links,
                                   [&]( const SG::Link& l ) { return l.Id == deletedLink.Get(); } );
            }

            ed::NodeId deletedNode;
            while ( ed::QueryDeletedNode( &deletedNode ) )
            {
                if ( ed::AcceptDeletedItem() )
                {
                    const uint64_t id = deletedNode.Get();
                    if ( auto it = std::find_if( m_Doc.Nodes.begin(), m_Doc.Nodes.end(),
                                                 [&]( const SG::Node& n ) { return n.Id == id; } );
                         it != m_Doc.Nodes.end() )
                    {
                        std::erase_if( m_Doc.Links,
                                       [&]( const SG::Link& l )
                                       {
                                           auto owns = [&]( uint64_t pin )
                                           {
                                               for ( const auto& p : it->Inputs )
                                                   if ( p.Id == pin )
                                                       return true;
                                               for ( const auto& p : it->Outputs )
                                                   if ( p.Id == pin )
                                                       return true;
                                               return false;
                                           };
                                           return owns( l.From ) || owns( l.To );
                                       } );
                        m_Doc.Nodes.erase( it );
                    }
                }
            }
        }
        ed::EndDelete();

        // --- Right-click palette ---
        const ImVec2 popupCanvasPos = ImGui::GetMousePos();
        ed::Suspend();
        if ( ed::ShowBackgroundContextMenu() )
            ImGui::OpenPopup( "##nodePalette" );

        if ( ImGui::BeginPopup( "##nodePalette" ) )
        {
            ImGui::TextDisabled( "Add node" );
            ImGui::Separator();

            const SG::Domain domain  = m_Doc.DomainEnum();
            const char*      outKind = SG::OutputKind( domain );
            const bool       hasOutput =
                 std::any_of( m_Doc.Nodes.begin(), m_Doc.Nodes.end(),
                              [&]( const SG::Node& n ) { return n.Kind == outKind; } );

            for ( const auto& spec : SG::Specs() )
            {
                if ( !SG::SpecInDomain( spec, domain ) )
                    continue; // only nodes valid in this domain
                if ( spec.Kind == std::string( outKind ) && hasOutput )
                    continue; // exactly one output per graph
                if ( ImGui::MenuItem( spec.Title ) )
                {
                    auto node = SG::MakeNode( m_Doc, spec.Kind );
                    node.X    = popupCanvasPos.x;
                    node.Y    = popupCanvasPos.y;
                    ed::SetNodePosition( ed::NodeId( node.Id ), popupCanvasPos );
                    m_Doc.Nodes.push_back( std::move( node ) );
                }
            }
            ImGui::EndPopup();
        }
        ed::Resume();

        ed::End();
        ed::SetCurrentEditor( nullptr );
    }

    void NodeGraphPanel::OnUIRender()
    {
        DrawToolbar();

        // The canvas gets the whole panel now. The 128 px thumbnail that used to sit beside it is gone,
        // and not merely moved: it was rendered by AssetThumbnailRenderer through
        // MaterialComponent::ShaderName — the shader-OVERRIDE route — which is a different path in
        // MeshRenderer from the one a scene mesh takes. It therefore showed a correct material at times
        // when the scene showed something else (Docs/MaterialEditor/STAGE1_END_TO_END.md). Compile now
        // opens a Material Editor window on this graph's material ASSET, which draws it through the same
        // per-slot route the game uses.
        DrawCanvas();
    }
} // namespace Desert::Editor
