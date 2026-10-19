#include "WorldParametersPanel.hpp"

#include <Editor/Core/EditorSubject.hpp>
#include <Editor/Core/SubjectTitle.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/WorldParametersAsset.hpp>

#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <array>
#include <format>

namespace Desert::Editor
{
    namespace S = Assets::Serialization;

    namespace
    {
        constexpr std::array<const char*, 2> kTypes = { "Scalar", "Vector" };

        // A name the list does not hold yet: "Parameter", "Parameter1", ...
        std::string FreshName( const S::WorldParametersData& data )
        {
            for ( int n = 0;; ++n )
            {
                std::string name  = n == 0 ? std::string( "Parameter" ) : std::format( "Parameter{}", n );
                bool        taken = false;
                for ( const auto& p : data.Parameters )
                    taken = taken || p.Name == name;
                if ( !taken )
                    return name;
            }
        }

        void DrawStatus( const std::string& status, const bool isError )
        {
            if ( status.empty() )
                return;
            if ( isError )
                ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.4f, 1.0f ), "%s", status.c_str() );
            else
                ImGui::TextDisabled( "%s", status.c_str() );
        }
    } // namespace

    WorldParametersPanel::WorldParametersPanel( const Assets::AssetHandle& subject, Assets::AssetManager* assets )
         : ISubjectDocument(
                AssetSubjectTitle<Assets::WorldParametersAsset>( subject, assets, "World Parameters" ),
                AssetSubject( subject, static_cast<uint32_t>( Assets::AssetTypeID::WorldParameters ) ) ),
           m_Assets( assets ), m_Handle( subject ), m_Data( std::make_shared<Data>() )
    {
        const auto asset = m_Assets ? m_Assets->FindByHandle<Assets::WorldParametersAsset>( m_Handle ) : nullptr;
        if ( !asset )
        {
            m_Status        = "the asset is not registered";
            m_StatusIsError = true;
            return;
        }
        if ( !asset->IsReadyForUse() )
            if ( const auto loaded = asset->LoadFromFile(); !loaded )
            {
                m_Status        = loaded.GetError();
                m_StatusIsError = true;
                return;
            }
        m_Path   = asset->GetMetadata().Filepath;
        *m_Data  = asset->GetData();
        m_OnDisk = *m_Data;
    }

    InputAssetOwner<WorldParametersPanel::Data> WorldParametersPanel::Owner() const
    {
        InputAssetOwner<Data> owner;
        owner.Label   = std::format( "World Parameters '{}'", m_Path.stem().string() );
        owner.Resolve = [weak = std::weak_ptr<Data>( m_Data )]() -> Data*
        {
            const auto data = weak.lock();
            return data.get(); // the window owns the copy: a closed window resolves to nothing
        };
        return owner;
    }

    void WorldParametersPanel::OnUIRender()
    {
        // No ImGui::Begin: the editor's panel loop wraps OnUIRender in this panel's window.
        if ( m_Path.empty() )
        {
            DrawStatus( m_Status, true );
            return;
        }
        Data& data = *m_Data;

        int removeAt = -1;
        if ( ImGui::BeginTable( "##WorldParameters", 4,
                                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                     ImGuiTableFlags_SizingStretchProp ) )
        {
            ImGui::TableSetupColumn( "Name", ImGuiTableColumnFlags_WidthStretch, 1.2f );
            ImGui::TableSetupColumn( "Type", ImGuiTableColumnFlags_WidthFixed, 90.0f );
            ImGui::TableSetupColumn( "Default", ImGuiTableColumnFlags_WidthStretch, 2.0f );
            ImGui::TableSetupColumn( "", ImGuiTableColumnFlags_WidthFixed, 24.0f );
            ImGui::TableHeadersRow();
            for ( int i = 0; i < static_cast<int>( data.Parameters.size() ); ++i )
            {
                S::WorldParameterData& p = data.Parameters[static_cast<size_t>( i )];
                ImGui::PushID( i );
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                std::array<char, 128> name{};
                p.Name.copy( name.data(), name.size() - 1 );
                ImGui::SetNextItemWidth( -1.0f );
                if ( ImGui::InputText( "##Name", name.data(), name.size() ) )
                    p.Name = name.data();
                if ( !S::IsWorldParameterName( p.Name ) && ImGui::IsItemHovered() )
                    ImGui::SetTooltip(
                         "A parameter name is an identifier: a letter or _, then letters, digits, _" );
                ImGui::TableNextColumn();
                int type = static_cast<int>( p.Type );
                ImGui::SetNextItemWidth( -1.0f );
                if ( ImGui::Combo( "##Type", &type, kTypes.data(), static_cast<int>( kTypes.size() ) ) )
                {
                    p.Type = static_cast<S::WorldParameterType>( type );
                    if ( p.Type == S::WorldParameterType::Scalar )
                        p.Default = { p.Default[0], 0.0f, 0.0f, 0.0f };
                }
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth( -1.0f );
                if ( p.Type == S::WorldParameterType::Scalar )
                    ImGui::DragFloat( "##Default", p.Default.data(), 0.01f );
                else
                    ImGui::DragFloat4( "##Default", p.Default.data(), 0.01f );
                ImGui::TableNextColumn();
                if ( ImGui::SmallButton( "x" ) )
                    removeAt = i;
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "Remove the parameter" );
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if ( removeAt >= 0 )
            (void)m_Edits.Edit( Owner(),
                                [removeAt]( Data& d )
                                {
                                    d.Parameters.erase( d.Parameters.begin() + removeAt );
                                    return Common::MakeSuccess( true );
                                } );
        if ( ImGui::Button( "Add Parameter" ) )
            (void)m_Edits.Edit( Owner(),
                                []( Data& d )
                                {
                                    d.Parameters.push_back(
                                         { FreshName( d ), S::WorldParameterType::Scalar, {} } );
                                    return Common::MakeSuccess( true );
                                } );

        ImGui::Separator();
        if ( ImGui::Button( "Save" ) )
            (void)SaveDocument();
        DrawStatus( m_Status, m_StatusIsError );

        (void)m_Edits.Observe( Owner(), ImGui::IsAnyItemActive() );
    }

    bool WorldParametersPanel::IsSubjectAlive() const
    {
        return m_Assets && m_Assets->FindMetadataByHandle( m_Handle ) != nullptr;
    }

    ISubjectDocument::DiskState WorldParametersPanel::GetDiskState() const
    {
        if ( m_Path.empty() )
            return DiskState::Untracked;
        return *m_Data == m_OnDisk ? DiskState::Clean : DiskState::Dirty;
    }

    bool WorldParametersPanel::SaveDocument()
    {
        // Through the asset serializer (which validates), then a re-read so the world re-adopts the defaults.
        Common::BoolResultStr saved = Assets::WorldParametersAsset::Save( m_Path, *m_Data );
        if ( saved && m_Assets != nullptr )
            if ( const auto asset = m_Assets->FindByHandle<Assets::WorldParametersAsset>( m_Handle ) )
                saved = asset->LoadFromFile();
        m_StatusIsError = !saved;
        m_Status        = saved ? std::format( "Saved {}", m_Path.filename().string() ) : saved.GetError();
        if ( !saved )
        {
            LOG_ERROR( "[WorldParameters] '{}' was not saved: {}", m_Path.string(), saved.GetError() );
            return false;
        }
        m_OnDisk = *m_Data;
        return true;
    }
} // namespace Desert::Editor
