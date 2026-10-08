#include "FracturePanel.hpp"

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/Selection/ModelingToolTarget.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/Selection/ViewportMode.hpp>

#include <Engine/Assets/FractureAsset.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>

#include <Common/Core/Constants.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>

namespace Desert::Editor
{
    namespace
    {
        /// One Generate or interior-material edit: the file's bytes before and after (FractureAsset::FileStep).
        class FractureFileCommand final : public ICommand
        {
        public:
            FractureFileCommand( Assets::FractureAsset::FileStep step, std::string label )
                 : m_Step( std::move( step ) ), m_Label( std::move( label ) )
            {
            }

            bool Undo() override
            {
                return static_cast<bool>( Assets::FractureAsset::RestoreBytes( m_Step.File, m_Step.Before ) );
            }

            bool Redo() override
            {
                return static_cast<bool>( Assets::FractureAsset::RestoreBytes( m_Step.File, m_Step.After ) );
            }

            std::string GetLabel() const override
            {
                return m_Label;
            }

        private:
            Assets::FractureAsset::FileStep m_Step;
            std::string                     m_Label;
        };

        std::filesystem::path AssetFile( const char* relative )
        {
            return std::filesystem::path( Common::Constants::Path::ASSETS_PATH ) / relative;
        }

        std::string GuidText( const Common::Content::AssetGuid& guid )
        {
            return guid.IsNull() ? std::string() : std::format( "{:016x}{:016x}", guid.Hi, guid.Lo );
        }

        /// 32 hex digits (Hi then Lo) -> the GUID; empty text is the null GUID; anything else is refused.
        bool ParseGuid( const char* text, Common::Content::AssetGuid& out )
        {
            const std::string s( text );
            if ( s.empty() )
            {
                out = {};
                return true;
            }
            if ( s.size() != 32 )
                return false;
            Common::Content::AssetGuid guid;
            const auto                 hi = std::from_chars( s.data(), s.data() + 16, guid.Hi, 16 );
            const auto                 lo = std::from_chars( s.data() + 16, s.data() + 32, guid.Lo, 16 );
            if ( hi.ec != std::errc() || lo.ec != std::errc() || hi.ptr != s.data() + 16 ||
                 lo.ptr != s.data() + 32 )
                return false;
            out = guid;
            return true;
        }

        constexpr std::array<const char*, 4> kMethodNames = { "Uniform Voronoi", "Clustered Voronoi", "Planar",
                                                              "Brick" };
    } // namespace

    FracturePanel::FracturePanel( const std::shared_ptr<Desert::Core::Scene>& scene )
         : IPanel( "Fracture", /*showPanel=*/false ) // contextual: the mode opens it
           ,
           m_Scene( scene )
    {
        m_Settings.Levels.emplace_back();
    }

    bool FracturePanel::IsRelevant() const
    {
        return Core::ViewportMode::Get() == Core::EditorMode::Fracture;
    }

    void FracturePanel::OnUIRender()
    {
        DrawTarget();
        ImGui::Separator();
        DrawGenerate();
        ImGui::Separator();
        DrawInteriorMaterial();
        ImGui::Separator();
        DrawView();
        if ( !m_Status.empty() )
            ImGui::TextWrapped( "%s", m_Status.c_str() );
    }

    void FracturePanel::DrawTarget()
    {
        ImGui::TextUnformatted( "Geometry Collection (.dfrac, under Assets)" );
        ImGui::InputText( "##FracturePath", m_Path.data(), m_Path.size() );
        ImGui::SameLine();
        if ( ImGui::Button( "Load" ) )
            Load();
        if ( m_Loaded )
            ImGui::Text( "%zu nodes, %u levels", m_Fracture.Nodes.size(),
                         Destruction::DeepestLevel( m_Fracture.Nodes ) );
    }

