#pragma once

#include <Editor/Panels/FileExplorer/FileType.hpp>
#include <Editor/Widgets/ThumbnailFormats.hpp>

#include <array>
#include <optional>
#include <string_view>

// ONE TABLE: ASSET KIND -> WHAT MAKES ITS THUMBNAIL (THM1n), UE's UThumbnailRenderer-per-class registry.
//
// ONE CHAIN (THM1n-3): extension -> FileType (FileType.hpp, kFileExtensions) -> Producer (this table). The
// per-extension producer table that ThumbnailFormats.hpp used to hold answered the same question a second
// time and drifted from this one; it is gone, and ProducerOfPath below is the only way a path is asked.
//
// The browser used to answer this question in two hand-written copies of the same `if` chain (the tile and
// the tooltip) plus two more in the folder prefetch and the splash warm-up, and a kind added to one copy
// and not the others drew a picture in one place and an icon in another. Every FileType has exactly one
// row here — ThumbnailProducers suite (thumbnail_producers_test) walks the enum and fails on a kind with
// no row — and every consumer switches on ProducerOf.
namespace Desert::Editor::ThumbnailProducers
{
    enum class Producer
    {
        RenderedMesh,     // the cooked mesh photographed (ThumbnailSubject::ResolveMesh, ThumbnailService)
        RenderedMaterial, // the material on its preview (sphere / mesh / volume;
                          // ThumbnailSubject::ResolveMaterial)
        Decoded,          // the file IS a picture: decoded from disk (ThumbnailCache)
        Painted,          // painted on the CPU from the asset's own bytes (ThumbnailService::RequestPainted)
        NotYetProduced,   // a kind UE gives a picture and this editor does not yet: the type icon, and the
                          // register below names each one (kNotYetProduced) so the debt cannot grow silently
        TypeIcon,         // a kind with no picture by design (text, code, settings): the type icon is the answer
    };

    struct Row
    {
        FileType         Type;
        Producer         How;
        std::string_view Why;
    };

    inline constexpr std::array kTable = {
         Row{ FileType::Unknown, Producer::TypeIcon, "nothing is known about the file" },
         Row{ FileType::Scene, Producer::NotYetProduced, "UE: the level's saved camera view" },
         Row{ FileType::Prefab, Producer::NotYetProduced, "UE (Blueprint): its components' meshes rendered" },
         Row{ FileType::Script, Producer::TypeIcon, "source text; the preview pane shows its opening lines" },
         Row{ FileType::Audio, Producer::NotYetProduced, "UE (SoundWave): the waveform in 2D" },
         Row{ FileType::Shader, Producer::TypeIcon,
              "a program has no appearance until a material supplies its parameters; the .demat has the picture" },
         Row{ FileType::Texture, Producer::Decoded, "the image itself" },
         Row{ FileType::Cubemap, Producer::Painted, "the HDR painted on a sphere (HdrSphereThumbnail)" },
         Row{ FileType::Model, Producer::RenderedMesh, "the cooked mesh photographed" },
         Row{ FileType::Material, Producer::RenderedMaterial, "the material on its preview" },
         Row{ FileType::ShaderGraph, Producer::TypeIcon, "a graph document; its material has the picture" },
         Row{ FileType::Ini, Producer::TypeIcon, "settings text" },
         Row{ FileType::Font, Producer::TypeIcon,
              "at 64 px most faces are indistinguishable; the file name is the face name" },
         Row{ FileType::Cloud, Producer::Painted, "painted from its own bytes (CloudThumbnail)" },
         Row{ FileType::UITheme, Producer::Painted, "its palette painted" },
         Row{ FileType::LandscapeLayerInfo, Producer::TypeIcon, "a layer's settings; UE draws its colour swatch" },
         Row{ FileType::ImportSettings, Producer::TypeIcon, "import settings text beside a source file" },
         Row{ FileType::SkinnedMesh, Producer::NotYetProduced, "UE (USkeletalMesh): the mesh in its bind pose" },
         Row{ FileType::Skeleton, Producer::NotYetProduced,
              "UE (USkeleton): its PreviewSkeletalMesh in the bind pose" },
         Row{ FileType::Animation, Producer::NotYetProduced,
              "UE (UAnimSequence): its skeleton's preview mesh at the clip's middle" },
         Row{ FileType::ControlRig, Producer::TypeIcon, "UE draws a rig with its class icon" },
         Row{ FileType::AnimGraph, Producer::TypeIcon, "UE draws an AnimBlueprint with its class icon" },
         Row{ FileType::Retarget, Producer::TypeIcon,
              "a retarget document: two skeletons' mapping, no appearance" },
         Row{ FileType::FoliageType, Producer::NotYetProduced, "UE (UFoliageType): its mesh rendered" },
         Row{ FileType::StringTable, Producer::TypeIcon, "UE draws a StringTable with its class icon" },
         Row{ FileType::CookedWorld, Producer::TypeIcon,
              "cooked streaming data, not authored; nothing to picture" },
    };

    /// The kinds UE photographs that this editor still draws as an icon. Pinned by name so closing one is
    /// an edit here and in the table, and opening a new one is not free.
    inline constexpr std::array kNotYetProduced = { FileType::Scene,       FileType::Prefab,   FileType::Audio,
                                                    FileType::SkinnedMesh, FileType::Skeleton, FileType::Animation,
                                                    FileType::FoliageType };

    /// The row's producer, or nullopt for a kind with no row — a census failure, never a default.
    [[nodiscard]] constexpr std::optional<Producer> ProducerOf( FileType type )
    {
        for ( const Row& row : kTable )
        {
            if ( row.Type == type )
                return row.How;
        }
        return std::nullopt;
    }

    /// The whole chain for one file: its extension, the kind the browser types it as, that kind's producer.
    /// nullopt only for a kind with no row — the same census failure ProducerOf reports.
    [[nodiscard]] inline std::optional<Producer> ProducerOfPath( std::string_view path )
    {
        return ProducerOf( FileTypeOf( ThumbnailFormats::ExtensionOf( path ) ) );
    }
} // namespace Desert::Editor::ThumbnailProducers
