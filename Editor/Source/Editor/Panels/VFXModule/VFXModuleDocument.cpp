#include "VFXModuleDocument.hpp"

#include <Editor/Core/AssetOpen.hpp>
#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/Commands/VFXModuleEdit.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/SubjectTitle.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/VFXModuleAsset.hpp>
#include <Engine/VFX/VFXEmitterSpawn.hpp>
#include <Engine/VFX/VFXStackCompiler.hpp>

#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <filesystem>
#include <format>
#include <system_error>

namespace Desert::Editor
{
    namespace
    {
        using ValueType = Assets::Serialization::VFXValueType;

        constexpr std::array<ValueType, 6> kValueTypes = { ValueType::Float, ValueType::Vec2, ValueType::Vec3,
                                                           ValueType::Vec4,  ValueType::Int,  ValueType::Bool };

        constexpr std::array<VFX::VFXStage, 4> kStages = { VFX::VFXStage::EmitterSpawn, VFX::VFXStage::EmitterUpdate,
                                                           VFX::VFXStage::ParticleSpawn, VFX::VFXStage::ParticleUpdate };

        // ImGui's std::string binding (imgui_stdlib's InputText): the buffer grows with the text.
        int ResizeString( ImGuiInputTextCallbackData* data )
        {
            if ( data->EventFlag == ImGuiInputTextFlags_CallbackResize )
            {
                auto* text = static_cast<std::string*>( data->UserData );
                text->resize( static_cast<std::size_t>( data->BufTextLen ) );
                data->Buf = text->data();
            }
            return 0;
        }

        bool InputString( const char* label, std::string& text )
        {
            return ImGui::InputText( label, text.data(), text.capacity() + 1, ImGuiInputTextFlags_CallbackResize,
                                     ResizeString, &text );
        }

        bool InputCode( const char* label, std::string& text, const ImVec2& size )
        {
            return ImGui::InputTextMultiline(
                 label, text.data(), text.capacity() + 1, size,
                 ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_AllowTabInput, ResizeString, &text );
        }

        // The two declaration tables hold different rows (an input carries its default, VFXModuleInputDecl); the
        // table UI edits what they share, Name and Type.
        std::size_t CountOf( const VFX::VFXModuleDefinition& module, const bool inputs )
        {
            return inputs ? module.Block.Inputs.size() : module.Block.Attributes.size();
        }
        std::string& NameOf( VFX::VFXModuleDefinition& module, const bool inputs, const std::size_t i )
        {
            return inputs ? module.Block.Inputs[i].Name : module.Block.Attributes[i].Name;
        }
        ValueType& TypeOf( VFX::VFXModuleDefinition& module, const bool inputs, const std::size_t i )
        {
            return inputs ? module.Block.Inputs[i].Type : module.Block.Attributes[i].Type;
        }
        void EraseAt( VFX::VFXModuleDefinition& module, const bool inputs, const std::size_t i )
        {
            if ( inputs )
                module.Block.Inputs.erase( module.Block.Inputs.begin() + static_cast<std::ptrdiff_t>( i ) );
            else
                module.Block.Attributes.erase( module.Block.Attributes.begin() + static_cast<std::ptrdiff_t>( i ) );
        }
        bool Declares( VFX::VFXModuleDefinition& module, const bool inputs, const std::string& name )
        {
            for ( std::size_t i = 0; i < CountOf( module, inputs ); ++i )
                if ( NameOf( module, inputs, i ) == name )
                    return true;
            return false;
        }

