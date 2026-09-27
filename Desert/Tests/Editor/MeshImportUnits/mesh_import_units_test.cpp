// WHAT UNIT AN IMPORTED FILE IS IN.
//
// The defect these tests pin: assimp normalises every file it reads to METRES, and the engine is
// centimetres. Its FBX reader calls SetFileScale( UnitScaleFactor * 0.01 ), so aiProcess_GlobalScale
// run with the default factor divided every centimetre-authored FBX by 100. Measured on the repository's
// own base.fbx — a 190 cm humanoid — before the fix: 1.1542 x 1.8983 x 0.3784 world units. After it:
// 115.42 x 189.83 x 37.84. Nothing in the log mentioned that a scale had been applied at all.
//
// The assertions below are about the RELATION rather than either side: the factor handed to assimp,
// multiplied by the metres-per-unit assimp itself worked out, must be the file's own
// centimetres-per-unit. Either side alone is a number that can be right while the import is wrong.

#include <Editor/Import/ImportUnits.hpp>

#include <Common/Core/Units.hpp>

#include <gtest/gtest.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string>

namespace
{
    using namespace Desert::Editor;

    std::filesystem::path RepositoryRoot()
    {
        std::filesystem::path here = std::filesystem::current_path();
        for ( int up = 0; up < 8 && !std::filesystem::exists( here / "BuildScripts" / "ThirdParty" ); ++up )
        {
            here = here.parent_path();
        }
        return here;
    }

    // assimp's own metres-per-file-unit for an FBX, straight out of FBXImporter.cpp:180:
    //     SetFileScale( UnitScaleFactor * 0.01 )
    // Stated here as the formula and not as a constant, because the point of the relation test is that
    // our factor cancels exactly this and nothing else.
    constexpr float AssimpMetresPerUnitForFbx( float statedCentimetresPerUnit )
    {
        return statedCentimetresPerUnit * 0.01f;
    }

    // Everything that actually reaches the geometry: assimp's ScaleProcess multiplies the scene by
    // ( AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY * AI_CONFIG_APP_SCALE_KEY ).
    float AppliedScale( float centimetresPerUnit, float assimpMetresPerUnit )
    {
        return ImportUnits::GlobalScaleFactor( centimetresPerUnit, assimpMetresPerUnit ) * assimpMetresPerUnit;
    }

    std::string ReadFile( const std::filesystem::path& path )
    {
        const std::ifstream in( path, std::ios::binary );
        std::ostringstream  out;
        out << in.rdbuf();
        return out.str();
    }

