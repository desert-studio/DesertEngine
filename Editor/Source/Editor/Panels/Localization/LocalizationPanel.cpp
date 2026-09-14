#include "LocalizationPanel.hpp"

#include <Editor/Core/ToastManager.hpp>

#include <Engine/Localization/LocalizationService.hpp>

#include <ImGui/imgui.h>

#include <map>
#include <string>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    LocalizationPanel::LocalizationPanel() : IPanel( "Localization", /*showPanel=*/false )
    {
    }

    void LocalizationPanel::OnUIRender()
    {
        if ( !GetVisibility() )
        {
            return;
        }

        auto& loc = Localization::Localization::Get();

        ImGui::Begin( GetName().c_str(), &GetVisibility() );

        // --- The language ---------------------------------------------------------------------------
        // EVERY language the build knows is offered, not only the ones this project has strings in. That
        // is the same decision the palette's Language entries take and for the same reason: switching to
        // a language with no translations is how an author SEES the hole, and hiding the entry would hide
        // it.
        const Localization::LocaleRow& current = loc.Language();
        if ( ImGui::BeginCombo( "Language",
                                ( std::string( current.Tag ) + " - " + std::string( current.Endonym ) ).c_str() ) )
        {
            for ( const Localization::LocaleRow& row : Localization::Locales() )
            {
                const std::string label = std::string( row.Tag ) + " - " + std::string( row.Endonym );
                if ( ImGui::Selectable( label.c_str(), &row == &current ) )
                {
                    if ( const auto set = loc.SetLanguage( row.Tag ); !set )
                        ToastManager::Push( set.GetError(), ToastLevel::Error );
                }
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        ImGui::TextDisabled( "generation %u", loc.Generation() );

        const std::vector<std::string> tables = loc.TableIds();
        if ( tables.empty() )
        {
            // NOT an empty list drawn as if everything were fine. A project with no tables is a project
            // whose UI is all literals, and the panel says so rather than showing a blank grid the reader
            // has to interpret.
            ImGui::Separator();
            ImGui::TextWrapped( "No string tables are loaded. Put a .destrings file under "
                                "Assets/Localization/ and every '#key' in the scene will resolve." );
            ImGui::End();
            return;
        }

        ImGui::Separator();
        ImGui::Text( "%zu table(s):", tables.size() );
        for ( const std::string& id : tables )
            ImGui::BulletText( "%s", id.c_str() );

        // --- Every key, and what it resolves to RIGHT NOW --------------------------------------------
        // Resolved through the same function the canvas draws with, so what is listed here is what is on
        // screen — a second lookup written for the panel would be a second answer to one question.
        ImGui::Separator();

        // STRUCTURAL, not a resolve. Asking Resolve for every key merely to LIST it would push a miss
        // into the session register for each hole and log it — the panel would be manufacturing the
        // reports it is supposed to show. So it reads the row and asks whether this language is in it.
        std::size_t       missing = 0;
        const std::string language( current.Tag );

        if ( ImGui::BeginTable( "##loc_keys", 2,
                                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                     ImGuiTableFlags_Resizable ) )
        {
            ImGui::TableSetupColumn( "Key", ImGuiTableColumnFlags_WidthFixed, 220.0F );
            ImGui::TableSetupColumn( "In this language" );
            ImGui::TableSetupScrollFreeze( 0, 1 );
            ImGui::TableHeadersRow();

            for ( const std::string& key : loc.Keys() )
            {
                const Localization::LocalizedEntry*       entry = loc.Find( key );
                const std::map<std::string, std::string>* forms = nullptr;
                if ( entry != nullptr )
                {
                    const auto found = entry->Forms.find( language );
                    if ( found != entry->Forms.end() )
                        forms = &found->second;
                }

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex( 0 );
                ImGui::TextUnformatted( key.c_str() );
                ImGui::TableSetColumnIndex( 1 );

                if ( forms == nullptr )
                {
                    ++missing;
                    ImGui::TextColored( ImVec4( 1.0f, 0.42f, 0.35f, 1.0f ), "MISSING - draws as \"#%s\"",
                                        key.c_str() );
                    continue;
                }

                // The `other` form is what a caller with no count and no gender gets, and the parser
                // guarantees every language has one — so it is the honest one-line preview of this row.
                // A row with more forms says how many, because "5 файлов" is not visible from "{n} файла".
                const auto        other = forms->find( "other" );
                const std::string preview =
                     other == forms->end() ? std::string( "<no 'other' form>" ) : other->second;
                if ( forms->size() == 1 )
                {
                    ImGui::TextUnformatted( preview.c_str() );
                }
                else
                {
                    ImGui::Text( "%s", preview.c_str() );
                    ImGui::SameLine();
                    ImGui::TextDisabled( "(+%zu forms)", forms->size() - 1 );
                }
            }
            ImGui::EndTable();
        }

        ImGui::Separator();
        if ( missing == 0 )
            ImGui::Text( "%zu keys, all translated into %s.", loc.Keys().size(),
                         std::string( current.Endonym ).c_str() );
        else
            ImGui::TextColored( ImVec4( 1.0f, 0.42f, 0.35f, 1.0f ), "%zu of %zu keys have no %s text.", missing,
                                loc.Keys().size(), std::string( current.Tag ).c_str() );

        ImGui::End();
    }
} // namespace Desert::Editor
