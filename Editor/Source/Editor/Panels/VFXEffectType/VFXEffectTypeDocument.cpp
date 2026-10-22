#include "VFXEffectTypeDocument.hpp"

#include <Editor/Core/AssetOpen.hpp>
#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/SubjectTitle.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/VFXEffectTypeAsset.hpp>

#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <array>
#include <filesystem>
#include <format>
#include <memory>
#include <system_error>
#include <utility>

namespace Desert::Editor
{
    namespace
    {
        using Levels = std::vector<Assets::Serialization::VFXEffectTypeLevel>;

        // The Effects.QualityLevel names, Low .. Cinematic (Scalability.json's Effects rows).
        constexpr std::array<const char*, Assets::Serialization::kVFXEffectTypeLevelCount> kLevelNames = {
             "Low", "Medium", "High", "Epic", "Cinematic" };

        /// One undo step of a type's levels: the whole list before and after.
        class VFXEffectTypeEditCommand final : public ICommand
        {
        public:
            VFXEffectTypeEditCommand( Assets::Serialization::VFXEffectTypeData* target, Levels before, Levels after,
                                      std::string label )
                 : m_Target( target ), m_Before( std::move( before ) ), m_After( std::move( after ) ),
                   m_Label( std::move( label ) )
            {
            }

            bool Undo() override
            {
                m_Target->Levels = m_Before;
                return true;
            }

            bool Redo() override
            {
                m_Target->Levels = m_After;
                return true;
            }

            [[nodiscard]] const void* EditedObject() const override
            {
                return m_Target;
            }

            std::string GetLabel() const override
            {
                return m_Label;
            }

        private:
            Assets::Serialization::VFXEffectTypeData* m_Target;
            Levels                                    m_Before;
            Levels                                    m_After;
            std::string                               m_Label;
        };
    } // namespace