        /// The module compiled in a one-emitter stack, in each GPU group it states, its inputs given as zero
        /// Values: the declaration, attribute and stage errors the particle world would refuse it with.
        std::string StackCheck( const VFX::VFXModuleDefinition& module, const std::filesystem::path& file )
        {
            if ( !module.HasParticleStage() )
                return VFX::HasCpuBehaviour( module.Guid )
                            ? std::string(
                                   "Runs on the CPU only (Emitter groups): the engine's C++ for this module "
                                   "runs it; there is no GLSL to check." )
                            : std::format(
                                   "No GLSL to check, and the engine has no C++ for GUID {}: a stack using "
                                   "this module in EmitterUpdate is refused.",
                                   Common::Content::AssetGuidToText( module.Guid ) );
            VFX::VFXModuleLibrary library;
            library.Set( module, file.generic_string() );

            namespace S = Assets::Serialization;
            S::VFXModuleUse use;
            use.Module = { Common::Content::AssetGuidToText( module.Guid ), file.generic_string() };
            if ( !module.HasCpuStage() )
                for ( const VFX::VFXModuleInputDecl& in : module.Block.Inputs )
                {
                    S::VFXModuleInput value;
                    value.Name   = in.Name;
                    value.Type   = in.Type;
                    value.Source = S::VFXInputSource::Value;
                    value.Value  = glm::vec4( 0.0f );
                    use.Inputs.push_back( std::move( value ) );
                }
            S::VFXSystemData  system;
            S::VFXEmitterData emitter;
            emitter.Name = "Check";
            if ( module.AllowsStage( VFX::VFXStage::ParticleSpawn ) )
                emitter.Stack.ParticleSpawn.push_back( use );
            if ( module.AllowsStage( VFX::VFXStage::ParticleUpdate ) )
                emitter.Stack.ParticleUpdate.push_back( use );
            system.Emitters.push_back( std::move( emitter ) );
            const auto compiled = VFX::CompileEmitterStack( system, 0, library );
            return compiled ? std::format( "The stack compiles ({} attribute(s), {} parameter row(s)).",
                                           compiled.GetValue().Layout.Attributes.size(),
                                           compiled.GetValue().Slots.size() )
                            : compiled.GetError();
        }
    } // namespace

