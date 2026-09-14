// The MISSING-FILE branch of the loaders, which was DEAD CODE until 2026-09-05. Each of these
// loaders was written with a deliberate answer to "the file is not there":
//
//   - SurfaceMaterialAsset::Load: "New / empty material — canonical defaults; editable and
//     re-savable" (a success, by design — the editor creates materials by naming a file that does
//     not exist yet);
//   - CloudTypeAsset::Load: "a file that is missing ... is an ERROR carrying the reason" (its own
//     header says so);
//   - PrefabAsset::Load: a missing file is the read's own named error, an empty file is "Prefab
//     file is empty" — both errors (the scene loader logs and
//     survives — covered by the live editor run, not here, because PrefabAsset.cpp includes
//     Scene.hpp and no GPU-free suite can compile it).
//
// None of those branches could execute for a genuinely absent file, because
// FileSystem::ReadFileContent aborted the process before returning. These tests pin the branches
// now that the primitive is soft: each Load RETURNS (the suite being alive is half the assertion)
// and answers with exactly the policy its author wrote. Reverting the primitive's miss path to
// DESERT_VERIFY kills this suite outright.

#include <Engine/Assets/CloudTypeAsset.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Assets/Skybox/SkyboxAsset.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

namespace fs = std::filesystem;

namespace
{
    fs::path MissingPath( const char* name )
    {
        const fs::path dir = fs::temp_directory_path() / "desert_missing_asset_test";
        fs::remove_all( dir );
        fs::create_directories( dir );
        return dir / name; // never created
    }

    // A file that IS there, with the given bytes. The companion MissingPath needs: half of what these
    // loaders answer wrong is "the file is not there" and half is "the file is there and unusable",
    // and until this existed the second half had nothing to build.
    fs::path PathWith( const char* name, const std::string& content )
    {
        const fs::path path = MissingPath( name );
        std::ofstream  out( path, std::ios::binary );
        out << content;
        out.close();
        return path;
    }
} // namespace

TEST( AssetMissingFile, SurfaceMaterialLoadsCanonicalDefaults )
{
    const fs::path path = MissingPath( "brand_new.demat" );
    ASSERT_FALSE( fs::exists( path ) );

    Desert::Assets::SurfaceMaterialAsset material( Desert::Assets::AssetPriority::Medium, path );
    const auto                           result = material.Load();

    // The branch's own comment: a missing .demat is a NEW material — usable, editable, re-savable.
    EXPECT_TRUE( result.IsSuccess() );
    EXPECT_TRUE( material.IsReadyForUse() );
    // Canonical defaults: no authored parameters, and the shader falls back to the standard surface.
    EXPECT_TRUE( material.Data().Params.empty() );
    EXPECT_FALSE( material.Data().ShaderName.has_value() );
}

TEST( AssetMissingFile, CloudTypeLoadRefusesWithTheReason )
{
    const fs::path path = MissingPath( "gone.decloudtype" );
    ASSERT_FALSE( fs::exists( path ) );

    Desert::Assets::CloudTypeAsset type( Desert::Assets::AssetPriority::Medium, path );
    const auto                     result = type.Load();

    // The header's contract: missing is an ERROR carrying the reason — never a quiet default type.
    ASSERT_FALSE( result.IsSuccess() );
    EXPECT_NE( result.GetError().find( "empty or could not be opened" ), std::string::npos ) << result.GetError();
    EXPECT_FALSE( type.IsReadyForUse() );
}

// A missing .demat is a new material and a SUCCESS (above). A .demat that is THERE and will not parse is
// a different thing entirely, and the difference is the authored parameters: they are still in the file,
// and the moment anything writes this asset back they are gone. The loader's own message has always said
// so ("re-saving will overwrite the file") — it just had no way to stop it, because Save() handed out the
// substituted defaults like any other material's data.
TEST( AssetMissingFile, AnUnparseableMaterialLoadsUsableAndRefusesToSaveOverItsFile )
{
    const fs::path path = PathWith( "corrupt.demat", "{ this is not json" );

    Desert::Assets::SurfaceMaterialAsset material( Desert::Assets::AssetPriority::Medium, path );
    const auto                           result = material.Load();

    // Deliberately still a success and still usable: AssetManager::CreateAsset drops an asset whose
    // Load fails, so refusing here would delete the material from the asset database and leave every
    // mesh slot pointing at it resolving to nothing. See the comment on that branch.
    EXPECT_TRUE( result.IsSuccess() );
    EXPECT_TRUE( material.IsReadyForUse() );

    const auto saved = material.Save();
    ASSERT_FALSE( saved.IsSuccess() ) << "the material offered to serialize its substituted defaults; "
                                         "writing them out is the step that makes the loss permanent";
    EXPECT_NE( saved.GetError().find( path.string() ), std::string::npos )
         << "the refusal does not name the file the user has to fix: " << saved.GetError();

    fs::remove_all( path.parent_path() );
}

// The control for the test above: a material that parsed saves, or the refusal would be a material
// asset that can never be written at all.
TEST( AssetMissingFile, AParsedMaterialSavesNormally )
{
    const fs::path path = PathWith( "fine.demat", R"({"Params":[],"Textures":[]})" );

    Desert::Assets::SurfaceMaterialAsset material( Desert::Assets::AssetPriority::Medium, path );
    ASSERT_TRUE( material.Load().IsSuccess() );

    const auto saved = material.Save();
    EXPECT_TRUE( saved.IsSuccess() ) << saved.GetError();
    EXPECT_FALSE( saved.GetValue().empty() );

    fs::remove_all( path.parent_path() );
}