    VFXEffectTypeDocument::VFXEffectTypeDocument( const Assets::AssetHandle& subject, Assets::AssetManager* assets )
         : ISubjectDocument(
                AssetSubjectTitle( subject, assets, "VFX Effect Type" ),
                AssetSubject( subject, static_cast<uint32_t>( Assets::AssetTypeID::VFXEffectType ) ) ),
           m_Assets( assets )
    {
        const auto asset =
             m_Assets != nullptr ? m_Assets->FindByHandle<Assets::VFXEffectTypeAsset>( subject ) : nullptr;
        if ( !asset )
        {
            m_LoadError = "the effect type asset is not held by the asset manager";
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

    VFXEffectTypeDocument::~VFXEffectTypeDocument()
    {
        // The history's entries point at m_Working, which dies with this window.
        CommandHistory::Get().DropFor( &m_Working );
    }

    bool VFXEffectTypeDocument::IsSubjectAlive() const
    {
        return m_Assets != nullptr &&
               m_Assets->FindMetadataByHandle( Assets::AssetHandle( Subject().Owner ) ) != nullptr;
    }

    ISubjectDocument::DiskState VFXEffectTypeDocument::GetDiskState() const
    {
        if ( !m_Loaded )
            return DiskState::Untracked;
        return m_Working.Levels != m_Saved.Levels ? DiskState::Dirty : DiskState::Clean;
    }

    bool VFXEffectTypeDocument::SaveDocument()
    {
        if ( !m_Loaded || m_Assets == nullptr )
            return false;
        const Assets::AssetHandle handle( Subject().Owner );
        const auto*               meta = m_Assets->FindMetadataByHandle( handle );
        if ( meta == nullptr )
        {
            m_Refusal = "Save: the effect type asset is no longer held by the asset manager";
            return false;
        }
        if ( auto saved = Assets::VFXEffectTypeAsset::Save( meta->Filepath, m_Working ); !saved )
        {
            m_Refusal = std::format( "Save: {}", saved.GetError() );
            LOG_ERROR( "[VFX] '{}' was not saved: {}", meta->Filepath.string(), saved.GetError() );
            return false;
        }
        m_Saved = m_Working;
        m_Refusal.clear();
        // The held asset re-reads what was written, so every VFXWorld's next tick reads the new levels.
        if ( const auto asset = m_Assets->FindByHandle<Assets::VFXEffectTypeAsset>( handle ) )
        {
            if ( auto reloaded = asset->LoadFromFile(); !reloaded )
                LOG_ERROR( "[VFX] '{}' was saved but would not be read back: {}", meta->Filepath.string(),
                           reloaded.GetError() );
        }
        return true;
    }

    void VFXEffectTypeDocument::TrackDrag( const char* label )
    {
        if ( ImGui::IsItemActivated() )
            m_DragBefore = m_Working.Levels;
        if ( !ImGui::IsItemDeactivatedAfterEdit() )
            return;
        if ( auto valid = Assets::Serialization::ValidateVFXEffectTypeData( m_Working ); !valid )
        {
            m_Refusal        = std::format( "{}: {}", label, valid.GetError() );
            m_Working.Levels = m_DragBefore;
            return;
        }
        m_Refusal.clear();
        if ( m_Working.Levels == m_DragBefore )
            return;
        CommandHistory::Get().PushCommand( std::make_unique<VFXEffectTypeEditCommand>(
             &m_Working, std::move( m_DragBefore ), m_Working.Levels, label ) );
        m_DragBefore.clear();
    }

    void VFXEffectTypeDocument::OnUIRender()
    {
        if ( !m_Loaded )
        {
            ImGui::TextWrapped( "This effect type could not be read: %s", m_LoadError.c_str() );
            return;
        }

        if ( ImGui::Button( ICON_MDI_CONTENT_SAVE " Save" ) )
            (void)SaveDocument();
        ImGui::SameLine();
        ImGui::TextDisabled( "Per Effects quality level (Effects.QualityLevel)" );
        if ( !m_Refusal.empty() )
            ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ), "%s", m_Refusal.c_str() );

        constexpr ImGuiTableFlags kTable =
             ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
        if ( !ImGui::BeginTable( "##levels", 3, kTable ) )
            return;
        ImGui::TableSetupColumn( "Level", ImGuiTableColumnFlags_WidthFixed, 90.0f );
        ImGui::TableSetupColumn( "Max Distance (cm, 0 = never culled)" );
        ImGui::TableSetupColumn( "Spawn Count Scale" );
        ImGui::TableHeadersRow();
        for ( std::size_t i = 0; i < m_Working.Levels.size() && i < kLevelNames.size(); ++i )
        {
            auto& level = m_Working.Levels[i];
            ImGui::PushID( static_cast<int>( i ) );
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted( kLevelNames[i] );
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth( -1.0f );
            ImGui::DragFloat( "##distance", &level.MaxDistance, 10.0f, 0.0f, 1.0e7f, "%.0f cm",
                              ImGuiSliderFlags_AlwaysClamp );
            TrackDrag( "Max Distance" );
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth( -1.0f );
            ImGui::DragFloat( "##scale", &level.SpawnCountScale, 0.005f, 0.0f, 1.0f, "%.3f",
                              ImGuiSliderFlags_AlwaysClamp );
            TrackDrag( "Spawn Count Scale" );
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    SubjectEditorRegistry::PathOpenOutcome RequestVFXEffectTypeDocument( Assets::AssetManager*        assets,
                                                                         const std::string&           path,
                                                                         const SubjectEditorRegistry& editors )
    {
        using Outcome = SubjectEditorRegistry::PathOpenOutcome;
        std::error_code ec;
        if ( assets == nullptr ||
             std::filesystem::path( path ).extension() != Assets::Serialization::kVFXEffectTypeExtension ||
             !std::filesystem::exists( path, ec ) )
            return Outcome::NotMine;

        auto asset = assets->FindByPath<Assets::VFXEffectTypeAsset>( path );
        if ( !asset )
            asset = assets->CreateAsset<Assets::VFXEffectTypeAsset>( path );
        if ( !asset )
        {
            LOG_ERROR( "[VFX] '{}' could not be registered as an effect type - no editor was opened.", path );
            return Outcome::Failed;
        }
        if ( !asset->IsReadyForUse() )
        {
            if ( const auto loaded = asset->LoadFromFile(); !loaded )
            {
                LOG_ERROR( "[VFX] '{}' would not load as an effect type - no editor was opened: {}", path,
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