    VFXModuleDocument::VFXModuleDocument( const Assets::AssetHandle& subject, Assets::AssetManager* assets )
         : ISubjectDocument( AssetSubjectTitle( subject, assets, "VFX Module" ),
                             AssetSubject( subject, static_cast<uint32_t>( Assets::AssetTypeID::VFXModule ) ) ),
           m_Assets( assets )
    {
        const auto asset =
             m_Assets != nullptr ? m_Assets->FindByHandle<Assets::VFXModuleAsset>( subject ) : nullptr;
        if ( !asset )
        {
            m_LoadError = "the module asset is not held by the asset manager";
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
        m_File    = asset->GetMetadata().Filepath;
        m_Working = asset->GetDefinition();
        m_Saved   = m_Working;
        m_Loaded  = true;
        m_UsedBy  = Assets::ContentRegistry::Referrers( m_File );
        SyncDrafts();
    }

    VFXModuleDocument::~VFXModuleDocument()
    {
        CommandHistory::Get().DropFor( &m_Working );
    }

    bool VFXModuleDocument::IsSubjectAlive() const
    {
        return m_Assets != nullptr &&
               m_Assets->FindMetadataByHandle( Assets::AssetHandle( Subject().Owner ) ) != nullptr;
    }

    ISubjectDocument::DiskState VFXModuleDocument::GetDiskState() const
    {
        if ( !m_Loaded )
            return DiskState::Untracked;
        return m_Working != m_Saved ? DiskState::Dirty : DiskState::Clean;
    }

    bool VFXModuleDocument::SaveDocument()
    {
        if ( !m_Loaded || m_Assets == nullptr )
            return false;
        const Assets::AssetHandle handle( Subject().Owner );
        if ( auto saved = Assets::VFXModuleAsset::Save( m_File, m_Working ); !saved )
        {
            m_Refusal = std::format( "Save: {}", saved.GetError() );
            LOG_ERROR( "[VFX] '{}' was not saved: {}", m_File.string(), saved.GetError() );
            return false;
        }
        m_Saved = m_Working;
        m_Refusal.clear();
        if ( const auto asset = m_Assets->FindByHandle<Assets::VFXModuleAsset>( handle ) )
        {
            if ( auto reloaded = asset->LoadFromFile(); !reloaded )
                LOG_ERROR( "[VFX] '{}' was saved but would not be read back: {}", m_File.string(),
                           reloaded.GetError() );
        }
        m_UsedBy = Assets::ContentRegistry::Referrers( m_File );
        return true;
    }

    void VFXModuleDocument::Report( const Common::BoolResultStr& result )
    {
        if ( result.IsSuccess() )
            m_Refusal.clear();
        else
            m_Refusal = result.GetError();
    }

    void VFXModuleDocument::Edit( const std::string&                                                       label,
                                  const std::function<Common::BoolResultStr( VFX::VFXModuleDefinition& )>& edit )
    {
        Report( ApplyVFXModuleEdit( m_Working, CommandHistory::Get(), label, edit ) );
    }

    void VFXModuleDocument::SyncDrafts()
    {
        m_DraftsOf           = m_Working;
        m_Drafts.Name        = m_Working.DisplayName;
        m_Drafts.Category    = m_Working.Category;
        m_Drafts.Description = m_Working.Description;
        m_Drafts.Body        = m_Working.Block.Body;
        m_Drafts.Inputs.clear();
        for ( const VFX::VFXModuleInputDecl& d : m_Working.Block.Inputs )
            m_Drafts.Inputs.push_back( d.Name );
        m_Drafts.Attributes.clear();
        for ( const VFX::VFXModuleDecl& d : m_Working.Block.Attributes )
            m_Drafts.Attributes.push_back( d.Name );
    }

    void VFXModuleDocument::DrawMetadata()
    {
        const auto text =
             [this]( const char* label, std::string& draft, std::string VFX::VFXModuleDefinition::*field )
        {
            InputString( label, draft );
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                Edit( std::format( "Set {}", label ),
                      [&]( VFX::VFXModuleDefinition& m )
                      {
                          m.*field = draft;
                          return BOOLSUCCESS;
                      } );
        };
        text( "Name", m_Drafts.Name, &VFX::VFXModuleDefinition::DisplayName );
        text( "Category", m_Drafts.Category, &VFX::VFXModuleDefinition::Category );
        text( "Description", m_Drafts.Description, &VFX::VFXModuleDefinition::Description );

        ImGui::TextUnformatted( "Usage" );
        for ( const VFX::VFXStage stage : kStages )
        {
            ImGui::SameLine();
            bool       on   = m_Working.AllowsStage( stage );
            const auto name = std::string( VFX::StageName( stage ) );
            if ( ImGui::Checkbox( name.c_str(), &on ) )
                Edit( std::format( "{} stage {}", on ? "Allow" : "Disallow", name ),
                      [stage, on]( VFX::VFXModuleDefinition& m )
                      {
                          const auto bit = VFX::UsageBit( stage );
                          m.Block.Usage  = on ? static_cast<uint8_t>( m.Block.Usage | bit )
                                              : static_cast<uint8_t>( m.Block.Usage & ~bit );
                          return BOOLSUCCESS;
                      } );
        }
        ImGui::TextDisabled( "GUID %s", Common::Content::AssetGuidToText( m_Working.Guid ).c_str() );
    }

    void VFXModuleDocument::DrawDeclarations( const char* id, const bool inputs )
    {
        const std::size_t         count  = CountOf( m_Working, inputs );
        std::vector<std::string>& drafts = inputs ? m_Drafts.Inputs : m_Drafts.Attributes;
        const char*               what   = inputs ? "input" : "attribute";

        constexpr ImGuiTableFlags kTable = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg;
        if ( !ImGui::BeginTable( id, 3, kTable ) )
            return;
        ImGui::TableSetupColumn( "Name", ImGuiTableColumnFlags_WidthStretch );
        ImGui::TableSetupColumn( "Type", ImGuiTableColumnFlags_WidthFixed, 110.0f );
        ImGui::TableSetupColumn( "", ImGuiTableColumnFlags_WidthFixed );
        ImGui::TableHeadersRow();
        std::size_t remove = count;
        for ( std::size_t i = 0; i < count && i < drafts.size(); ++i )
        {
            ImGui::PushID( static_cast<int>( i ) );
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth( -FLT_MIN );
            InputString( "##name", drafts[i] );
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                Edit( std::format( "Rename {} {}", what, NameOf( m_Working, inputs, i ) ),
                      [&, i]( VFX::VFXModuleDefinition& m )
                      {
                          NameOf( m, inputs, i ) = drafts[i];
                          return BOOLSUCCESS;
                      } );
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth( -FLT_MIN );
            const ValueType   currentType = TypeOf( m_Working, inputs, i );
            const std::string current( VFX::GlslTypeName( currentType ) );
            if ( ImGui::BeginCombo( "##type", current.c_str() ) )
            {
                for ( const ValueType type : kValueTypes )
                {
                    const std::string name( VFX::GlslTypeName( type ) );
                    if ( ImGui::Selectable( name.c_str(), type == currentType ) && type != currentType )
                        Edit( std::format( "Retype {} {}", what, NameOf( m_Working, inputs, i ) ),
                              [&, i, type]( VFX::VFXModuleDefinition& m )
                              {
                                  TypeOf( m, inputs, i ) = type;
                                  return BOOLSUCCESS;
                              } );
                }
                ImGui::EndCombo();
            }
            ImGui::TableNextColumn();
            if ( ImGui::SmallButton( ICON_MDI_CLOSE ) )
                remove = i;
            ImGui::PopID();
        }
        ImGui::EndTable();
        if ( remove < count )
            Edit( std::format( "Remove {} {}", what, NameOf( m_Working, inputs, remove ) ),
                  [&, remove]( VFX::VFXModuleDefinition& m )
                  {
                      EraseAt( m, inputs, remove );
                      return BOOLSUCCESS;
                  } );
        if ( ImGui::SmallButton( inputs ? ICON_MDI_PLUS " Add input" : ICON_MDI_PLUS " Add attribute" ) )
            Edit( std::format( "Add {}", what ),
                  [&]( VFX::VFXModuleDefinition& m )
                  {
                      std::string name = inputs ? "NewInput" : "NewAttribute";
                      for ( int n = 1; Declares( m, inputs, name ); ++n )
                          name = std::format( "{}{}", inputs ? "NewInput" : "NewAttribute", n );
                      if ( inputs )
                          m.Block.Inputs.push_back( { name, ValueType::Float } );
                      else
                          m.Block.Attributes.push_back( { name, ValueType::Float } );
                      return BOOLSUCCESS;
                  } );
    }

    void VFXModuleDocument::DrawCode()
    {
        ImGui::TextDisabled( "void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )" );
        InputCode( "##glsl", m_Drafts.Body, ImVec2( -FLT_MIN, ImGui::GetTextLineHeight() * 18.0f ) );
        if ( ImGui::IsItemDeactivatedAfterEdit() )
            Edit( "Edit GLSL",
                  [this]( VFX::VFXModuleDefinition& m )
                  {
                      m.Block.Body = m_Drafts.Body;
                      if ( !m.Block.Body.empty() && !m.Block.Body.ends_with( '\n' ) )
                          m.Block.Body += '\n'; // the block's closing brace starts its own line
                      return BOOLSUCCESS;
                  } );
    }

    void VFXModuleDocument::DrawStackCheck()
    {
        if ( m_Working != m_CheckedOf || m_Check.empty() )
        {
            m_CheckedOf = m_Working;
            m_Check     = StackCheck( m_Working, m_File );
        }
        ImGui::TextWrapped( "%s", m_Check.c_str() );
    }

    void VFXModuleDocument::DrawUsedBy()
    {
        if ( m_UsedBy.empty() )
        {
            ImGui::TextDisabled( "No asset uses this module." );
            return;
        }
        for ( const std::string& key : m_UsedBy )
            ImGui::BulletText( "%s", key.c_str() );
    }

    void VFXModuleDocument::OnUIRender()
    {
        if ( !m_Loaded )
        {
            ImGui::TextWrapped( "This module could not be read: %s", m_LoadError.c_str() );
            return;
        }
        if ( m_Working != m_DraftsOf )
            SyncDrafts();

        if ( ImGui::Button( ICON_MDI_CONTENT_SAVE " Save" ) )
            (void)SaveDocument();
        if ( !m_Refusal.empty() )
            ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ), "%s", m_Refusal.c_str() );

