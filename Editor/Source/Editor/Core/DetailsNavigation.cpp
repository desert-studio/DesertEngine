#include <Editor/Core/DetailsNavigation.hpp>

#include <ImGui/imgui.h>

namespace Desert::Editor
{
    DetailsNavigation& GetDetailsNavigation()
    {
        static DetailsNavigation navigation;
        return navigation;
    }

    void MarkDetailsField( std::string_view field )
    {
        DetailsNavigation& navigation = GetDetailsNavigation();
        const std::string  label      = navigation.FieldLabel( field );
        navigation.NoteField( field );
        // A quarter down the panel rather than the top edge: the row above it stays visible, so a frame
        // shows which section the field belongs to.
        if ( navigation.TakeReveal( label ) )
            ImGui::SetScrollHereY( 0.25f );
    }

    bool TakeDetailsPickerRequest( std::string_view picker )
    {
        DetailsNavigation& navigation = GetDetailsNavigation();
        navigation.NotePicker( picker );
        if ( !navigation.TakePicker( picker ) )
            return false;
        ImGui::SetScrollHereY( 0.25f );
        ImGui::SetNextWindowPos( ImVec2( ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y ) );
        return true;
    }
} // namespace Desert::Editor
