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
        switch ( navigation.TakePicker( picker ) )
        {
            case DetailsNavigation::PickerStep::None:
                return false;
            case DetailsNavigation::PickerStep::Scroll:
                ImGui::SetScrollHereY( 0.25f );
                return false;
            case DetailsNavigation::PickerStep::Open:
                // The row is where the scroll put it now, so the popup lands under it, inside the window.
                ImGui::SetNextWindowPos( ImVec2( ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y ) );
                return true;
        }
        return false;
    }
} // namespace Desert::Editor