    // The extensions of one NAME row of BuildScripts/ThirdParty/AssimpImporters.txt. Rows read
    // `NAME  ext [ext ...]  reason...`, and the extensions are the words before the first word that
    // starts with a capital letter, which is where the prose begins — the same reading AssimpLibraryPin
    // uses, deliberately simple because the register is a file people edit by hand.
    std::set<std::string> RegisteredExtensions( const std::string& rowName )
    {
        std::set<std::string> extensions;
        std::istringstream    lines(
             ReadFile( RepositoryRoot() / "BuildScripts" / "ThirdParty" / "AssimpImporters.txt" ) );
        std::string line;
        while ( std::getline( lines, line ) )
        {
            std::istringstream words( line );
            std::string        name;
            if ( !( words >> name ) || name != rowName )
                continue;

            std::string word;
            while ( words >> word )
            {
                if ( word.empty() || ( std::isupper( static_cast<unsigned char>( word[0] ) ) != 0 ) )
                    break;
                extensions.insert( "." + word );
            }
            break;
        }
        return extensions;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// What the file said
// ---------------------------------------------------------------------------------------------------

TEST( MeshImportUnits, AFileThatStatesCentimetresIsNotRescaled )
{
    const ImportUnits::Scale unit = ImportUnits::Resolve( ".fbx", true, 1.0f );

    EXPECT_EQ( unit.From, ImportUnits::Source::StatedByFile );
    EXPECT_FLOAT_EQ( unit.CentimetresPerUnit, Common::Units::UnitsPerCm );
}

TEST( MeshImportUnits, AFileThatStatesMetresBecomesCentimetres )
{
    const ImportUnits::Scale unit = ImportUnits::Resolve( ".fbx", true, 100.0f );

    EXPECT_EQ( unit.From, ImportUnits::Source::StatedByFile );
    EXPECT_FLOAT_EQ( unit.CentimetresPerUnit, Common::Units::UnitsPerMetre );
}

TEST( MeshImportUnits, AFileThatStatesInchesBecomesCentimetres )
{
    // 2.54 is the FBX unit for inches, and it is here because it is the case that a metre/centimetre
    // pair of tests would let through: a rule that merely toggled between 1 and 100 passes both of
    // those and mis-scales this one.
    const ImportUnits::Scale unit = ImportUnits::Resolve( ".fbx", true, 2.54f );

    EXPECT_EQ( unit.From, ImportUnits::Source::StatedByFile );
    EXPECT_FLOAT_EQ( unit.CentimetresPerUnit, 2.54f );
}

TEST( MeshImportUnits, AFileThatStatesNothingIsTakenAsCentimetresAndTheReasonSaysSo )
{
    const ImportUnits::Scale unit = ImportUnits::Resolve( ".obj", false, 0.0f );

    EXPECT_EQ( unit.From, ImportUnits::Source::AssumedCentimetres )
         << "an OBJ carries no unit, and the importer must record that it ASSUMED one rather than "
            "letting the number read as something the file said.";
    EXPECT_FLOAT_EQ( unit.CentimetresPerUnit, Common::Units::UnitsPerCm );
    EXPECT_NE( std::string( ImportUnits::Describe( unit.From ) ).find( "assumed" ), std::string::npos )
         << "the log phrase for an assumed unit must say that it was assumed — this string is the whole "
            "of what stops a guess reading as a fact.";
}

TEST( MeshImportUnits, AStatedUnitThatIsNotAScaleIsRefusedRatherThanUsed )
{
    for ( const float bad :
          { 0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() } )
    {
        const ImportUnits::Scale unit = ImportUnits::Resolve( ".fbx", true, bad );

        EXPECT_EQ( unit.From, ImportUnits::Source::StatedButUnusable )
             << "stated value " << bad << " was accepted as a scale.";
        EXPECT_FLOAT_EQ( unit.CentimetresPerUnit, Common::Units::UnitsPerCm )
             << "a refused unit must still leave a usable scale behind, and the caller logs the number "
                "it refused.";
    }
}

// ---------------------------------------------------------------------------------------------------
// What the format says when the file does not
// ---------------------------------------------------------------------------------------------------

TEST( MeshImportUnits, TheGltfFamilyIsMetresByItsOwnSpecification )
{
    // glTF 2.0 §3.5: "The units for all linear distances are meters." No glTF file states a unit, so
    // without this branch every one of them would import a hundred times too small.
    for ( const char* extension : { ".gltf", ".glb", ".vrm", ".GLTF", ".Glb" } )
    {
        const ImportUnits::Scale unit = ImportUnits::Resolve( extension, false, 0.0f );

        EXPECT_EQ( unit.From, ImportUnits::Source::FixedByFormat ) << extension;
        EXPECT_FLOAT_EQ( unit.CentimetresPerUnit, Common::Units::UnitsPerMetre ) << extension;
    }
}

TEST( MeshImportUnits, TheRuleCoversExactlyTheFormatsTheRegisterShips )
{
    // The rule and BuildScripts/ThirdParty/AssimpImporters.txt are two statements of the same list, and
    // a format added to the register without a unit would otherwise be silently assumed to be
    // centimetres — the one outcome this whole file exists to make loud.
    const std::set<std::string> gltfRow = RegisteredExtensions( "GLTF" );
    ASSERT_FALSE( gltfRow.empty() ) << "the GLTF row of AssimpImporters.txt could not be read, so this "
                                       "comparison had nothing to compare against.";

    for ( const std::string& extension : gltfRow )
    {
        EXPECT_EQ( ImportUnits::Resolve( extension, false, 0.0f ).From, ImportUnits::Source::FixedByFormat )
             << extension
             << " is in the register's GLTF row but the unit rule does not treat it as "
                "glTF, so it would import a hundred times too small.";
    }

    for ( const std::string& extension : RegisteredExtensions( "OBJ" ) )
    {
        EXPECT_EQ( ImportUnits::Resolve( extension, false, 0.0f ).From, ImportUnits::Source::AssumedCentimetres )
             << extension << " states no unit, so it must be recorded as assumed.";
    }
}

// ---------------------------------------------------------------------------------------------------
// The relation: our factor and assimp's must multiply out to the file's own unit
// ---------------------------------------------------------------------------------------------------

TEST( MeshImportUnits, TheFactorHandedToAssimpMakesTheAppliedScaleTheFilesOwnUnit )
{
    for ( const float statedCentimetresPerUnit : { 1.0f, 100.0f, 2.54f, 0.1f, 1000.0f } )
    {
        const ImportUnits::Scale unit = ImportUnits::Resolve( ".fbx", true, statedCentimetresPerUnit );
        const float              assimpMetresPerUnit = AssimpMetresPerUnitForFbx( statedCentimetresPerUnit );

        EXPECT_NEAR( AppliedScale( unit.CentimetresPerUnit, assimpMetresPerUnit ), statedCentimetresPerUnit,
                     statedCentimetresPerUnit * 1e-5f )
             << "for a file stating " << statedCentimetresPerUnit
             << " cm per unit, the scale that actually reaches the geometry must be that same number.";
    }

    // glTF: assimp states no file scale for it, so its metres-per-unit stays 1.0 and the whole of the
    // conversion is ours.
    const ImportUnits::Scale gltf = ImportUnits::Resolve( ".gltf", false, 0.0f );
    EXPECT_FLOAT_EQ( AppliedScale( gltf.CentimetresPerUnit, 1.0f ), Common::Units::UnitsPerMetre );

    // OBJ: nothing is stated, nothing is applied.
    const ImportUnits::Scale obj = ImportUnits::Resolve( ".obj", false, 0.0f );
    EXPECT_FLOAT_EQ( AppliedScale( obj.CentimetresPerUnit, 1.0f ), Common::Units::UnitsPerCm );
}

TEST( MeshImportUnits, TheOldBehaviourShrankACentimetreFileByOneHundred )
{
    // This is the regression, stated as the arithmetic that produced it. Before the fix the importer
    // passed aiProcess_GlobalScale with AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY left at its default of 1.0,
    // so the applied scale was assimp's metres-per-unit alone — 0.01 for a centimetre-authored FBX.
    // base.fbx is exactly that file, and it imported 1.8983 units tall instead of 189.83.
    constexpr float kStatedByBaseFbx    = 1.0f; // base.fbx: GlobalSettings::UnitScaleFactor = 1.0
    const float     assimpMetresPerUnit = AssimpMetresPerUnitForFbx( kStatedByBaseFbx );

    EXPECT_FLOAT_EQ( AppliedScale( kStatedByBaseFbx, assimpMetresPerUnit ), 1.0f );

    const float oldAppliedScale = 1.0f * assimpMetresPerUnit;
    EXPECT_FLOAT_EQ( oldAppliedScale, 0.01f );

    // 189.8341 is the measured height of base.fbx in file units after its node transform is baked.
    constexpr float kHeightInFileUnits = 189.8341f;
    EXPECT_NEAR( kHeightInFileUnits * AppliedScale( kStatedByBaseFbx, assimpMetresPerUnit ), 189.8341f, 0.01f );
    EXPECT_NEAR( kHeightInFileUnits * oldAppliedScale, 1.8983f, 0.01f )
         << "if this stops reproducing the old 1.9, the arithmetic this suite is guarding against has "
            "changed and the fix needs re-deriving rather than re-measuring.";
}

TEST( MeshImportUnits, AScaleMustBeFiniteAndPositive )
{
    EXPECT_TRUE( ImportUnits::IsUsableScale( 1.0f ) );
    EXPECT_TRUE( ImportUnits::IsUsableScale( 0.01f ) );
    EXPECT_FALSE( ImportUnits::IsUsableScale( 0.0f ) );
    EXPECT_FALSE( ImportUnits::IsUsableScale( -1.0f ) );
    EXPECT_FALSE( ImportUnits::IsUsableScale( std::numeric_limits<float>::quiet_NaN() ) );
    EXPECT_FALSE( ImportUnits::IsUsableScale( std::numeric_limits<float>::infinity() ) );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
