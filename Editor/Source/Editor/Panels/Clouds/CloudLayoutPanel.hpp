#pragma once

#include "../IPanel.hpp"

#include <Engine/Assets/CloudLayout.hpp>
#include <Engine/Assets/CloudProceduralVolume.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
}
namespace Desert::Graphic
{
    class Image2D;
}
namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor
{
    /**
     * @brief The artist's tool for a PAINTED SKY: DRAW the layout, or point at a picture and say which of
     *        its channels feeds which cloud SPECIES, look at the SKY it makes, bake it to a `.dclayout`.
     *
     * WHY IT EXISTS. Phase PT shipped the format, the engine path, the Details slot with its picker and
     * its drag-and-drop target, and `Tools/CloudLayoutBaker`. That closes half the loop: an artist could
     * ATTACH a finished painting but could only MAKE one from a terminal. The owner asked for the editor.
     *
     * AND THE BRUSH IS THE SECOND HALF OF THAT ANSWER. Importing a picture presumes somebody drew it
     * somewhere else; the owner asked for a panel where the designer CREATES the texture rather than
     * brings one. The canvas below is that panel, and it is the same surface an import fills, so the two
     * ways in converge at `Assets::MakeCloudLayoutFromCanvas` and neither can come to mean something
     * different by a channel, a mask or a mean. Importing is not removed and is not second class: an
     * artist brings a picture and then paints on it, which is what a canvas that is literally the decoded
     * image buys for nothing.
     *
     * AND SINCE O-4 THERE ARE TWO PICTURES, NOT ONE — Unreal's `Layout_CloudGlobalPattern` and
     * `Layout_GlobalCloudMask`, which are separate texture parameters there and separate material inputs
     * here. The pattern imports and exports as an RGBA picture, the mask as its own grey one. That is not
     * a convenience: a layout needs FIVE planes and an image has four, so while there was one picture the
     * mask had to BE the pattern's alpha, a layout using all five could not be opened for painting at all,
     * and the panel had to keep telling the artist which of two things the alpha plane currently meant.
     * All three of those are gone with the second file.
     *
     * IT IS BUILT AFTER THE CLOUD NOISE VOLUME PANEL NEXT DOOR, deliberately and down to the section
     * order — source, preview, save — because that panel already does work of this shape and a second
     * layout for the same job is a second thing to learn.
     *
     * THE BAKE CALLS THE SAME FUNCTION THE COMMAND-LINE TOOL CALLS. `Assets::MakeCloudLayoutFromCanvas`
     * then `Assets::CloudLayoutAsset::Save`, which is `Tools/CloudLayoutBaker/Source/main.cpp` with a file
     * dialog in front of it. A panel whose output differed from the tool's would be the second path §1.3
     * and §4.2 of the contract forbid, and the difference would show up as a sky rather than as an error.
     *
     * THE PREVIEW SHOWS THE SKY, NOT THE TEXTURE, and that is the panel's one real design decision.
     * Showing the painting is honest and nearly useless: an artist draws a sky, and §PT's own proof that
     * the feature works is a TOP-DOWN frame rather than a texture viewer. So the right-hand pane is a
     * top-down map of the coverage the painting produces, built by `Assets::BuildCloudLayoutPreview` —
     * which calls the very function the bake calls — sampled on the MAPPED SPECIES' OWN PLACEMENT CELL,
     * because that is the resolution that species of cloud can be placed at. The painting is shown beside
     * it at its own resolution, and the difference between the two panes is the whole of what an artist
     * needs to learn.
     *
     * THE LAYER IS READ FROM THE SCENE AND NEVER EDITED HERE. Region Size, Weather Tile Size, Coverage,
     * Seed, Weather Patch Strength and the five Layout fields live on the cloud component, and the panel
     * takes them from whichever layer the open scene has. There are deliberately NO copies of them on this
     * panel: a duplicated Layout Repeats would be two numbers obliged to agree (§4.2), and it would be the
     * one that is wrong that the artist believes. Move them in Details and this panel follows the same
     * frame. With no cloud layer in the scene the panel says so and previews against the shipped defaults,
     * printed rather than assumed.
     *
     * AND IT SHOWS FOUR THINGS THE SKY KNEW AND NOTHING SAID — the first two measured by §PT and left in
     * its protocol, the last two found by reading VolumetricCloudRenderer beside this panel (§PTP):
     *
     *   1. PATTERN STRENGTH AND MASK STRENGTH ARE NOT INDEPENDENT. With the mask at full strength a pair
     *      of frames across the whole pattern slider came back byte for byte identical, because the mask
     *      alone drove the figure's inside over 1 and its outside under 0 and the clamp then ate the
     *      pattern's entire contribution. The panel counts the cells the two ends of the pattern slider
     *      disagree about and says so when the answer is none.
     *   2. LEGIBILITY IS BOUNDED BY THE STROKE, AND THE VALIDATOR CHECKS THE TEXEL.
     *      `ValidateCloudProceduralLayout` compares one layout texel against the cell, which is the right
     *      bound for "can the painting tell two cells apart" and the wrong one for "does this still read
     *      as a letter". The panel measures the painting's own strokes
     *      (`Assets::MeasureCloudLayoutStrokes`) and reports how much of the drawing is finer than a
     *      cloud cell. Reported and never refused, because a stroke width is a fact about a picture and a
     *      threshold on it would be an opinion about somebody's art.
     *
     *      THE BRUSH TURNS THAT REPORT INTO SOMETHING AN ARTIST CAN ACT ON BEFORE DRAWING RATHER THAN
     *      AFTER. The canvas carries the PLACEMENT CELL as a grid, and the cursor carries the stroke's own
     *      measured width as a circle on top of it — so "wider than a cell" is a thing you can see under
     *      your hand instead of a percentage you read afterwards. The grid and the verdict quote the same
     *      two numbers (`m_Preview.CellKm` and `m_TexelKm`) for exactly the reason this panel refuses to
     *      keep its own copy of a layer field: two derivations of one length is how they come to disagree.
     *
     *   3. A CHANNEL DOES NOT DRIVE "CLOUD TYPE N". The four type slots an artist fills in Details are
     *      COMPACTED into species — an empty slot is skipped, a repeated type is dropped
     *      (`ECS::ResolveCloudSpecies`) — and the painting's channels are indexed by SPECIES. So in a
     *      layer whose only authored type sits in Cloud Type 3, it is the painting's RED channel that
     *      decides where that cloud goes. Nothing in Details says so, and the symptom of not knowing it is
     *      a painted channel that appears to do nothing. The panel names the TYPE behind every channel.
     *
     *   4. THE MAP IS THE PAINTING FLIPPED TOP TO BOTTOM, because the layout's v runs NORTH and an image's
     *      first row is its top. Both panes are drawn in the orientation their own reader expects — the
     *      painting as it was authored, the map north up — so the flip between them is real, and it is
     *      stated under them rather than left for an artist to find in a rendered sky.
     *
     * NO BACKGROUND WORK, unlike the noise volume panel: building a 512-square layout from an image is
     * one pass over a million texels and the map is a few thousand evaluations of a closed-form
     * expression, so both are recomputed when something changes and never per frame.
     *
     * ONE WINDOW PER `.dclayout`, OPENED BY DOUBLE-CLICKING THE ASSET — UE's flow, and what this class
     * became in Р3. It was a SINGLETON with an "Open" combo, reached from the View menu; the combo is gone
     * with the singleton, because the subject is now the window's identity.
     *
     * THE SCENE IS STILL READ AND STILL NOT EDITED. A document is bound to an ASSET, but this panel's
     * preview needs the LAYER's numbers to say anything true about a sky, so it keeps following the active
     * scene through SetScene exactly as before. That is not a second subject: nothing here writes to the
     * scene, and the layer is an input to the preview in the same way a light is an input to a material
     * thumbnail.
     */
    class CloudLayoutPanel final : public ISubjectDocument
    {
    public:
        CloudLayoutPanel( const Assets::AssetHandle& subject, std::shared_ptr<::Desert::Core::Scene> scene,
                          Assets::AssetManager* assets );

        // NEVER — see CloudNoiseVolumePanel::HoldsView. The "sky" in this panel's right-hand pane is
        // a CPU-evaluated top-down map uploaded as a Graphic::Image2D, not a rendered frame: there is no
        // Scene of its own and no SceneRenderer, so this document holds no view. It holds a
        // shared_ptr to the ACTIVE scene, which it reads and never renders.

        // The `CloudLayout` this window is about, gone from the manager — deleted in the browser, or the project
        // closed under it. Asked of the metadata rather than of a typed lookup: the question is whether the
        // asset is still THERE, and a typed lookup answers a different one (whether it is still that type).
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
        // The authored state of a `.dclayout` is the CANVAS — the pattern's four planes and the mask's
        // one. m_Layout is derived from it (RebuildLayout) and is what Bake writes, so the question
        // "is there an unsaved edit" is a question about the canvas.
        //
        // COMPARED AGAINST A COPY of what the file held, with a DEFAULTED operator== on
        // Assets::CloudLayoutCanvas — a memcmp of about 1.3 MiB at the shipped 512 side, which is cheap
        // beside the picture this panel draws every frame. A dirty FLAG raised at each of the seven places
        // that can move a texel is the thing that falls behind, and the cost of it falling behind is a
        // painting thrown away by a close that asked nothing.
        [[nodiscard]] DiskState GetDiskState() const override;

        // Bakes the canvas into a layout and writes it to the SUBJECT'S OWN file — the same call the Bake
        // button makes, so the write, the re-registration and the status line cannot drift apart.
        bool SaveDocument() override;

        /// The layout's own numbers: which channel of the picture feeds which cloud species, and which
        /// channel of the imported mask picture is read as the mask. The PICTURES themselves are reported
        /// and refused rather than omitted — a property missing from a census reads as a property the
        /// format does not have.
        [[nodiscard]] std::vector<EditableProperty> EditableProperties() const override;
        [[nodiscard]] Common::BoolResultStr         SetEditableProperty( const std::string&        name,
                                                                         const std::vector<float>& value ) override;

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            // TALL, and the number came from looking at it. The panel carries two square panes and a
            // paragraph of verdict under them; at 900 the map was cut off by the window's own bottom
            // edge, which is the one part an artist opens this panel FOR.
            return { 760.0f, 1120.0f };
        }

        void OnUIRender() override;

        void SetScene( const std::shared_ptr<::Desert::Core::Scene>& scene ) override
        {
            m_Scene        = scene;
            m_PreviewDirty = true;
        }

    private:
        /// Everything about the cloud LAYER the preview needs. Read from the scene each frame, or the
        /// shipped defaults when the scene has no layer — and `FromScene` says which, because a preview
        /// drawn against numbers that are not the artist's is worse than no preview.
        struct LayerContext
        {
            /// One SPECIES of the layer: the thing a channel of the painting actually decides the
            /// placement of. It is NOT "Cloud Type N" — see ECS::ResolveCloudSpecies for why the two are
            /// numbered differently, and why this panel has to say so out loud.
            struct SpeciesSlot
            {
                uint32_t AuthoredSlot = 0u;    // which "Cloud Type N" in Details this came from, 0-based
                bool     BuiltIn      = false; // the engine's cumulus congestus, because no slot is filled
                float    Scale        = 1.0f;  // the type's Placement Scale — how much coarser than the layer
                float    Anisotropy   = 1.0f;  // the type's Placement Anisotropy — the stretch along the wind

                /// The `.decloudtype`'s file name, so the artist reads a name rather than a slot number.
                std::string TypeName;
            };

            bool  FromScene         = false;
            float RegionSizeKm      = 48.0f;
            float LatticeKm         = 3.0f;
            float PatchTileKm       = 21.0f;
            float Coverage          = 0.45f;
            float PatchStrength     = 0.60f;
            float ResolvableChordKm = 0.125f;

            uint32_t Seed = 1u;

            /// How many species the layer has, 1..kCloudLayoutChannels. Channels past this drive nothing
            /// in THIS scene, and the panel says so rather than offering a slot that goes nowhere.
            uint32_t    SpeciesCount = 1u;
            SpeciesSlot Species[Assets::kCloudLayoutChannels];

            Assets::CloudLayoutPlacement Placement;

            // A `BoundLayout` handle used to be read out of the material here, described as "what the panel
            // adopts when it opens with nothing loaded". Nothing read it — not Matches, not the preview,
            // not the source section — and the panel has been keyed on its SUBJECT since Р3, so adopting
            // the layer's painting is not a thing it may do at all. Removed rather than left as a field
            // one reader would eventually be written against (contract §1.3).

            /// Field by field, because these numbers live on a component the Details panel edits and there
            /// is no notification — comparing is what makes moving Layout Repeats in Details redraw the
            /// map on the same frame. A method rather than a loose expression so that a field added above
            /// has one obvious place to be missed from.
            bool Matches( const LayerContext& other ) const;
        };

        void DrawSourceSection();
        void DrawChannelSection();
        void DrawLayerSection();
        void DrawBrushSection();
        void DrawPreviewSection();
        void DrawVerdictSection();
        void DrawSaveSection();

        /// Reads the open scene's first cloud layer. Pure of side effects on the panel's own state.
        LayerContext ReadLayer() const;

        /// Reads the subject `.dclayout` into the editing buffer. Called once, from the constructor: the
        /// subject cannot change, so neither can the answer.
        void LoadSubject();

        /// Which table an import, an export or a stroke is about. One enum for all three, so a button and
        /// the plane it acts on cannot come to disagree.
        enum class Table
        {
            Pattern,
            Mask
        };

        /// Reads a picture off disk into one of the canvas's two tables. Named so the file dialog and the
        /// drag-and-drop target are one operation and cannot decode a picture two different ways.
        void LoadSourceImage( const std::filesystem::path& path, Table table );

        /// Writes one of the canvas's two tables out as a PNG, asking for the name. The other half of the
        /// door LoadSourceImage opens, and the reason a `.dclayout` is not a one-way trip.
        void ExportImage( Table table );

        /// Rebuilds m_Layout from the canvas through Assets::MakeCloudLayoutFromCanvas — the tool's
        /// function, not a second reading of what a picture means.
        void RebuildLayout();

        /// True when the canvas is a surface the brush can paint on: square, and inside the layout's own
        /// resolution bounds. It is the SAME condition the import accepts, asked before the fact rather
        /// than after, so the canvas cannot exist in a state the bake would refuse.
        bool CanPaint() const;

        /// True when the brush is currently aimed at the mask rather than at a species slot. One
        /// derivation of it, because "channel 4 means the mask" restated at each of its five call sites is
        /// how one of them comes to say 3.
        bool PaintingMask() const;

        /// The buffer the brush writes into, and how many channels it interleaves — the pattern's four or
        /// the mask's one. Returned together because they are one fact about one plane; handing them out
        /// separately is how a stride and a buffer come to disagree.
        std::vector<unsigned char>& PaintPlane();
        uint32_t                    PaintPlaneChannels() const;
        uint32_t                    PaintPlaneChannel() const;

        /// Replaces the source with a blank canvas of @p side. Named rather than inlined because the
        /// button and a future caller must clear exactly the same six pieces of state — a canvas that kept
        /// the previous picture's name is a file baked under the wrong one.
        void StartCanvas( uint32_t side );

        /// Streams the painted channel into m_CanvasImage, creating the device image only when the side or
        /// the channel changed. STREAMED AND NOT RECREATED: a drag touches this every frame, and building
        /// a fresh VkImage per frame is descriptor churn for a picture that is one upload.
        void RefreshCanvasImage();

        /// The placement cell in texels of the CURRENT painting, or 0 when there is no map to take it
        /// from. One derivation, shared by the canvas grid and the legibility verdict.
        float CellTexels() const;

        /// Rebuilds the map, the stroke statistics and both device images. Called when the layout, the
        /// layer or the previewed slot changes — never per frame, because it allocates device images.
        void RefreshPreview( const LayerContext& layer );

        /// The parameters a bake would receive for @p layer with this painting bound. One place, so the
        /// map and the numbers beside it cannot describe different skies.
        Assets::CloudProceduralFieldParams BuildParams( const LayerContext& layer ) const;

        std::shared_ptr<::Desert::Core::Scene> m_Scene;
        Assets::AssetManager*                  m_Assets = nullptr;

        /// WHERE THE BAKE WRITES: the subject's own file, resolved once at construction. A document saves
        /// over what it opened — there is no "Bake to..." dialog here, because writing somewhere else would
        /// leave the window's title, its ImGui id and its open-or-focus key all naming the file it no
        /// longer edits. Making a NEW painting is the asset browser's job ("New Cloud Layout").
        std::filesystem::path m_SubjectPath;

        // WHAT THE FILE HOLDS, as of the last open or the last bake to this document's own file. The other
        // side of the comparison GetDiskState makes. UNTRACKED IS NOT CLEAN: m_Tracked is false until this
        // document has actually recovered a canvas from its subject, because a window whose painting would
        // not open has no file to be clean against.
        Assets::CloudLayoutCanvas m_OnDiskCanvas;
        bool                      m_Tracked = false;

        // Bakes the canvas and writes @p target. The body of the Bake button, lifted out so SaveDocument
        // runs the same sequence rather than a second route to the same bytes.
        bool WriteTo( const std::filesystem::path& target, bool isCopy );

        // ---- source -----------------------------------------------------------------------------------

        /// The surface being authored: the pattern's four planes and the mask's one, at one side. It is
        /// the engine's own struct rather than a pair of buffers the panel keeps in step by hand, so
        /// "these two tables share a resolution" is stated where the import enforces it.
        Assets::CloudLayoutCanvas m_Canvas;

        std::string m_SourceName; // the picture's file name, or the .dclayout's

        /// Which SOURCE channel feeds each species slot. THE CONVENTION IS A CONTROL AND NOT AN
        /// ASSUMPTION: a painting is usually greyscale, and without this an artist wanting one drawing on
        /// slot 2 would have to author an RGBA image to say so. Defaults to straight RGBA, the tool's own
        /// default.
        uint32_t m_ChannelForSlot[Assets::kCloudLayoutChannels] = { 0u, 1u, 2u, 3u };

        /// Which channel of a MASK picture carries the mask. A greyscale PNG loads with R == G == B, so 0
        /// is right for everything an artist is likely to draw; it exists so a mask somebody packed into
        /// an alpha is not a re-export.
        int m_MaskSourceChannel = 0;

        // ---- the brush --------------------------------------------------------------------------------

        /// The brush's three numbers. They live here rather than on the layout because they describe the
        /// HAND and not the picture: nothing about them is written to the file, and a canvas opened
        /// tomorrow is painted with whatever the artist has the sliders on then.
        Assets::CloudLayoutBrush m_Brush;

        /// Which plane of the canvas the stroke lands in: 0..3 are the pattern's four species slots and
        /// @ref kMaskPaintChannel is the add/remove mask.
        ///
        /// FIVE AND NOT FOUR IS THE WHOLE OF O-4 SEEN FROM THE BRUSH. The mask used to share the alpha
        /// plane with species slot 3, so this control had four entries, one of which meant two different
        /// things depending on a checkbox somewhere else, and the panel had a paragraph explaining which.
        /// A layout has five tables; the canvas now has five planes; the combo has five entries.
        int m_PaintChannel = 0;

        /// The entry in that combo which is the mask. Named, because "4" spelled at each comparison is how
        /// one of them comes to be 3.
        static constexpr int kMaskPaintChannel = static_cast<int>( Assets::kCloudLayoutChannels );

        /// The side a "New canvas" would be. 512 is the shipped paintings' own resolution and, at the
        /// shipped 48 km region, puts one texel at 94 m — a thirtieth of a placement cell, so a stroke has
        /// room to be measured rather than quantised.
        int m_NewCanvasSide = 512;

        /// Pixels the canvas is drawn at. Larger than the preview panes on purpose: this one is drawn ON.
        int m_CanvasPane = 320;

        /// Whether the placement cell is drawn over the canvas. On by default, and it is the reason the
        /// canvas exists at this size — the grid is the width rule made visible.
        bool m_ShowCellGrid = true;

        /// The open drag, between mouse-down and mouse-up. See Assets::CloudLayoutStroke for why a stroke
        /// has state at all rather than being a series of independent stamps.
        Assets::CloudLayoutStroke m_Stroke;
        bool                      m_Painting = false;
        glm::vec2                 m_LastPaintTexel{ 0.0f, 0.0f };

        std::shared_ptr<Graphic::Image2D> m_CanvasImage;
        uint32_t                          m_CanvasImageSide    = 0u;
        int                               m_CanvasImageChannel = -1;
        bool                              m_CanvasImageDirty   = true;

        // ---- the layout being authored ----------------------------------------------------------------

        Assets::CloudLayoutData m_Layout;
        bool                    m_HasLayout = false;

        // AN `m_LayoutFromFile` FLAG USED TO SIT HERE, and it went with "Edit this painting". Both existed
        // because recovering the canvas from a layout could FAIL — five planes did not fit in four — so a
        // document opened on a `.dclayout` had to sit in a half-open state until somebody asked for the
        // recovery and it either worked or was refused. O-4 gave the mask its own plane; the recovery
        // cannot fail for that reason any more, so the subject is on the canvas the moment the window
        // opens and there is no second state to record. It also fixed a real regression on the way: with
        // the canvas empty until the button was pressed, "Export pattern" on a freshly opened document
        // would have had nothing to write.

        // ---- preview ----------------------------------------------------------------------------------

        enum class PaintingView : int
        {
            Slot0 = 0,
            Slot1,
            Slot2,
            Slot3,
            Mask
        };

        int          m_PreviewSlot  = 0;
        PaintingView m_PaintingView = PaintingView::Slot0;
        int          m_SpanRegions  = 1; // region periods the sky map covers, so tiling is visible

        /// Pixels each pane is drawn at. 180 AND NOT MORE BY DEFAULT: at 220 the verdict under the panes —
        /// which is the part of this panel an artist opens it for — fell below the window's bottom edge on
        /// a 1289-point screen. The slider goes to 512 for anyone who wants to study the map itself.
        int m_PreviewSide = 180;

        Assets::CloudLayoutPreview     m_Preview;
        bool                           m_HasPreview = false;
        Assets::CloudLayoutStrokeStats m_Strokes;

        /// The texel size and period the verdicts were computed at, carried out of the refresh so the text
        /// beside the map cannot quote a different sky than the map.
        float m_TexelKm  = 0.0f;
        float m_PeriodKm = 0.0f;

        std::shared_ptr<Graphic::Image2D> m_PaintingImage;
        std::shared_ptr<Graphic::Image2D> m_SkyImage;

        bool         m_PreviewDirty = true;
        LayerContext m_LastLayer;

        // ---- status -----------------------------------------------------------------------------------

        std::string m_Status;
        bool        m_StatusIsError = false;
    };
} // namespace Desert::Editor