// A PARAMETER THAT IS NOT A NUMBER IS REFUSED BY NAME, and the alternative is not a bad file.
//
// `rfl::json::write` is `std::string( yyjson_mut_write( doc, 0, nullptr ) )`. yyjson with no write flags
// has no spelling for a NaN or an infinity and answers a NULL pointer — measured on the vendored copy:
// `{"v":0.5}` writes, NaN and inf both answer NULL — and that constructor then reads it. So the failure
// mode of saving such a material is undefined behaviour inside a third-party header while the editor is
// writing the artist's work, not a `.demat` somebody can repair.
//
// IT IS REACHABLE, AND NOT THROUGH THE FILE. yyjson refuses those tokens on the way IN as well, so no
// `.demat` on disk can carry one and no test needs to pretend otherwise — this one puts the value in
// through the same door the editor does, `MaterialData::SetParam`. The editor's door is an ImGui drag,
// whose Ctrl-click text entry parses with `sscanf( buf, "%f", … )`; `%f` accepts `nan`, `inf` and `1e40`.
TEST( AssetMissingFile, AMaterialHoldingANonNumberRefusesToSaveAndNamesTheParameter )
{
    const fs::path path = PathWith( "not_a_number.demat", R"({"Params":[],"Textures":[]})" );

    for ( const float bad : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::infinity() } )
    {
        Desert::Assets::SurfaceMaterialAsset material( Desert::Assets::AssetPriority::Medium, path );
        ASSERT_TRUE( material.Load().IsSuccess() );

        material.Data().SetParam( "Coverage", glm::vec4( bad, 0.0f, 0.0f, 0.0f ) );

        const auto saved = material.Save();
        ASSERT_FALSE( saved.IsSuccess() ) << "the writer was handed a value JSON cannot spell";
        EXPECT_NE( saved.GetError().find( "Coverage" ), std::string::npos )
             << "the refusal does not name the parameter the user has to fix: " << saved.GetError();
        EXPECT_NE( saved.GetError().find( path.string() ), std::string::npos ) << saved.GetError();
    }

    // The control, and it is not decorative: a guard that read the whole vec4 of every parameter would
    // refuse this one too, because a scalar's unused lanes are whatever the writer left in them. They are
    // zero here, which is what the editor writes — the point of the control is that an ORDINARY material
    // still saves after the guard exists.
    {
        Desert::Assets::SurfaceMaterialAsset material( Desert::Assets::AssetPriority::Medium, path );
        ASSERT_TRUE( material.Load().IsSuccess() );
        material.Data().SetParam( "Coverage", glm::vec4( 0.45f, 0.0f, 0.0f, 0.0f ) );
        const auto saved = material.Save();
        EXPECT_TRUE( saved.IsSuccess() ) << saved.GetError();
        EXPECT_NE( saved.GetValue().find( "Coverage" ), std::string::npos );
    }

    fs::remove_all( path.parent_path() );
}

// A brand-new material (no file at all) must STILL save — that is how the editor creates one, and a
// refusal here would make "New Material" impossible.
TEST( AssetMissingFile, ABrandNewMaterialSavesNormally )
{
    const fs::path path = MissingPath( "brand_new_saves.demat" );

    Desert::Assets::SurfaceMaterialAsset material( Desert::Assets::AssetPriority::Medium, path );
    ASSERT_TRUE( material.Load().IsSuccess() );

    EXPECT_TRUE( material.Save().IsSuccess() );

    fs::remove_all( path.parent_path() );
}

// SkyboxAsset::Load was `m_ReadyForUse = true; return BOOLSUCCESS;` with its only check commented out —
// it never opened the file it named. A skybox whose .hdr had been moved, renamed or left out of a
// package therefore loaded, registered and reported ready, and the sky came out black with every
// diagnostic in the editor saying the skybox was fine.
TEST( AssetMissingFile, SkyboxLoadRefusesAPanoramaThatIsNotThere )
{
    const fs::path path = MissingPath( "gone.hdr" );
    ASSERT_FALSE( fs::exists( path ) );

    Desert::Assets::SkyboxAsset skybox( Desert::Assets::AssetPriority::Medium, path );
    const auto                  result = skybox.Load();

    ASSERT_FALSE( result.IsSuccess() );
    EXPECT_NE( result.GetError().find( path.string() ), std::string::npos ) << result.GetError();
    EXPECT_FALSE( skybox.IsReadyForUse() );
}

// The control: a check that refused everything would satisfy the test above and make every skybox in
// the project unloadable.
TEST( AssetMissingFile, SkyboxLoadAcceptsAPanoramaThatIsThere )
{
    const fs::path path = PathWith( "present.hdr", "not really an HDR, and Load does not read it" );

    Desert::Assets::SkyboxAsset skybox( Desert::Assets::AssetPriority::Medium, path );
    EXPECT_TRUE( skybox.Load().IsSuccess() );
    EXPECT_TRUE( skybox.IsReadyForUse() );

    fs::remove_all( path.parent_path() );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