    void FracturePanel::DrawGenerate()
    {
        ImGui::TextUnformatted( "Generate" );
        int seed = static_cast<int>( m_Settings.Seed & 0x7fffffff );
        if ( ImGui::InputInt( "Random Seed", &seed ) )
            m_Settings.Seed = static_cast<uint64_t>( std::max( seed, 0 ) );

        // UE fractures the selected level again to make the next one; ours keeps the list and re-bakes it all,
        // so every level is reproducible from the file's settings.
        for ( size_t i = 0; i < m_Settings.Levels.size(); ++i )
        {
            ImGui::PushID( static_cast<int>( i ) );
            DrawLevel( i, m_Settings.Levels[i] );
            ImGui::PopID();
        }
        if ( ImGui::Button( "Add Level" ) )
            m_Settings.Levels.push_back( m_Settings.Levels.empty() ? Destruction::FractureLevelSettings{}
                                                                   : m_Settings.Levels.back() );
        ImGui::SameLine();
        ImGui::BeginDisabled( m_Settings.Levels.size() <= 1 );
        if ( ImGui::Button( "Remove Level" ) )
            m_Settings.Levels.pop_back();
        ImGui::EndDisabled();

        ImGui::Checkbox( "Auto Cluster (grid)", &m_Settings.AutoCluster.Enabled );
        if ( m_Settings.AutoCluster.Enabled )
            ImGui::InputInt3( "Grid", &m_Settings.AutoCluster.GridX );

        if ( ImGui::Button( "Fracture (Generate)" ) )
            Generate();
    }

    void FracturePanel::DrawLevel( size_t index, Destruction::FractureLevelSettings& level )
    {
        ImGui::Text( "Level %zu", index + 1 );
        int method = static_cast<int>( level.Method );
        if ( ImGui::Combo( "Method", &method, kMethodNames.data(), static_cast<int>( kMethodNames.size() ) ) )
            level.Method = static_cast<Destruction::FractureMethod>( method );
        switch ( level.Method )
        {
            case Destruction::FractureMethod::Uniform:
                ImGui::InputInt( "Sites", &level.SiteCount );
                break;
            case Destruction::FractureMethod::Clustered:
                ImGui::InputInt( "Clusters", &level.Clusters );
                ImGui::InputInt( "Sites Per Cluster", &level.SitesPerCluster );
                ImGui::InputDouble( "Min Radius (cm)", &level.MinRadius );
                ImGui::InputDouble( "Max Radius (cm)", &level.MaxRadius );
                break;
            case Destruction::FractureMethod::Planar:
                ImGui::Text( "%zu planes (set by the palette / file)", level.Planes.size() );
                break;
            case Destruction::FractureMethod::Brick:
                ImGui::InputDouble( "Brick Length (cm)", &level.Brick.Length );
                ImGui::InputDouble( "Brick Height (cm)", &level.Brick.Height );
                ImGui::InputDouble( "Brick Depth (cm)", &level.Brick.Depth );
                break;
        }
        ImGui::InputFloat( "Damage Threshold", &level.DamageThreshold );
    }

    void FracturePanel::DrawInteriorMaterial()
    {
        ImGui::TextUnformatted( "Internal Material (material asset GUID, 32 hex digits)" );
        ImGui::InputText( "##InteriorGuid", m_InteriorGuid.data(), m_InteriorGuid.size() );
        ImGui::SameLine();
        ImGui::BeginDisabled( !m_Loaded );
        if ( ImGui::Button( "Apply" ) )
        {
            Destruction::FractureData next = m_Fracture;
            if ( !ParseGuid( m_InteriorGuid.data(), next.InteriorMaterial ) )
                m_Status = "The interior material GUID is 32 hex digits (or empty for none).";
            else
                Commit( next, "Set fracture interior material" );
        }
        ImGui::EndDisabled();
    }