        DrawMetadata();
        if ( ImGui::CollapsingHeader( "Inputs", ImGuiTreeNodeFlags_DefaultOpen ) )
            DrawDeclarations( "##inputs", true );
        if ( ImGui::CollapsingHeader( "Attributes", ImGuiTreeNodeFlags_DefaultOpen ) )
            DrawDeclarations( "##attributes", false );
        if ( m_Working.HasParticleStage() && ImGui::CollapsingHeader( "GLSL", ImGuiTreeNodeFlags_DefaultOpen ) )
            DrawCode();
        if ( ImGui::CollapsingHeader( "Stack check", ImGuiTreeNodeFlags_DefaultOpen ) )
            DrawStackCheck();
        if ( ImGui::CollapsingHeader( "Used by" ) )
            DrawUsedBy();
    }

    SubjectEditorRegistry::PathOpenOutcome RequestVFXModuleDocument( Assets::AssetManager*        assets,
                                                                     const std::string&           path,
                                                                     const SubjectEditorRegistry& editors )
    {
        using Outcome = SubjectEditorRegistry::PathOpenOutcome;
        std::error_code ec;
        if ( assets == nullptr || std::filesystem::path( path ).extension() != VFX::kVFXModuleExtension ||
             !std::filesystem::exists( path, ec ) )
            return Outcome::NotMine;

        auto asset = assets->FindByPath<Assets::VFXModuleAsset>( path );
        if ( !asset )
            asset = assets->CreateAsset<Assets::VFXModuleAsset>( path );
        if ( !asset )
        {
            LOG_ERROR( "[VFX] '{}' could not be registered as a VFX module - no editor was opened.", path );
            return Outcome::Failed;
        }
        if ( !asset->IsReadyForUse() )
        {
            if ( const auto loaded = asset->LoadFromFile(); !loaded )
            {
                LOG_ERROR( "[VFX] '{}' would not load as a VFX module - no editor was opened: {}", path,
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
