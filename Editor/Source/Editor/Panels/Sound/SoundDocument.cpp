#include "SoundDocument.hpp"

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/EditorSubject.hpp>
#include <Editor/Core/SubjectOpenRequest.hpp>

#include <Engine/Assets/Common.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/SoundAsset.hpp>

#include <Audio/AudioMix.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/ContentKinds.hpp>
#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <system_error>

namespace Desert::Editor
{
    namespace
    {
        constexpr ImVec4 kRefusalColour( 1.0f, 0.4f, 0.4f, 1.0f );

        constexpr uint32_t kSoundType = static_cast<uint32_t>( Assets::AssetTypeID::Sound );

        // The `.desound` the registry row under @p handle names, or nullopt when no Sound row is there.
        std::optional<std::filesystem::path> SoundFileOf( const Assets::AssetHandle& handle )
        {
            const auto row = Assets::ContentRegistry::RowOf( Common::Content::ContentKind::Sound,
                                                             static_cast<uint64_t>( handle ) );
            if ( !row )
                return std::nullopt;
            return row->Path;
        }

        // A class's name as the project knows it: its asset's file stem (UE shows the SoundClass asset's name).
        // "None" for the null GUID; for a GUID no registry row states, the GUID itself, marked missing.
        std::string ClassDisplayName( const Audio::SoundClassId& id )
        {
            if ( id.IsNull() )
                return "None";
            const auto* row = Assets::ContentRegistry::Get().FindByHandle( Common::Content::HandleForGuid( id ) );
            if ( row == nullptr )
                return std::format( "(missing) {}", Common::Content::AssetGuidToText( id ) );
            return std::filesystem::path( row->Key ).stem().string();
        }
    } // namespace

    SoundDocument::SoundDocument( const Assets::AssetHandle& subject )
         : ISubjectDocument(
                SoundFileOf( subject ).value_or( std::filesystem::path( "Sound" ) ).filename().string(),
                AssetSubject( subject, kSoundType ) )
    {
        const auto file = SoundFileOf( subject );
        if ( !file )
        {
            m_LoadError = "the content registry lists no sound under this handle";
            return;
        }
        m_File            = *file;
        const auto parsed = Assets::SoundAsset::ReadFile( m_File );
        if ( !parsed )
        {
            m_LoadError = parsed.GetError();
            return;
        }
        m_Guid   = parsed.GetValue().Guid;
        m_Source = parsed.GetValue().Source;
        m_Edit   = Edit{ parsed.GetValue().Class, parsed.GetValue().Loading };
        m_Saved  = m_Edit;
    }

    SoundDocument::~SoundDocument()
    {
        // The picks' undo records point at m_Edit.
        CommandHistory::Get().DropFor( &m_Edit );
    }

    bool SoundDocument::IsSubjectAlive() const
    {
        return SoundFileOf( Assets::AssetHandle( Subject().Owner ) ).has_value();
    }

    ISubjectDocument::DiskState SoundDocument::GetDiskState() const
    {
        if ( !m_LoadError.empty() )
            return DiskState::Untracked;
        return m_Edit != m_Saved ? DiskState::Dirty : DiskState::Clean;
    }

    bool SoundDocument::SaveDocument()
    {
        if ( !m_LoadError.empty() )
        {
            m_Refusal = std::format( "Save: the sound was not loaded: {}", m_LoadError );
            return false;
        }
        if ( auto saved = Assets::SoundAsset::Save( m_File, m_Guid, m_Source, m_Edit.Class, m_Edit.Loading );
             !saved )
        {
            m_Refusal = std::format( "Save: {}", saved.GetError() );
            LOG_ERROR( "[Audio] '{}' was not saved: {}", m_File.generic_string(), saved.GetError() );
            return false;
        }
        m_Saved = m_Edit;
        m_Refusal.clear();
        return true;
    }

    void SoundDocument::Choose( const Edit& next )
    {
        if ( next == m_Edit )
            return;
        const Edit                        previous = m_Edit;
        const CommandHistory::FieldObject owner( &m_Edit );
        CommandHistory::Get().Push( &m_Edit, &previous, &next, sizeof( Edit ) );
        m_Edit = next;
    }

