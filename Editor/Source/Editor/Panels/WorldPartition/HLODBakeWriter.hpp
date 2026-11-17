#pragma once

// THE ONE WRITER OF BAKED HLOD MATERIALS, shared by the landscape bake (LandscapeHLODBake: a tile's landscape
// material seen from above, UE ULandscapeHLODBuilder) and the mesh bake (HLODMaterialBake: the source materials of
// a MeshMerge / MeshSimplify HLOD flattened into its atlas, UE FMeshMergingSettings::bMergeMaterials). Both read
// the same G-buffer targets back from an orthographic capture and both end as `T_<stem>_{BaseColor, ORM, Normal,
// Emissive}.detex` + `M_<stem>.demat` (StandardSurface) in the scene's BuiltHLODDirectory, so a far landscape tile
// and a far building are shaded by one material path and one set of texture intents.
//
// THE TEXEL LAYOUT IS THE CAPTURE'S: texel (c, r) of a W x H image is UV ((c + 0.5) / W, (r + 0.5) / H) of the
// mesh it is baked for - row 0 is the capture's world -Z edge (OriginZ), column 0 its OriginX edge, the readback's
// own order - and row 0 is the first row of the written image.

#include <Engine/Geometry/RenderMeshData.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Editor::HLODBake
{
    // The G-buffer targets as read back (GBufferOutputs.glslh), W x H, row 0 first.
    struct GBufferTexels
    {
        std::vector<uint8_t> A;        // attachment 0, RGBA8 UNORM: BaseColor.rgb (LINEAR), Metallic.a
        std::vector<uint8_t> B;        // attachment 1, RGBA16F: world Normal.xyz, Roughness.a
        std::vector<uint8_t> Emissive; // attachment 3, RGBA16F: Emissive.rgb (HDR), material AO.a
        uint32_t             Width  = 0;
        uint32_t             Height = 0;
    };

    // Refused with the numbers when a target's size is not W x H texels of its format.
    [[nodiscard]] Common::BoolResultStr CheckSizes( const GBufferTexels& texels );
    // G-buffer B's xyz: the signed world normal. Zero where nothing drew (the clear colour).
    [[nodiscard]] glm::vec3 WorldNormal( const GBufferTexels& texels, std::size_t texel );
    [[nodiscard]] bool      Covered( const GBufferTexels& texels, std::size_t texel );
    [[nodiscard]] float     Roughness( const GBufferTexels& texels, std::size_t texel );  // B.a
    [[nodiscard]] float     MaterialAO( const GBufferTexels& texels, std::size_t texel ); // Emissive.a
    [[nodiscard]] glm::vec3 EmissiveRadiance( const GBufferTexels& texels, std::size_t texel );

    [[nodiscard]] uint8_t UnitToByte( float value );
    // A linear [0, 1] value as an 8-bit sRGB code (every authored BaseColor is stated sRGB).
    [[nodiscard]] uint8_t LinearToSrgb8( float linear );
    [[nodiscard]] float   Srgb8ToLinear( uint8_t code );

    // Every texel of a W x H image whose centre lies in a render triangle of @p mesh in UV space (the layout
    // above), handed to @p visit with its barycentric weights and the triangle's three vertices. A texel on an
    // edge two triangles share is visited for both (the frame is continuous there).
    using UVTexelVisitor = std::function<void( std::size_t texel, const glm::vec3& weights,
                                               const std::array<const Desert::Vertex*, 3>& )>;
    void RasterizeUV( const Geometry::RenderMeshData& mesh, uint32_t width, uint32_t height,
                      const UVTexelVisitor& visit );

    // The vertex shader's frame at a texel (Mesh/Surface/Vertex_Static.glslh): each corner's T, B, N normalised,
    // interpolated barycentrically and left unnormalised, as the rasteriser hands them to the fragment shader.
    [[nodiscard]] glm::mat3 InterpolatedFrame( const glm::vec3&                            weights,
                                               const std::array<const Desert::Vertex*, 3>& v );

    // @p world (a world normal) taken into the tangent frame @p tbn - normalize(TBN * n) gives it back, what
    // SurfaceTypes.glslh computes from the stored texel - and written as RGBA8 into @p rgba; z kept >= a small
    // positive floor (the decoder rebuilds z from x and y). False, nothing written, when the frame or the normal
    // is degenerate.
    [[nodiscard]] bool EncodeTangentNormal( const glm::vec3& world, const glm::mat3& tbn, uint8_t* rgba );

    // A flat tangent-space normal image (0, 0, 1) of W x H.
    [[nodiscard]] std::vector<uint8_t> FlatNormals( uint32_t width, uint32_t height );

    // For every texel of a W x H image, the index of the nearest texel @p covered marks non-zero (rings outward,
    // 4-neighbourhood; a covered texel is its own). Empty when no texel is covered.
    [[nodiscard]] std::vector<std::size_t> NearestCovered( const std::vector<uint8_t>& covered, uint32_t width,
                                                           uint32_t height );

    // UE's UV border smear (FMaterialBakingHelpers::PerformUVBorderSmear): every texel @p covered marks 0 takes
    // the RGBA of the nearest covered texel (rings outward, 4-neighbourhood), so bilinear taps and mips at an
    // island's edge never pull the clear colour of the empty atlas in. An image with no covered texel is left as
    // it is.
    void SmearUncovered( std::vector<uint8_t>& rgba, const std::vector<uint8_t>& covered, uint32_t width,
                         uint32_t height );

    // What one baked material is made of, all W x H RGBA8 in the layout above. BaseColor is always written;
    // an absent map is not written and the material keeps StandardSurface's constant for it.
    struct BakedImages
    {
        uint32_t                            Width  = 0;
        uint32_t                            Height = 0;
        std::vector<uint8_t>                BaseColor; // sRGB-encoded
        std::optional<std::vector<uint8_t>> ORM;       // linear: r = AO, g = roughness, b = metallic
        std::optional<std::vector<uint8_t>> Normal;    // tangent space of the mesh the material is drawn on
        std::optional<std::vector<uint8_t>> Emissive;  // sRGB-encoded radiance / EmissiveScale
        float                               EmissiveScale = 1.0f; // EmissiveIntensity: the brightest channel
    };

    // The material a built HLOD's slot binds, as a scene record stores a material slot.
    struct BakedMaterial
    {
        std::string           Guid;
        std::string           Path; // assets-relative (StoredFormFor("MaterialAsset")), not the tagged key
        std::filesystem::path File;
    };

    // Writes `T_<stem>_<Map>.detex` for every map present and `M_<stem>.demat` binding them to StandardSurface in
    // BuiltHLODDirectory(@p scenePath); an existing material keeps its GUID (every reference holds it), a texture
    // its GUID with the bake's settings restated. Refused by name when a file cannot be encoded or written.
    [[nodiscard]] Common::ResultStr<BakedMaterial>
    WriteBakedMaterial( const std::filesystem::path& scenePath, const std::string& stem, BakedImages images );
} // namespace Desert::Editor::HLODBake
