#include "VFXDataChannelDocument.hpp"

#include <Editor/Core/AssetOpen.hpp>
#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/Commands/VFXDataChannelEdit.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/SubjectTitle.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/VFXDataChannelAsset.hpp>

#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace Desert::Editor
{
    namespace
    {
        using FieldType = Assets::Serialization::VFXDataChannelFieldType;

        // Every type a field may have, in the order the type combo lists them.
        constexpr std::array<FieldType, 5> kFieldTypes = { FieldType::Position, FieldType::Direction,
                                                           FieldType::Color, FieldType::Float, FieldType::Int };

        // Exhaustive on purpose (no default): a type added to the enum is a -Wswitch warning here rather than a
        // combo entry with no label.
        const char* FieldTypeLabel( const FieldType type )
        {
            switch ( type )
            {
                case FieldType::Position:
                    return "Position (3 floats, cm)";
                case FieldType::Direction:
                    return "Direction (3 floats)";
                case FieldType::Color:
                    return "Color (4 floats, linear)";
                case FieldType::Float:
                    return "Float";
                case FieldType::Int:
                    return "Int";
            }
            return "?";
        }

        void CopyName( std::array<char, 64>& buffer, const std::string& name )
        {
            const std::size_t n = std::min( name.size(), buffer.size() - 1 );
            std::copy_n( name.data(), n, buffer.data() );
            buffer[n] = '\0';
        }
    } // namespace

    VFXDataChannelDocument::VFXDataChannelDocument( const Assets::AssetHandle& subject,
                                                    Assets::AssetManager*      assets )
         : ISubjectDocument(
                AssetSubjectTitle( subject, assets, "VFX Data Channel" ),
                AssetSubject( subject, static_cast<uint32_t>( Assets::AssetTypeID::VFXDataChannel ) ) ),
           m_Assets( assets )
    {
        const auto asset =
             m_Assets != nullptr ? m_Assets->FindByHandle<Assets::VFXDataChannelAsset>( subject ) : nullptr;
        if ( !asset )
        {
            m_LoadError = "the channel asset is not held by the asset manager";
            return;
        }
        if ( !asset->IsReadyForUse() )
        {
            if ( auto loaded = asset->LoadFromFile(); !loaded )
            {
                m_LoadError = loaded.GetError();
                return;
            }
        }
        m_Working = asset->GetData();
        m_Saved   = m_Working;
        m_Loaded  = true;
    }

    VFXDataChannelDocument::~VFXDataChannelDocument()
    {
        // The history's entries point at m_Working, which dies with this window.
        CommandHistory::Get().DropFor( &m_Working );
    }

    bool VFXDataChannelDocument::IsSubjectAlive() const
    {
        return m_Assets != nullptr &&
               m_Assets->FindMetadataByHandle( Assets::AssetHandle( Subject().Owner ) ) != nullptr;
    }

    ISubjectDocument::DiskState VFXDataChannelDocument::GetDiskState() const
    {
        if ( !m_Loaded )
            return DiskState::Untracked;
        return m_Working.Fields != m_Saved.Fields ? DiskState::Dirty : DiskState::Clean;
    }

    bool VFXDataChannelDocument::SaveDocument()
    {
        if ( !m_Loaded || m_Assets == nullptr )
            return false;
        const Assets::AssetHandle handle( Subject().Owner );
        const auto*               meta = m_Assets->FindMetadataByHandle( handle );
        if ( meta == nullptr )
        {
            m_Refusal = "Save: the channel asset is no longer held by the asset manager";
            return false;
        }
        if ( auto saved = Assets::VFXDataChannelAsset::Save( meta->Filepath, m_Working ); !saved )
        {
            m_Refusal = "Save: " + saved.GetError();
            LOG_ERROR( "[VFX] '{}' was not saved: {}", meta->Filepath.string(), saved.GetError() );
            return false;
        }
        m_Saved = m_Working;
        m_Refusal.clear();
        // The held asset re-reads what was written, so a running channel and the next open see the new layout.
        if ( const auto asset = m_Assets->FindByHandle<Assets::VFXDataChannelAsset>( handle ) )
        {
            if ( auto reloaded = asset->LoadFromFile(); !reloaded )
                LOG_ERROR( "[VFX] '{}' was saved but would not be read back: {}", meta->Filepath.string(),
                           reloaded.GetError() );
        }
        return true;
    }

    void VFXDataChannelDocument::Report( const Common::BoolResultStr& result )
    {
        if ( result.IsSuccess() )
            m_Refusal.clear();
        else
            m_Refusal = result.GetError();
    }

    void VFXDataChannelDocument::DrawFieldRow( const std::size_t index )
    {
        // Indexed on every use, never held by reference: an accepted edit replaces the field vector.
        auto& history = CommandHistory::Get();
        ImGui::PushID( static_cast<int>( index ) );
        ImGui::TableNextRow();

        // NAME: committed once, when the text box is left after an edit - one rename, one undo step.
        ImGui::TableSetColumnIndex( 0 );
        std::array<char, 64> shown{};
        const bool           typing = m_NameRow == index;
        if ( !typing )
            CopyName( shown, m_Working.Fields[index].Name );
        auto& buffer = typing ? m_NameBuffer : shown;
        ImGui::SetNextItemWidth( -1.0f );
        ImGui::InputText( "##name", buffer.data(), buffer.size() );
        if ( ImGui::IsItemActivated() )
        {
            m_NameRow = index;
            CopyName( m_NameBuffer, m_Working.Fields[index].Name );
        }
        if ( ImGui::IsItemDeactivated() )
        {
            const std::string typed = buffer.data();
            m_NameRow               = kNoRow;
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                Report( RenameChannelField( m_Working, history, index, typed ) );
        }

        // TYPE
        ImGui::TableSetColumnIndex( 1 );
        ImGui::SetNextItemWidth( -1.0f );
        if ( ImGui::BeginCombo( "##type", FieldTypeLabel( m_Working.Fields[index].Type ) ) )
        {
            for ( const FieldType type : kFieldTypes )
            {
                if ( ImGui::Selectable( FieldTypeLabel( type ), type == m_Working.Fields[index].Type ) &&
                     type != m_Working.Fields[index].Type )
                    Report( RetypeChannelField( m_Working, history, index, type ) );
            }
            ImGui::EndCombo();
        }

        // ORDER and REMOVE. `index` stays valid for the rest of this row only: every action below is the
        // row's last use of the list.
        ImGui::TableSetColumnIndex( 2 );
        const std::size_t count = m_Working.Fields.size();
        ImGui::BeginDisabled( index == 0 );
        const bool up = ImGui::SmallButton( ICON_MDI_ARROW_UP );
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled( index + 1 >= count );
        const bool down = ImGui::SmallButton( ICON_MDI_ARROW_DOWN );
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool remove = ImGui::SmallButton( ICON_MDI_CLOSE );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Remove this field" );
        ImGui::PopID();

        if ( up )
            Report( MoveChannelField( m_Working, history, index, index - 1 ) );
        else if ( down )
            Report( MoveChannelField( m_Working, history, index, index + 1 ) );
        else if ( remove )
            Report( RemoveChannelField( m_Working, history, index ) );
    }

    void VFXDataChannelDocument::OnUIRender()
    {
        if ( !m_Loaded )
        {
            ImGui::TextWrapped( "This channel could not be read: %s", m_LoadError.c_str() );
            return;
        }

        if ( ImGui::Button( ICON_MDI_CONTENT_SAVE " Save" ) )
            (void)SaveDocument();
        ImGui::SameLine();
        if ( ImGui::Button( ICON_MDI_PLUS " Add field" ) )
            Report( AddChannelField( m_Working, CommandHistory::Get(), FieldType::Float ) );
        ImGui::SameLine();
        ImGui::TextDisabled( "%zu / %zu fields", m_Working.Fields.size(),
                             Assets::Serialization::kVFXDataChannelMaxFields );

        if ( !m_Refusal.empty() )
            ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ), "%s", m_Refusal.c_str() );

        constexpr ImGuiTableFlags kTable = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg;
        if ( ImGui::BeginTable( "##fields", 3, kTable ) )
        {
            ImGui::TableSetupColumn( "Name", ImGuiTableColumnFlags_WidthStretch );
            ImGui::TableSetupColumn( "Type", ImGuiTableColumnFlags_WidthStretch );
            ImGui::TableSetupColumn( "", ImGuiTableColumnFlags_WidthFixed );
            ImGui::TableHeadersRow();
            // A row's action may change the list's size (remove) or order (move); the loop re-reads the size,
            // and a moved field simply draws at its new place next frame.
            for ( std::size_t i = 0; i < m_Working.Fields.size(); ++i )
                DrawFieldRow( i );
            ImGui::EndTable();
        }
    }

    SubjectEditorRegistry::PathOpenOutcome RequestVFXDataChannelDocument( Assets::AssetManager*        assets,
                                                                          const std::string&           path,
                                                                          const SubjectEditorRegistry& editors )
    {
        using Outcome = SubjectEditorRegistry::PathOpenOutcome;
        std::error_code ec;
        if ( assets == nullptr ||
             std::filesystem::path( path ).extension() != Assets::Serialization::kVFXDataChannelExtension ||
             !std::filesystem::exists( path, ec ) )
            return Outcome::NotMine;

        auto asset = assets->FindByPath<Assets::VFXDataChannelAsset>( path );
        if ( !asset )
            asset = assets->CreateAsset<Assets::VFXDataChannelAsset>( path );
        if ( !asset )
        {
            LOG_ERROR( "[VFX] '{}' could not be registered as a data channel - no editor was opened.", path );
            return Outcome::Failed;
        }
        if ( !asset->IsReadyForUse() )
        {
            if ( const auto loaded = asset->LoadFromFile(); !loaded )
            {
                LOG_ERROR( "[VFX] '{}' would not load as a data channel - no editor was opened: {}", path,
                           loaded.GetError() );
                return Outcome::Failed;
            }
        }

        const auto handle = asset->GetMetadata().Handle;
        if ( const auto opened = Core::RequestOpenAsset( assets->FindMetadataByHandle( handle ), handle, editors );
             !opened.IsSuccess() )
        {
            LOG_ERROR( "[VFX] '{}': {}", path, opened.GetError() );
            return Outcome::Failed;
        }
        return Outcome::Requested;
    }
} // namespace Desert::Editor