    void SoundDocument::DrawClassPicker()
    {
        // The project's class set as the mix holds it: what SoundClassRegistry loaded from the class assets.
        const auto& mix = Audio::AudioMix::Get();
        if ( !m_Edit.Class.IsNull() && !mix.Has( m_Edit.Class ) )
            ImGui::TextColored( kRefusalColour, "Class %s is not in the mix: no voice of this sound will play.",
                                Common::Content::AssetGuidToText( m_Edit.Class ).c_str() );

        if ( ImGui::BeginCombo( "Class", ClassDisplayName( m_Edit.Class ).c_str() ) )
        {
            for ( const Audio::SoundClassDesc& desc : mix.Classes() )
            {
                const std::string name = ClassDisplayName( desc.Id );
                ImGui::PushID( static_cast<int>( desc.Id.Lo ^ desc.Id.Hi ) );
                if ( ImGui::Selectable( name.c_str(), desc.Id == m_Edit.Class ) )
                    Choose( Edit{ desc.Id, m_Edit.Loading } );
                ImGui::PopID();
            }
            if ( mix.Classes().empty() )
                ImGui::TextDisabled( "The project states no sound class asset." );
            ImGui::EndCombo();
        }
    }

    void SoundDocument::DrawLoadingPicker()
    {
        if ( ImGui::BeginCombo( "Loading", std::string( Audio::SoundLoadingName( m_Edit.Loading ) ).c_str() ) )
        {
            for ( const Audio::SoundLoading loading :
                  { Audio::SoundLoading::Resident, Audio::SoundLoading::Streamed } )
            {
                const std::string name( Audio::SoundLoadingName( loading ) );
                if ( ImGui::Selectable( name.c_str(), loading == m_Edit.Loading ) )
                    Choose( Edit{ m_Edit.Class, loading } );
            }
            ImGui::EndCombo();
        }
    }

    void SoundDocument::OnUIRender()
    {
        if ( !m_LoadError.empty() )
        {
            ImGui::TextColored( kRefusalColour, "Not loaded: %s", m_LoadError.c_str() );
            return;
        }

        ImGui::TextDisabled( "Sound" );
        ImGui::SameLine();
        if ( ImGui::SmallButton( "Save" ) )
            SaveDocument();
        if ( !m_Refusal.empty() )
            ImGui::TextColored( kRefusalColour, "%s", m_Refusal.c_str() );
        ImGui::Separator();

        ImGui::BeginDisabled();
        std::string source = m_Source;
        ImGui::InputText( "Source", source.data(), source.size() + 1, ImGuiInputTextFlags_ReadOnly );
        ImGui::EndDisabled();

        DrawLoadingPicker();
        DrawClassPicker();
    }

    SubjectEditorRegistry::PathOpenOutcome RequestSoundDocument( const std::string&           path,
                                                                 const SubjectEditorRegistry& editors )
    {
        using Outcome = SubjectEditorRegistry::PathOpenOutcome;
        std::error_code ec;
        if ( std::filesystem::path( path ).extension() != Assets::kSoundExtension ||
             !std::filesystem::exists( path, ec ) )
            return Outcome::NotMine;

        const auto parsed = Assets::SoundAsset::ReadFile( path );
        if ( !parsed )
        {
            LOG_ERROR( "[Audio] '{}' would not load - no editor was opened: {}", path, parsed.GetError() );
            return Outcome::Failed;
        }
        const Assets::AssetHandle handle(
             static_cast<uint64_t>( Common::Content::HandleForGuid( parsed.GetValue().Guid ) ) );
        if ( !SoundFileOf( handle ) )
        {
            LOG_ERROR( "[Audio] '{}' (GUID {}) is not listed by the content registry - no editor was opened.",
                       path, Common::Content::AssetGuidToText( parsed.GetValue().Guid ) );
            return Outcome::Failed;
        }
        if ( !editors.HasEditorFor( AssetSubjectType( kSoundType ) ) )
        {
            LOG_ERROR( "[Audio] '{}': the open register says a sound has an editor, and none is registered",
                       path );
            return Outcome::Failed;
        }
        Core::SubjectOpenRequests::Request( AssetSubject( handle, kSoundType ) );
        return Outcome::Requested;
    }
} // namespace Desert::Editor