    void FracturePanel::DrawView()
    {
        ImGui::TextUnformatted( "View (preview only, not saved)" );
        ImGui::SliderFloat( "Explode Amount", &m_View.ExplodeAmount, 0.0f, 1.0f );
        const int deepest = static_cast<int>( Destruction::DeepestLevel( m_Fracture.Nodes ) );
        ImGui::SliderInt( "Fracture Level (-1 = all)", &m_View.ViewLevel, -1, std::max( deepest, 0 ) );
        if ( !m_Loaded )
            return;

        // The preview: every leaf's hull from above (X right, Z down the canvas), moved by its exploded offset and
        // tinted by level (UE's bone colours).
        const auto   offsets = Destruction::ExplodedOffsets( m_Fracture.Nodes, m_View );
        const ImVec2 origin  = ImGui::GetCursorScreenPos();
        const float  side    = std::max( 160.0f, ImGui::GetContentRegionAvail().x );
        ImGui::InvisibleButton( "##FracturePreview", ImVec2( side, side ) );
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRect( origin, ImVec2( origin.x + side, origin.y + side ), IM_COL32( 90, 90, 90, 255 ) );

        double reach = 1.0;
        for ( size_t i = 0; i < m_Fracture.Nodes.size(); ++i )
            for ( const glm::vec3& v : m_Fracture.Nodes[i].HullVertices )
                reach = std::max( { reach, std::abs( v.x + offsets[i].x ), std::abs( v.z + offsets[i].z ) } );
        const float  scale = static_cast<float>( 0.45 * side / reach );
        const ImVec2 centre( origin.x + 0.5f * side, origin.y + 0.5f * side );

        constexpr std::array<ImU32, 4> kLevelColours = {
             IM_COL32( 230, 120, 60, 255 ), IM_COL32( 80, 170, 230, 255 ), IM_COL32( 120, 210, 90, 255 ),
             IM_COL32( 220, 200, 70, 255 ) };
        for ( size_t i = 0; i < m_Fracture.Nodes.size(); ++i )
        {
            const Destruction::FractureNode& node   = m_Fracture.Nodes[i];
            const ImU32                      colour = kLevelColours[node.Level % kLevelColours.size()];
            for ( const std::vector<int>& face : node.HullFaces )
                for ( size_t k = 0; k < face.size(); ++k )
                {
                    const glm::vec3& a = node.HullVertices[static_cast<size_t>( face[k] )];
                    const glm::vec3& b = node.HullVertices[static_cast<size_t>( face[( k + 1 ) % face.size()] )];
                    draw->AddLine( ImVec2( centre.x + scale * static_cast<float>( a.x + offsets[i].x ),
                                           centre.y + scale * static_cast<float>( a.z + offsets[i].z ) ),
                                   ImVec2( centre.x + scale * static_cast<float>( b.x + offsets[i].x ),
                                           centre.y + scale * static_cast<float>( b.z + offsets[i].z ) ),
                                   colour );
                }
        }
    }

    void FracturePanel::Load()
    {
        const auto    file = AssetFile( m_Path.data() );
        std::ifstream in( file, std::ios::binary );
        if ( !in )
        {
            m_Status = std::format( "'{}' does not exist yet: Fracture creates it.", file.string() );
            m_Loaded = false;
            return;
        }
        const std::vector<unsigned char> bytes( ( std::istreambuf_iterator<char>( in ) ),
                                                std::istreambuf_iterator<char>() );
        auto                             decoded = Destruction::DecodeFracture( bytes );
        if ( !decoded )
        {
            m_Status = std::format( "'{}' refused: {}", file.string(), decoded.GetError() );
            m_Loaded = false;
            return;
        }
        m_Fracture             = decoded.GetValue();
        m_Settings             = m_Fracture.Settings;
        m_Loaded               = true;
        const std::string guid = GuidText( m_Fracture.InteriorMaterial );
        m_InteriorGuid.fill( '\0' );
        std::copy_n( guid.begin(), std::min( guid.size(), m_InteriorGuid.size() - 1 ), m_InteriorGuid.begin() );
        m_Status.clear();
    }

    void FracturePanel::Generate()
    {
        const auto scene    = m_Scene.lock();
        const auto selected = Core::SelectionManager::GetSelected();
        if ( !scene || !selected.has_value() )
        {
            m_Status = "Select the static mesh entity to fracture.";
            return;
        }
        auto ref = scene->FindEntityByID( *selected );
        if ( !ref || !ref->get().HasComponent<ECS::StaticMeshComponent>() )
        {
            m_Status = "The selected entity has no static mesh.";
            return;
        }
        const auto target = GetToolTargetMesh( ref->get().GetComponent<ECS::StaticMeshComponent>() );
        if ( !target )
        {
            m_Status = std::format( "No mesh to fracture: {}", target.GetError() );
            return;
        }
        auto next = Destruction::GenerateFracture( *target.GetValue().Mesh, m_Fracture, m_Settings );
        if ( !next )
        {
            m_Status = next.GetError();
            return;
        }
        Commit( next.GetValue(), "Fracture (Generate)" );
    }

    void FracturePanel::Commit( const Destruction::FractureData& next, const char* label )
    {
        const auto file = AssetFile( m_Path.data() );
        auto       step = Assets::FractureAsset::WriteStep( file, next );
        if ( !step )
        {
            m_Status = step.GetError();
            return;
        }
        CommandHistory::Get().PushCommand( std::make_unique<FractureFileCommand>( step.GetValue(), label ) );
        Load();
    }
} // namespace Desert::Editor
