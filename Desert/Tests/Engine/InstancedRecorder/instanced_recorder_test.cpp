// Г28 — THE TEXTURE CHANNEL OF EVERY BATCHED SURFACE, AND THE ONE DECISION THAT DECIDED IT.
//
// The defect this suite pins was invisible to every existing test and to the frame itself. A material
// that reaches the screen through a hardware-instanced draw was recorded with a material the RENDERER
// owned, not with the one the mesh's `.demat` resolved to; the renderer's material has no asset behind
// it, so its samplers hold the shader schema's 1x1 white default. Measured on the world scene
// (Worlds/World_Grid8km.desce, 1024 floor cubes on `M_CheckerFloor.demat`):
//
//   | knob                                   | before          | after                  |
//   | replace u_AlbedoTexture's handle       | 0 / 0 / 0       | 317 852 / 436 601 /    |
//   | (street / mid / altitude, of 560 560)  |                 | 547 802                |
//   | UVTiling 500 -> 1                      | 0 / 0 / 0       | 313 064 / 434 787 /    |
//   |                                        |                 | 546 609                |
//   | AlbedoColor -> red (positive control)  | 317 889 / 436 005 / 547 534              |
//
// Noise floor: 0 of 560 560, measured over three independent runs of the same binary on the same
// scene — byte-identical PNGs, the warm-up run included. And the same texture swap on
// MAT_ProbeGeoShadows, which holds ONE floor cube and therefore takes the PER-OBJECT path, moved
// 490 090 pixels (87.43 %) BEFORE the fix: the texture channel was alive everywhere a draw was not
// batched, which is why no scene in the repository ever showed this.
//
// The colour moved because a colour rides the Materials[] storage row, which IS per instance. The
// texture did not, because a texture is a descriptor and a draw binds one set.
//
// WHY THE SUITE IS THIS SHAPE. The wrong line lived inside `MeshRenderer::DrawStaticMeshes`, which
// needs a Vulkan device, a swapchain and a scene to run at all — so the decision was lifted OUT of it
// into `SelectInstancedRecorder`, a constexpr function over two booleans. That is the entire content of
// the defect: the old code answered `RendererSpare` unconditionally. This file holds the new answer to
// being different on the branch that mattered, and holds the shader table to having the cells that make
// the new answer reachable.

#include <gtest/gtest.h>

#include <Engine/Graphic/Materials/Mesh/InstancedRecorder.hpp>
#include <Engine/Graphic/Materials/Mesh/MeshVertexPath.hpp>

using Desert::Graphic::InstancedRecorder;
using Desert::Graphic::MeshPass;
using Desert::Graphic::MeshShaderFor;
using Desert::Graphic::MeshVertexPath;
using Desert::Graphic::SelectInstancedRecorder;

// THE ROW THE DEFECT WAS. A group whose material came from a `.demat` and whose instanced sibling was
// built must be recorded by that sibling. Answering `RendererSpare` here is precisely what drew 1024
// floor cubes with a white 1x1 in `u_AlbedoTexture`, and it is what this expectation forbids.
TEST( InstancedRecorder, AnAssetBackedGroupIsRecordedByItsOwnVariant )
{
    EXPECT_EQ( SelectInstancedRecorder( /*groupHasAsset=*/true, /*assetVariantBuilt=*/true ),
               InstancedRecorder::AssetVariant );

    // Stated as an inequality too, and not as decoration: the failure mode has a NAME, and a future
    // edit that reintroduces it will read as "AssetVariant became RendererSpare" rather than as an
    // anonymous enum mismatch.
    EXPECT_NE( SelectInstancedRecorder( true, true ), InstancedRecorder::RendererSpare );
}

// THE REFUSAL, AND WHY IT IS NOT `RendererSpare`. When the engine has no (Instanced x pass) shader for
// an asset's surface there is no honest material to record with. Substituting the spare would draw the
// geometry — in the right place, at the right size, with the right silhouette — and in the wrong
// colours, with nothing in the log. `None` makes the caller say so.
TEST( InstancedRecorder, AnAssetBackedGroupWithNoVariantIsRefusedRatherThanSubstituted )
{
    EXPECT_EQ( SelectInstancedRecorder( /*groupHasAsset=*/true, /*assetVariantBuilt=*/false ),
               InstancedRecorder::None );
    EXPECT_NE( SelectInstancedRecorder( true, false ), InstancedRecorder::RendererSpare );
}

// AND THE SPARE IS STILL RIGHT FOR EXACTLY ONE CASE. A mesh whose material slot did not resolve is
// drawn with MeshECSSystem's default material, which has no asset and therefore no sibling. It also has
// no authored texture to lose, which is the whole reason the spare is safe here and unsafe above.
TEST( InstancedRecorder, AGroupWithNoAssetIsRecordedByTheRendererSpare )
{
    EXPECT_EQ( SelectInstancedRecorder( /*groupHasAsset=*/false, /*assetVariantBuilt=*/false ),
               InstancedRecorder::RendererSpare );

    // `assetVariantBuilt` cannot be true without an asset, but a caller that passes it anyway must not
    // be handed a variant that belongs to nobody.
    EXPECT_EQ( SelectInstancedRecorder( false, true ), InstancedRecorder::RendererSpare );
}

// THE CELLS THAT MAKE THE FIX REACHABLE. `None` is the honest answer to a missing shader, but if it
// were the answer for the PBR surface in either pass the fix above would silently become "batched
// surfaces are no longer batched". Both cells exist; this is what fails if one is removed.
TEST( InstancedRecorder, BothInstancedCellsOfThePbrSurfaceExist )
{
    EXPECT_NE( MeshShaderFor( MeshVertexPath::Instanced, MeshPass::Forward ), nullptr );
    EXPECT_NE( MeshShaderFor( MeshVertexPath::Instanced, MeshPass::GBuffer ), nullptr );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
