#pragma once

// PK3 — THE SINGLE REGISTER OF DDC BUCKETS A PACKAGED GAME SHIPS.
//
// Before this file existed, `StageCookedEntries` (Editor/Packaging/PackageCook.cpp) held its own
// hand-written array of bucket names. `kMeshDeriver`'s bucket ("StaticMesh", MeshDerivedData.hpp) was
// never added to it, so a packaged game carried every cooked texture, font, icon, shader and pipeline
// blob and NO mesh render data at all — the exact defect a hand-maintained second list invites, because
// nothing forces the two lists (the derivers that exist, and the buckets a package copies) to agree.
//
// This header is now the one place that answers "does the runtime read this DDC bucket". A deriver
// added anywhere in the engine owes a row here; `ShippedDDCBuckets()` is the only input
// `StageCookedEntries` uses, so a missing row is a missing bucket in the produced package rather than a
// bug found by a player looking at an empty scene. `DerivedDataKey`'s
// `EveryDDCDeriverBucketHasExactlyOneRegisterRow` census scans the engine tree for every
// `Common::DDC::Deriver` declaration and fails if one has no row here, or if a row here names a bucket
// no deriver declares.
//
// EDITOR-ONLY BUCKETS ARE LISTED EXPLICITLY, never by omission. "Thumbnails" has no `Common::DDC::Deriver`
// at all (its entries are not addressed by `MakeKey`, see `DerivedDataCache.hpp`'s comment on
// `BucketDir`) and is read only by `Editor/Widgets/ThumbnailCache.cpp` and `ThumbnailKey.hpp` — there is
// no runtime reader, so it stays out of a package on purpose, and that purpose is written down below.

#include <string_view>
#include <vector>

namespace Desert::Assets
{
    enum class DDCBucketReach
    {
        // A runtime loader — code shared by Editor.exe and Runtime.exe, not an editor-only panel — reads
        // this bucket while the game is running (at boot, on stream-in, or on first use). It ships.
        Shipped,

        // No runtime loader reads this bucket. Only editor-side tooling touches it, so a packaged game
        // has no use for it and StageCookedEntries leaves it in the DDC root, off the package.
        EditorOnly,
    };

    struct DDCBucketRow
    {
        std::string_view Bucket;
        DDCBucketReach   Reach;
        std::string_view Reason; // file:line of the deriver (or the editor-only reader) and why
    };

    // clang-format off
    inline constexpr DDCBucketRow kDDCBucketRegister[] = {
        { "StaticMesh",       DDCBucketReach::Shipped,    "Assets/MeshDerivedData.hpp kMeshDeriver - StaticMeshAsset loads mesh render data from this at runtime (PK3: this row was missing, so a package shipped no mesh render data at all)" },
        { "Texture",          DDCBucketReach::Shipped,    "Assets/TextureSourceAsset.hpp kTextureDeriver - texture streaming reads this at runtime" },
        { "CloudModelling",   DDCBucketReach::Shipped,    "Assets/CloudProceduralVolume.hpp kCloudModellingDeriver - VolumetricCloudRenderer reads this at runtime" },
        { "FontCache",        DDCBucketReach::Shipped,    "Text/FontCache.cpp kFontDeriver - text rendering reads this at runtime" },
        { "IconCache",        DDCBucketReach::Shipped,    "Vector/IconBake.cpp kIconDeriver - in-game UI icon rendering reads this at runtime" },
        { "EnvironmentCache", DDCBucketReach::Shipped,    "Graphic/Environment/EnvironmentBake.cpp kEnvironmentDeriver - sky/reflection rendering reads this at runtime" },
        { "PipelineCache",    DDCBucketReach::Shipped,    "Graphic/API/Vulkan/VulkanDevice.cpp kPipelineDeriver - the GPU pipeline-cache blob the driver reads at runtime" },
        { "ShaderCache",      DDCBucketReach::Shipped,    "Core/ShaderCompiler/ShaderSpirvCache.cpp kSpirvDeriver - compiled SPIR-V the material pipelines read at runtime" },
        { "ShaderMap",        DDCBucketReach::Shipped,    "Core/ShaderCompiler/ShaderMapCache.cpp kShaderMapDeriver - VulkanShader::Reload reads this at runtime" },
        { "Thumbnails",       DDCBucketReach::EditorOnly, "no Common::DDC::Deriver; Editor/Widgets/ThumbnailCache.cpp - asset-browser previews, no runtime reader" },
    };
    // clang-format on

    inline std::vector<std::string_view> ShippedDDCBuckets()
    {
        std::vector<std::string_view> out;
        for ( const DDCBucketRow& row : kDDCBucketRegister )
            if ( row.Reach == DDCBucketReach::Shipped )
                out.push_back( row.Bucket );
        return out;
    }
} // namespace Desert::Assets
