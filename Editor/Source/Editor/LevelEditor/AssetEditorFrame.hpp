#pragma once

namespace Desert::Assets
{
    struct AssetMetadata;
}

namespace Desert::Editor
{
    class ISubjectDocument;

    /**
     * @brief The frame every asset editor is drawn in: a toolbar on top, the editor's body, a status bar at the
     * bottom — UE's FAssetEditorToolkit (the toolbar with Save / Browse and the editor's extender) and the status
     * bar each asset-editor major tab carries.
     *
     * DRAWN BY THE DOCUMENT LOOP, NOT BY THE EDITORS (DocumentHost::DrawDocuments wraps OnUIRender for every
     * document whose subject is SubjectDomain::Asset). That is what makes it common: a new asset editor gets Save,
     * Browse and the status line by existing, and adds its own entries through ISubjectDocument::ExtendToolbar.
     *
     * THE BODY IS A CHILD REGION sized to what the bars leave, so an editor's GetContentRegionAvail() is the space
     * between them and no editor needs to know the bars exist.
     */
    namespace AssetEditorFrame
    {
        // Save (ISubjectDocument::SaveDocument), Browse (sync the Assets browser to the subject's file), a
        // separator, then the document's ExtendToolbar entries. `asset` may be null (an unregistered subject):
        // Browse is then disabled and says why.
        void DrawToolbar( ISubjectDocument& document, const Assets::AssetMetadata* asset );

        // The body region between the bars. EndBody is called whatever BeginBody returned (ImGui's child rule).
        bool BeginBody();
        void EndBody();

        // The subject's file, its saved state (GetDiskState) and the document's StatusText on the right.
        void DrawStatusBar( const ISubjectDocument& document, const Assets::AssetMetadata* asset );
    } // namespace AssetEditorFrame
} // namespace Desert::Editor
