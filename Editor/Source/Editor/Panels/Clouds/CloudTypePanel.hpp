#pragma once

#include "../IPanel.hpp"

#include <Engine/Assets/CloudTypeData.hpp>

#include <Common/Core/Core.hpp>

#include <string>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
    /**
     * @brief The artist's tool for a KIND of cloud: author it, look at its profile, save it as an asset.
     *
     * This is the half of the owner's request that the Details panel cannot be: a slot lets an artist
     * CHOOSE a cloud type, and this is where they MAKE one. Three things it offers that twelve sliders on
     * the component could not:
     *
     *   AUTHOR   the numbers and the vertical profile curve, validated as a set before anything is
     *            written, so an illegal type is
     *            refused with the number that is wrong rather than saved and discovered as a missing sky.
     *   SEE      the field those inputs actually produce, read down two vertical lines — at the rim of a
     *            placement patch and at its core. It is the only way to tell "this type is a tower in the
     *            middle of a patch" from "this type is flat everywhere" without rendering a frame, and it
     *            is deliberately NOT the profile curve played back: the curve is the input, and a preview
     *            that redrew the input would agree with itself no matter what the generator did with it.
     *   KEEP     save to a `.decloudtype`, which the Content Browser then lists and a cloud layer's slot
     *            then accepts. That round trip is the whole feature.
     *
     * NO BACKGROUND WORK, unlike the noise volume panel next door. The claim used to be about generating a
     * 16 384-texel profile table, which has not existed since phase Э5; what it costs now is placing one
     * preview region's lumps and reading the field down two vertical lines, which is a few thousand
     * evaluations. Still cheaper than the widgets around it, so it is redrawn every frame the panel is open.
     *
     * ONE WINDOW PER `.decloudtype`, OPENED BY DOUBLE-CLICKING THE ASSET — UE's flow, and what this class
     * became in Р3. It was a SINGLETON with an "Open a type..." combo, reached from the View menu; the
     * combo is gone with the singleton, because the subject is now the window's identity.
     */
    class CloudTypePanel final : public ISubjectDocument
    {
    public:
        CloudTypePanel( const Assets::AssetHandle& subject, Assets::AssetManager* assets );

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 520.0f, 760.0f };
        }

        void OnUIRender() override;

        // NEVER — see CloudNoiseVolumePanel::HoldsRendererSlot. This panel draws a curve with ImGui::PlotLines
        // and owns no Scene and no SceneRenderer, so it costs none of the six slots.

        // The `CloudType` this window is about, gone from the manager — deleted in the browser, or the project
        // closed under it. Asked of the metadata rather than of a typed lookup: the question is whether the
        // asset is still THERE, and a typed lookup answers a different one (whether it is still that type).
        // DEFINED IN THE .cpp: this header only forward-declares AssetManager, and pulling the whole of
        // it in for one metadata lookup would put the asset system into every translation unit that draws
        // a cloud panel.
        [[nodiscard]] bool IsSubjectAlive() const override;

        [[nodiscard]] bool HoldsRendererSlot() const override
        {
            return false;
        }

        // ...and never will — see CloudNoiseVolumePanel::ClaimsRendererSlot.
        [[nodiscard]] bool ClaimsRendererSlot() const override
        {
            return false;
        }

        // ── WHAT AN EDIT HAS REACHED ───────────────────────────────────────────────────────────────────
        //
        // WRITE-THROUGH IS THE FACT HERE, not a default standing in for one, and it is worth saying why it
        // differs from the Material Editor next door. A material has a WORKING copy and an APPLIED one
        // because the thing it edits is already in the level and an accidental drag would otherwise reach
        // every mesh using it. A cloud type reaches the sky only through the CloudTypeService, and the
        // service is re-registered by Save — so nothing this window changes is in any scene until the file
        // is written. There is no third state to stage, and offering Apply/Discard here would be two
        // buttons that do nothing (§1.3).
        //
        // WHICH MAKES THE DISK STATE THE ONLY QUESTION WORTH ASKING of this document, and it is the one
        // that was missing: the tab had no dirty dot, "Save All" could not see it, and a close threw the
        // edit away without asking. It is answered by comparing the working data against a COPY of what
        // was loaded or last written — not by a flag each edit path has to remember to set, because the
        // flag is the thing that falls behind (Assets::CloudTypeData::operator== is defaulted for exactly
        // this).
        [[nodiscard]] DiskState GetDiskState() const override;

        // Writes the working data to the SUBJECT'S OWN FILE. The same call the Save button makes — not a
        // second route to the same bytes, which would be the second execution path §1.3 forbids and would
        // drift the day somebody adds a step (the re-register, the re-read, the status line).
        bool SaveDocument() override;

        // ── THE NUMBERS A CLIENT CAN DRAG ──────────────────────────────────────────────────────────────
        //
        // The shape's own scalars, derived from CloudTypeShape rather than typed twice: the same table
        // drives the sliders this panel draws and the properties the control channel offers, so a channel
        // that could set a value the panel cannot show, or the reverse, is not expressible.
        [[nodiscard]] std::vector<EditableProperty> EditableProperties() const override;
        [[nodiscard]] Common::BoolResultStr         SetEditableProperty( const std::string&        name,
                                                                         const std::vector<float>& value ) override;

    private:
        void DrawLibrarySection();
        void DrawShapeSection();

        // The vertical profile, drawn as the cloud's own silhouette and edited by dragging it.
        //
        // ITS OWN METHOD AND NOT PART OF THE SHAPE SECTION, because it is the one control here that is a
        // CANVAS rather than a slider: it owns a hit region, a drag that runs across several samples, and
        // a set of presets. Folding it into the slider list would have buried the only authored thing on
        // this panel that is not a number.
        void DrawProfileEditor();

        void DrawNoiseSection();
        void DrawPreviewSection();
        void DrawSaveSection();

        // Loads @p path into the editing buffer. Separate from the combo that calls it so that "open" is
        // one operation whether it came from the list or from a save that has just happened.
        void OpenType( const Common::Filepath& path );

        Assets::AssetManager* m_Assets = nullptr;

        // Writes @p target and re-registers the asset. The body of the Save button, lifted out so that
        // SaveDocument (the command palette, "Save All", the control channel) and the button run the SAME
        // sequence. Returns whether the file was written; @p isCopy is a Save As to a different file,
        // which becomes its own document rather than repointing this one.
        bool WriteTo( const Common::Filepath& target, bool isCopy );

        Assets::CloudTypeData m_Data = Assets::CloudTypeDefault();
        // WHAT THE FILE HOLDS, as of the last open or the last write to this document's own file. The
        // other side of the comparison GetDiskState makes; a copy and not a hash, because CloudTypeData is
        // small and a defaulted operator== cannot fall behind the struct the way a digest of hand-listed
        // fields would.
        Assets::CloudTypeData m_OnDisk;
        // UNTRACKED IS NOT CLEAN. False until this document has actually read its subject off disk — a
        // window that fell back to the built-in default has no file to be clean against, and drawing "no
        // dot" for it would assert the file is up to date on no evidence (ISubjectDocument::DiskState).
        bool                  m_Tracked = false;
        Common::Filepath      m_SourcePath;                        // empty until saved or opened
        std::string           m_SourceName = "(built-in default)"; // what is in the buffer, for the header

        // The two text fields, kept as fixed buffers because ImGui::InputText writes into one and the
        // asset's own strings are std::optional<std::string>.
        char m_NameBuffer[128]  = {};
        char m_NotesBuffer[512] = {};

        std::string m_Status;
        bool        m_StatusIsError = false;

        // The preview's two curves — the profile at the rim of a placement patch and at its core. Members
        // rather than locals so the plot does not reallocate 512 floats every frame.
        std::vector<float> m_ProfileEdge;
        std::vector<float> m_ProfileCore;
    };
} // namespace Desert::Editor
