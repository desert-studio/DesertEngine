#pragma once

#include "../IPanel.hpp"

#include <Engine/Assets/CloudModellingVolume.hpp>

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
     * @brief The artist's tool for a HERO CLOUD'S BODY: sculpt it, look through it, bake it, put it in the
     *        sky — without touching code.
     *
     * WHY THIS PANEL IS THE POINT OF PHASE Э4. The cloud field has two producers behind one seam
     * (ANALYSIS_APPROACH.md §4). The procedural one is infinite, needs no content, and has a limit three
     * separate tasks measured independently: its Alligator coverage is `best - second`, which lays a zero
     * between every pair of cells, so its lobes CANNOT MERGE and the sky reads as a deck of separate
     * cushions. A0 built the other producer and proved a fused body could exist by writing one in C++.
     * This panel is what makes that capability belong to an artist rather than to a compiler.
     *
     * IT IS AN EDITOR FOR CloudModellingVolumeRecipe AND NOTHING ELSE. A0 put the recipe in the `.dcmv`
     * header precisely so this would be possible — so there is exactly one description of a sculpted body
     * in the codebase, the panel edits it, the baker bakes it, and Open reads it back out of a file
     * somebody saved a month ago. No second scene representation, no project-side sidecar.
     *
     * THE THREE THINGS IT HAS TO DO, and why each is shaped the way it is:
     *
     *   SCULPT   lumps, added and removed and selected, each with a position, a size, a rotation, a
     *            primitive and a weight in the union — plus the one knob the whole phase turns on, the
     *            join's BLEND RADIUS. That number decides whether neighbouring lobes read as separate
     *            beads or fuse into one convective mass, which is the exact capability the procedural
     *            producer cannot have by construction.
     *
     *   SEE      a 3D field has no picture; it has SLICES. The panel bakes ONE PLANE per frame it needs
     *            one — 1/64 or 1/128 of a volume — so the preview keeps up with a slider where a full bake
     *            (tens of seconds unoptimised) would not. The plane comes from the same evaluator the full
     *            bake uses, so what the artist tunes against is what Save writes; the suite
     *            Desert/Tests/Engine/CloudModellingRecipe asserts that on the bytes rather than trusting
     *            it.
     *
     *   KEEP     bake and Save As to a `.dcmv`, registered with the AssetManager and uploaded to the
     *            CloudModellingService immediately, so a hero cloud's slot lists it without a restart —
     *            and Open, which reads a volume's recipe back so a body can be revised rather than
     *            re-sculpted.
     *
     * THE FULL BAKE RUNS OFF THE UI THREAD, and it is not optional politeness. 1 048 576 voxels against
     * every lump measures about 1.6 s optimised and tens of seconds in the debug build this is developed
     * in; freezing the editor for that is how a tool gets a reputation in one use. The panel refuses a
     * second bake while one is running, and it CANCELS a running bake in its destructor rather than
     * waiting for a job that has no reason to finish — which is why
     * Assets::GenerateCloudModellingVolume's progress callback can say stop.
     *
     * WHAT IS DELIBERATELY NOT HERE: no viewport gizmo for dragging lumps in 3D. That is a scene-editing
     * mechanism and a hero cloud is an ASSET being authored, not an entity being placed; the entity's own
     * transform is what puts a finished body in the sky (phase A2). Numbers and slices are what a volume
     * with 15.6 m voxels is actually tuned by.
     *
     * ONE WINDOW PER `.dcmv`, OPENED BY DOUBLE-CLICKING THE ASSET — UE's flow, and what this class became
     * in Р3. It was a SINGLETON with an "Open..." file dialog in it, reached from the View menu; the dialog
     * is gone with the singleton, because the subject is now the window's identity and a window that could
     * open a different body would be titled after a file it no longer edits.
     */
    class CloudModellingVolumePanel final : public ISubjectDocument
    {
    public:
        CloudModellingVolumePanel( const Assets::AssetHandle& subject, Assets::AssetManager* assets );
        ~CloudModellingVolumePanel() override;

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 620.0f, 780.0f };
        }

        void OnUIRender() override;

        // NEVER — see CloudNoiseVolumePanel::HoldsView. This panel bakes one slice plane per frame
        // on the CPU and uploads it as a Graphic::Image2D; it owns no Scene and no SceneRenderer, so it
        // holds no view and frees none when it closes.

        // The `CloudModellingVolume` this window is about, gone from the manager — deleted in the browser, or the
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

        // ...and never will — see CloudNoiseVolumePanel::ClaimsView.
        [[nodiscard]] bool ClaimsView() const override
        {
            return false;
        }

        // ── WHAT AN EDIT HAS REACHED ───────────────────────────────────────────────────────────────────
        //
        // The authored state of a `.dcmv` is its RECIPE — "THE ONE PIECE OF STATE", as m_Recipe's own note
        // puts it — and the voxels in the file are a pure function of it. So the document is dirty exactly
        // when the recipe differs from the one the file's header carries, compared against a copy with a
        // DEFAULTED operator== so that a field added to a lump tomorrow is compared tomorrow.
        [[nodiscard]] DiskState GetDiskState() const override;

        // ── SAVING A `.dcmv` IS A BAKE, AND THIS ONE BLOCKS ────────────────────────────────────────────
        //
        // The Bake & Save button runs the bake on the job pool so the artist can keep typing. A
        // programmatic save cannot: SaveDocument's contract is that its answer says whether the FILE WAS
        // WRITTEN, and a call that started a job and returned would leave a caller unable to tell "refused"
        // from "in progress" — the two-facts-one-answer shape §1.4 is about. So this one runs the same
        // generator synchronously and answers about the file. It costs about 200 ms for the shipped
        // eight-lump body in a debug build (Г10's measurement), which is what a save costs.
        bool SaveDocument() override;

        /// The recipe's own numbers, and each lump's, addressed as `Lump0.CentreKm` and so on. Derived
        /// from the recipe rather than listed, so a body with three lumps offers three lumps' worth of
        /// rows and a body with nine offers nine.
        [[nodiscard]] std::vector<EditableProperty> EditableProperties() const override;
        [[nodiscard]] Common::BoolResultStr         SetEditableProperty( const std::string&        name,
                                                                         const std::vector<float>& value ) override;

    private:
        // Reads the subject's recipe out of its `.dcmv` header into the editing buffer. Called once, from
        // the constructor: the subject cannot change, so neither can the answer.
        void LoadSubject( Assets::AssetManager* assets );

        void DrawRecipeSection();
        void DrawLumpListSection();
        void DrawSelectedLumpSection();
        void DrawPreviewSection();
        void DrawBakeSection();

        // Collects a finished bake and writes it. Called once per frame from OnUIRender, because a future
        // that nobody polls is a thread whose result is discarded on shutdown.
        void PollBake();

        // Writes @p voxels to m_BakeTarget and registers the asset. Split out because the write and the
        // registration are the same whether the bake was started by Save As or by a re-bake, and two
        // copies of an asset-registration sequence is how one of them comes to lack the upload.
        void StoreBakedVolume( std::vector<unsigned char>&& voxels );

        // Rebuilds the slice texture from the CURRENT recipe. Bakes one plane; does not touch the volume.
        // Called only when something the plane depends on has changed, never unconditionally per frame.
        void RefreshSlice();

        // Marks the preview stale. Every widget that can move a voxel calls this, which is why it is one
        // function and not a flag set in twenty places.
        void InvalidateSlice()
        {
            m_SliceDirty = true;
        }

        // Applies the primitive's own constraint to a lump's radii (a sphere's three agree, a capsule's
        // cross-section is round). The panel keeps every lump legal AS IT IS EDITED so that
        // ValidateCloudModellingRecipe's refusals are a guard against hand-edited files rather than
        // something an artist meets by moving a slider.
        static void ConformRadiiToPrimitive( Assets::CloudModellingBlob& blob );

        Assets::AssetManager* m_Assets = nullptr;

        // THE ONE PIECE OF STATE. Everything else in this panel is derived from it or is about showing it.
        Assets::CloudModellingVolumeRecipe m_Recipe;

        // WHAT THE FILE'S HEADER HOLDS, as of the last open or the last bake to this document's own file.
        // The other side of the comparison GetDiskState makes. UNTRACKED IS NOT CLEAN: m_Tracked is false
        // until this document has actually decoded its subject, because a window showing the shipped
        // example has no file to be clean against.
        Assets::CloudModellingVolumeRecipe m_OnDiskRecipe;
        bool                               m_Tracked = false;

        int         m_Selected = 0; // which lump the property editor is showing, -1 for none
        std::string m_SourceName;   // what m_Recipe came from: a file name, or "(the shipped example)"
        std::string m_Status;       // the last thing that happened, shown to the artist
        bool        m_StatusIsError = false;

        // The running bake. A future rather than a raw thread so the result is collected exactly once and
        // the panel cannot be destroyed while a worker is still writing into it.
        std::future<Common::ResultStr<std::vector<unsigned char>>> m_Baking;
        std::atomic<float>                                         m_BakeProgress{ 0.0f };
        std::atomic<bool>                                          m_BakeCancelled{ false };
        bool                                                       m_BakeRunning = false;

        // Where the finished bake goes, and the recipe it was started from. The recipe is SNAPSHOT at the
        // moment Save was pressed, so that editing while a bake runs cannot make the file disagree with
        // the header written beside it.
        std::filesystem::path              m_BakeTarget;
        Assets::CloudModellingVolumeRecipe m_BakingRecipe;

        // THE FILE THIS DOCUMENT EDITS, resolved once at construction. "Bake & Save" writes here; "Bake &
        // Save As" may write ELSEWHERE, but never assigns to this — a copy becomes its OWN document rather
        // than repointing this window, because the subject is the window's identity. See DrawBakeSection.
        std::filesystem::path m_SubjectPath;

        // Which of the four channels the preview shows. All four at once is the overview; one at a time is
        // the only way to tell "the Density Scale channel is flat" from "the profile is too shallow".
        enum class ChannelView : int
        {
            AllFour            = 0, ///< profile as brightness, detail type as hue, envelope as the surround
            DimensionalProfile = 1,
            DetailType         = 2,
            DensityScale       = 3,
            CutoutEnvelope     = 4,
        };

        Assets::CloudModellingAxis m_Axis        = Assets::CloudModellingAxis::Z;
        int                        m_SliceIndex  = static_cast<int>( Assets::kCloudModellingVolumeDepth / 2 );
        ChannelView                m_ChannelView = ChannelView::AllFour;
        int                        m_PreviewZoom = 3; // integer magnification, so a 128-wide slice is readable

        std::shared_ptr<Graphic::Image2D> m_SliceImage;
        uint32_t                          m_SliceImageWidth  = 0;
        uint32_t                          m_SliceImageHeight = 0;
        bool                              m_SliceDirty       = true;
    };
} // namespace Desert::Editor
