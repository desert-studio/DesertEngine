#pragma once

#include "../IPanel.hpp"

#include <Engine/Assets/CloudNoiseVolume.hpp>

#include <atomic>
#include <filesystem>
#include <future>
#include <memory>
#include <string>

namespace Desert::Assets
{
    class AssetManager;
}
namespace Desert::Graphic
{
    class Image2D;
}

namespace Desert::Editor
{
    /**
     * @brief The artist's tool for the volumetric clouds' 3D noise: author it, look at it, save it.
     *
     * Three things a volume that only ever existed on the GPU could not offer, and the reason this panel
     * exists rather than four sliders on the component:
     *
     *   AUTHOR   the seed, the curl strength and the four lattice periods, validated against the resolution
     *            before anything is baked, so an illegal set is refused with the number that is wrong
     *            rather than producing a volume with a seam in it.
     *   SEE      a 3D field has no picture. It has SLICES, so the panel shows one at a time, along any
     *            axis, either as all four channels at once or one channel on its own — which is the only
     *            way to tell "the billowy channel is empty" from "the wispy channel is too strong".
     *   KEEP     bake, then save to a `.dcnv`, which the Content Browser then lists and the cloud
     *            component's slot then accepts. That round trip is the whole feature.
     *
     * THE BAKE RUNS OFF THE UI THREAD. 128^3 is 8.4 million samples and measures 9.6 s optimised and 82 s
     * unoptimised on the machine this was written on; blocking the editor for that would make the tool feel
     * broken the first time it was used. The generator writes a progress fraction that the panel reads each
     * frame, and the panel refuses to start a second bake while one is running.
     *
     * ONE WINDOW PER `.dcnv`, OPENED BY DOUBLE-CLICKING THE ASSET — UE's flow, and what this class became
     * in Р3. It was a SINGLETON with an "Open" combo in it: one window, whichever volume was last picked in
     * it, reached from the View menu. The combo is gone with the singleton — the subject is fixed at
     * construction, two volumes are two windows, and the browser is where a volume is chosen. See
     * Editor/Core/SubjectEditorRegistry.hpp for the seam and Clouds/CloudDocumentOpen.hpp for the resolution.
     */
    class CloudNoiseVolumePanel final : public ISubjectDocument
    {
    public:
        CloudNoiseVolumePanel( const Assets::AssetHandle& subject, Assets::AssetManager* assets );
        ~CloudNoiseVolumePanel() override;

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 560.0f, 720.0f };
        }

        void OnUIRender() override;

        // NEVER, and this is measured rather than assumed. This panel owns no PreviewViewport, no Scene and
        // no SceneRenderer: it bakes on the CPU and uploads the result as a single Graphic::Image2D, which
        // is a device image and not a renderer. So the six-slot census (EditorLayer::RendererSlotCensus)
        // counts this document at zero for its whole life, and closing it returns nothing because it took
        // nothing. The same is true of the other three cloud documents.

        // The `CloudNoiseVolume` this window is about, gone from the manager — deleted in the browser, or the
        // project closed under it. Asked of the metadata rather than of a typed lookup: the question is whether
        // the asset is still THERE, and a typed lookup answers a different one (whether it is still that type).
        // DEFINED IN THE .cpp: this header only forward-declares AssetManager, and pulling the whole of
        // it in for one metadata lookup would put the asset system into every translation unit that draws
        // a cloud panel.
        [[nodiscard]] bool IsSubjectAlive() const override;

        [[nodiscard]] bool HoldsView() const override
        {
            return false;
        }

        // ...and never will, which is the half the SLOT CENSUS needs. Without it an open cloud document
        // counts as a claim that has not landed yet, and five of them would exhaust the cap on paper.
        [[nodiscard]] bool ClaimsView() const override
        {
            return false;
        }

        // ── THE DOCUMENT SURFACE ───────────────────────────────────────────────────────────────────────
        //
        // See the note beside m_VolumeRevision for what "dirty" means for a `.dcnv`.
        [[nodiscard]] DiskState GetDiskState() const override;
        bool                    SaveDocument() override;

        /// The recipe's own numbers. Their group says what a client has to know and the type cannot: they
        /// take effect at the next BAKE, not when they are set — a `.dcnv` holds voxels, and a recipe
        /// nobody has baked has changed nothing about the sky or about the file.
        [[nodiscard]] std::vector<EditableProperty> EditableProperties() const override;
        [[nodiscard]] Common::BoolResultStr         SetEditableProperty( const std::string&        name,
                                                                         const std::vector<float>& value ) override;

    private:
        // Reads the subject out of the AssetManager into the editing buffer. Called once, from the
        // constructor: the subject cannot change, so neither can the answer.
        void LoadSubject( Assets::AssetManager* assets );

        // Writes @p target and re-registers the asset. The body of the Save button, lifted out so that
        // SaveDocument (the palette, "Save All", the control channel) runs the SAME sequence rather than a
        // second route to the same bytes.
        bool WriteTo( const std::filesystem::path& target, bool isCopy );

        // THE ONE PLACE m_Volume IS REPLACED. Three callers -- the load, a finished bake and a sheet
        // import -- and it exists so that "the voxels changed" is counted rather than remembered: an 8 MiB
        // buffer cannot be compared against a snapshot every frame, and a dirty flag written at three
        // sites is a flag that will one day be written at two.
        void AdoptVolume( Assets::CloudNoiseVolumeData&& volume );

        void DrawGenerateSection();
        void DrawPreviewSection();
        void DrawSaveSection();

        // Import and export through the flat tiled slice sheet — the round trip out to whatever tool the
        // artist owns and back. Its own section rather than two more buttons under Save, because a sheet is
        // NOT a `.dcnv` and the difference matters: exporting drops the recipe, and the section says so
        // where the button is rather than in a log line nobody reads.
        void DrawSheetSection();

        // Reads a sheet off disk into m_Volume. Named separately from the button so the failure path has one
        // place to live: an image that will not decode, and a decoded image that is not a sheet, are two
        // different refusals and the artist needs to be told which one happened.
        void ImportSheet( const std::filesystem::path& path );

        // Rebuilds the slice texture from m_Volume. Called when the volume, the axis, the slice index or
        // the channel view changes — never per frame, because it allocates a device image.
        void RefreshSlice();

        // Copies one slice of the volume into RGBA8 the way the current channel view asks for it. Pure and
        // small, so the preview cannot be a different reading of the bytes than the numbers beside it.
        std::vector<unsigned char> BuildSlicePixels() const;

        Assets::AssetManager* m_Assets = nullptr;

        Assets::CloudNoiseVolumeParams m_Params;
        Assets::CloudNoiseVolumeData   m_Volume;
        bool                           m_HasVolume = false;
        std::string                    m_SourceName; // what is in m_Volume: a file name, or "(baked)"
        std::string                    m_Status;     // the last thing that happened, shown to the artist
        bool                           m_StatusIsError = false;

        // ── WHAT AN EDIT HAS REACHED ───────────────────────────────────────────────────────────────────
        //
        // What Save writes is m_Volume -- the VOXELS -- and not the recipe beside them, so the document is
        // dirty when the voxels have been re-baked or imported since the file was last read or written.
        // AN EDITED RECIPE THAT HAS NOT BEEN BAKED IS NOT DIRTY, and that is a fact about this format
        // rather than an oversight: nothing of it has reached the file, and a dot claiming otherwise would
        // send an artist to Save when what they owe is a Bake. The panel says which state it is in.
        //
        // COUNTED, NOT COMPARED. The volume is 8 MiB at the shipped resolution; a snapshot to diff against
        // would double the panel's footprint for a question asked once a frame. AdoptVolume is the one
        // place the buffer changes, so a counter beside it cannot fall behind the way a flag at three call
        // sites would.
        uint32_t m_VolumeRevision = 0u;
        uint32_t m_SavedRevision  = 0u;
        // UNTRACKED IS NOT CLEAN: false until this document has read its subject off disk. A window whose
        // asset would not load has no file to be clean against.
        bool m_Tracked = false;

        // WHERE SAVE WRITES: the subject's own file, resolved once at construction. Save As may write
        // ELSEWHERE, but it never assigns to this — a copy becomes its OWN document rather than repointing
        // this window, because the subject is the window's identity. See DrawSaveSection.
        std::filesystem::path m_SubjectPath;

        // The running bake. A future rather than a raw thread so the result is collected exactly once and
        // the panel cannot be destroyed while a thread is still writing into it.
        std::future<Common::ResultStr<Assets::CloudNoiseVolumeData>> m_Baking;
        std::atomic<float>                                           m_BakeProgress{ 0.0f };
        bool                                                         m_BakeRunning = false;

        // Preview state.
        enum class SliceAxis : int
        {
            X = 0,
            Y = 1,
            Z = 2
        };
        enum class ChannelView : int
        {
            AllFour  = 0, // R and G in red/green, B and A in blue/alpha-as-grey — the whole volume at once
            WispyLF  = 1,
            WispyHF  = 2,
            BillowLF = 3,
            BillowHF = 4
        };

        SliceAxis   m_Axis        = SliceAxis::Z;
        int         m_SliceIndex  = 0;
        ChannelView m_ChannelView = ChannelView::AllFour;
        int         m_PreviewZoom = 3; // integer magnification, so a 128-wide slice is readable

        std::shared_ptr<Graphic::Image2D> m_SliceImage;
        bool                              m_SliceDirty = true;
    };
} // namespace Desert::Editor
