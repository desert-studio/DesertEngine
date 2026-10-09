#pragma once

#include <Editor/Panels/FileExplorer/FileType.hpp>
#include <Engine/Assets/ThumbnailInfo.hpp>
#include <Common/Core/ResultStr.hpp>

#include <ImGui/imgui.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Graphic
{
    class Image2D;
}

namespace Desert::Editor
{
    struct DirectoryInformation;
    class ThumbnailCache;

    /// A TILE'S PICTURE, AUTHORED FROM THE BROWSER (UE: SThumbnailEditModeTools + "Capture Thumbnail"). Two
    /// tools on one picture: Capture takes the main viewport's frame as the asset's thumbnail; Edit Thumbnail
    /// makes the tile itself the orbit control — a left drag orbits, the wheel zooms — and ONE gesture is ONE
    /// ThumbnailEdit::EditOrbit (one write into the asset's home, one undo entry) when it ends: the drag is
    /// released, or the wheel rests for kWheelRestSeconds, or the pointer leaves the tile. Esc or a click
    /// outside the tile leaves the mode, and so does leaving the folder. While a gesture runs the tile shows
    /// the LIVE picture: ThumbnailService::RequestPreview* with the live orbit (UE's realtime thumbnail), never
    /// written or cached; the orbit the gesture settles on is re-shot from the home after.
    class ThumbnailEditMode
    {
    public:
        /// A selected entry whose picture has an editable orbit: the entry as listed, and the file its orbit
        /// is read from and written to (OrbitFileOf).
        struct Subject
        {
            std::string Asset;
            std::string OrbitFile;
        };

        /// What the browser's thumbnail pool answers (the panel's until the pool is its own piece).
        struct Delegates
        {
            /// The cooked file a mesh-kind entry's picture is filed under (a static mesh's .stmesh, a skinned
            /// source's .skmesh); nullopt when it has none yet (not imported).
            std::function<std::optional<std::string>( const std::string& assetPath, FileType type )>
                 CookedPictureOf;
            /// The model a RenderedMesh tile photographs (a model itself, or a foliage type's mesh).
            std::function<std::optional<std::string>( const DirectoryInformation& )> MeshSourceOf;
            /// The browser's decoded pictures. Draw decodes the orbit preview through it itself, so the decode
            /// sits beside the PreviewLanded gate that makes it a re-read (ThumbnailRequesters census).
            std::function<ThumbnailCache&()> Thumbnails;
            /// The ImGui texture of a decoded picture; null while it has none.
            std::function<ImTextureID( const std::shared_ptr<Graphic::Image2D>& image )> TextureIdOf;
            /// A picture was written for @p assetPath under @p png: drop the cached decode and any refusal.
            std::function<void( const std::string& png, const std::string& assetPath )> OnCaptured;
        };

        ThumbnailEditMode( Assets::AssetManager* assetManager, std::weak_ptr<::Desert::Core::Scene> viewportScene,
                           Delegates delegates );

        /// Whether there is a viewport scene whose frame Capture would read.
        [[nodiscard]] bool CanCapture() const
        {
            return !m_ViewportScene.expired();
        }
        /// UE "Capture Thumbnail": the main viewport's rendered image, centre-cropped to a square, scaled to
        /// 512 and saved AS this asset's thumbnail — under the key its tile reads, with the hash its judge
        /// compares (ThumbnailProducers::CaptureKeyOf -> ThumbnailService::PictureKey), so the service does not
        /// re-shoot over it.
        Common::BoolResultStr Capture( const DirectoryInformation& entry );

        /// THE FILE @p entry's THUMBNAIL ORBIT LIVES UNDER, the one its picture is filed under: a material's
        /// .demat; a posed kind's own .skmesh / .skeleton / .anim; a model's or a foliage type's mesh picture.
        /// nullopt for a kind with no orbit (ThumbnailProducers::HasThumbnailOrbit) or a model with no picture.
        [[nodiscard]] std::optional<std::string> OrbitFileOf( const DirectoryInformation& entry ) const;
        /// The entries of @p entries whose orbit can be edited (the palette's "Edit Thumbnail: …" commands).
        [[nodiscard]] std::vector<Subject> SubjectsOf( const std::vector<DirectoryInformation*>& entries ) const;

        /// The Edit Thumbnail command on @p entry (the command's @p label names a refusal).
        Common::BoolResultStr Enter( const DirectoryInformation& entry, std::string_view label );
        [[nodiscard]] bool    IsEditing( const std::string& assetPath ) const
        {
            return !m_Path.empty() && m_Path == assetPath;
        }
        /// Runs the mode on the tile item just drawn (the thumbnail button, rect @p min..@p max).
        void Draw( const DirectoryInformation& entry, const ImVec2& min, const ImVec2& max );
        /// Leaves the mode: a running gesture is committed first, its preview ended.
        void Leave();

    private:
        struct Gesture
        {
            Assets::ThumbnailOrbit From;               // the orbit the home stated when the gesture began
            Assets::ThumbnailOrbit Live;               // From moved by the drag and the wheel so far
            ImVec2                 Drag{ 0.0f, 0.0f }; // pixels of the current left drag
            float                  Wheel     = 0.0f;   // notches so far
            double                 LastWheel = 0.0;    // ImGui time of the last notch
            std::string            PreviewKey;         // the path the service files this asset's preview under
            std::string            PreviewPng;         // ThumbnailKey::PreviewPath of it, once a preview was asked
        };
        static constexpr double kWheelRestSeconds = 0.35;

        // The gesture's orbit written as one edit; the gesture ends whether or not the write succeeded.
        void CommitGesture();
        // The live orbit asked of ThumbnailService as a preview (subject resolved as the tile resolves it).
        void RequestPreview( const DirectoryInformation& entry, Gesture& gesture );

        Assets::AssetManager*                m_AssetManager = nullptr;
        std::weak_ptr<::Desert::Core::Scene> m_ViewportScene;
        Delegates                            m_On;

        std::string            m_Path;      // the entry in the mode; empty = no tile is being edited
        std::string            m_OrbitFile; // OrbitFileOf( m_Path )
        std::optional<Gesture> m_Gesture;
    };
} // namespace Desert::Editor
