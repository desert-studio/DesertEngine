// Any static mesh is a Modeling tool target (P9). The relations pinned:
//  - the lift of an asset-only entity IS the P7 round trip (ReadMeshAssetData + DynamicMeshFromMeshAssetData);
//  - an entity with an EditableMesh targets that very object, and it is also the undo "before";
//  - a lift's undo "before" is null, and every undo path plans "drop the EditableMesh" for it;
//  - the lift is cached by identity until the .stmesh is rewritten;
//  - no Modeling tool reads StaticMeshComponent::EditableMesh itself (census over the tool sources).

#include <Editor/Core/Selection/ModelingToolTarget.hpp>

#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Geometry/DynamicMeshAsset.hpp>
#include <Engine/Geometry/DynamicMeshRenderConversion.hpp>
#include <Engine/Geometry/DynamicMeshSerialization.hpp>
#include <Engine/Geometry/EditMeshConversion.hpp>
#include <Engine/Geometry/EditMeshSerialization.hpp>
#include <Engine/Geometry/ShapeGenerators.hpp>

#include <gtest/gtest.h>

#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace Desert;
using Editor::MeshRestore;
using MeshPtr = std::shared_ptr<const Geometry::FDynamicMesh3>;

namespace
{
    MeshPtr Box( int subdivisions )
    {
        auto shape = Geometry::MakeBox( glm::vec3( 200.0f, 100.0f, 50.0f ), glm::ivec3( subdivisions, 1, 1 ) );
        auto edit  = Geometry::ShapeToEditMesh( shape );
        EXPECT_TRUE( edit.IsSuccess() );
        auto dyn = Geometry::DynamicMeshFromSerialized( Geometry::ToSerialized( edit.GetValue() ), "box" );
        EXPECT_TRUE( dyn.IsSuccess() ) << ( dyn.IsSuccess() ? "" : dyn.GetError() );
        return std::make_shared<const Geometry::FDynamicMesh3>( dyn.ExtractValue() );
    }

    std::string Bytes( const MeshPtr& mesh )
    {
        const std::vector<Common::UUID> slots = { Common::UUID( 1111 ) };
        auto                            data  = Geometry::DynamicMeshToMeshAssetData( *mesh, slots );
        EXPECT_TRUE( data.IsSuccess() ) << ( data.IsSuccess() ? "" : data.GetError() );
        return Assets::Serialization::EncodeMeshBinary( data.GetValue() );
    }

    void WriteFile( const fs::path& path, const std::string& bytes )
    {
        std::ofstream out( path, std::ios::binary | std::ios::trunc );
        out << bytes;
    }

    // Two meshes are the same when they render the same arrays.
    void ExpectSameRender( const Geometry::FDynamicMesh3& a, const Geometry::FDynamicMesh3& b )
    {
        auto ra = Geometry::ToRenderMesh( a );
        auto rb = Geometry::ToRenderMesh( b );
        ASSERT_TRUE( ra.IsSuccess() && rb.IsSuccess() );
        ASSERT_EQ( ra.GetValue().Indices.size(), rb.GetValue().Indices.size() );
        ASSERT_EQ( ra.GetValue().Vertices.size(), rb.GetValue().Vertices.size() );
        for ( size_t i = 0; i < ra.GetValue().Vertices.size(); ++i )
            EXPECT_EQ( ra.GetValue().Vertices[i].Position, rb.GetValue().Vertices[i].Position ) << i;
    }

    class ToolTarget : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            m_Dir =
                 fs::temp_directory_path() / ( std::string( "ModelingToolTarget_" ) +
                                               ::testing::UnitTest::GetInstance()->current_test_info()->name() );
            fs::remove_all( m_Dir );
            fs::create_directories( m_Dir );
            m_File = m_Dir / "Box.stmesh";
        }
        void TearDown() override
        {
            std::error_code ec;
            fs::remove_all( m_Dir, ec );
        }
        fs::path m_Dir;
        fs::path m_File;
    };
} // namespace

TEST_F( ToolTarget, TheLiftOfAnAssetOnlyEntityIsTheP7RoundTrip )
{
    const std::string bytes = Bytes( Box( 2 ) );
    WriteFile( m_File, bytes );

    auto target = Editor::GetToolTargetMeshAt( nullptr, m_File );
    ASSERT_TRUE( target.IsSuccess() ) << target.GetError();
    ASSERT_TRUE( target.GetValue().Mesh );
    EXPECT_EQ( target.GetValue().Committed, nullptr ) << "an asset-only entity holds no EditableMesh to restore";

    auto data = Assets::Serialization::ReadMeshAssetData( bytes, "reference" );
    ASSERT_TRUE( data.IsSuccess() ) << data.GetError();
    auto reference = Geometry::DynamicMeshFromMeshAssetData( data.GetValue() );
    ASSERT_TRUE( reference.IsSuccess() ) << reference.GetError();
    EXPECT_EQ( target.GetValue().Mesh->TriangleCount(), reference.GetValue().TriangleCount() );
    EXPECT_EQ( target.GetValue().Mesh->VertexCount(), reference.GetValue().VertexCount() );
    ExpectSameRender( *target.GetValue().Mesh, reference.GetValue() );
}

TEST_F( ToolTarget, AnEditableMeshIsItsOwnTargetAndItsOwnUndoBefore )
{
    WriteFile( m_File, Bytes( Box( 2 ) ) );
    const MeshPtr editable = Box( 3 );
    auto          target   = Editor::GetToolTargetMeshAt( editable, m_File );
    ASSERT_TRUE( target.IsSuccess() ) << target.GetError();
    EXPECT_EQ( target.GetValue().Mesh, editable ) << "the asset must not shadow the EditableMesh";
    EXPECT_EQ( target.GetValue().Committed, editable );
}

TEST_F( ToolTarget, NothingToLiftIsRefusedByName )
{
    auto none = Editor::GetToolTargetMeshAt( nullptr, {} );
    ASSERT_FALSE( none.IsSuccess() );
    EXPECT_NE( none.GetError().find( "no static mesh asset" ), std::string::npos ) << none.GetError();
    auto missing = Editor::GetToolTargetMeshAt( nullptr, m_Dir / "Gone.stmesh" );
    ASSERT_FALSE( missing.IsSuccess() );
    EXPECT_NE( missing.GetError().find( "Gone.stmesh" ), std::string::npos ) << missing.GetError();
}

TEST_F( ToolTarget, TheLiftIsTheSameObjectUntilTheFileIsRewritten )
{
    WriteFile( m_File, Bytes( Box( 2 ) ) );
    auto first  = Editor::GetToolTargetMeshAt( nullptr, m_File );
    auto second = Editor::GetToolTargetMeshAt( nullptr, m_File );
    ASSERT_TRUE( first.IsSuccess() && second.IsSuccess() );
    EXPECT_EQ( first.GetValue().Mesh, second.GetValue().Mesh ) << "the element selection tracks by identity";

    const auto before = fs::last_write_time( m_File );
    WriteFile( m_File, Bytes( Box( 4 ) ) );
    fs::last_write_time( m_File, before + std::chrono::seconds( 2 ) );
    auto third = Editor::GetToolTargetMeshAt( nullptr, m_File );
    ASSERT_TRUE( third.IsSuccess() ) << third.GetError();
    EXPECT_NE( third.GetValue().Mesh, first.GetValue().Mesh );
    EXPECT_NE( third.GetValue().Mesh->TriangleCount(), first.GetValue().Mesh->TriangleCount() )
         << "a rewritten .stmesh must lift again, not serve the old mesh";
}

// The undo plan, walked through the states an edit leaves: commit, undo, redo. EditMeshCommand, XformCommand
// (changed and deleted entities) and the split rollback all decide through PlanMeshRestore; the commands
// themselves upload to the device (ECS::SetEditableMesh) and are exercised in the editor frame.
TEST( ToolTargetUndo, AnEditOnALiftCommitsAnEditableMeshAndItsUndoDropsIt )
{
    const MeshPtr edited = Box( 2 );
    const MeshPtr lifted = nullptr; // ToolTargetMesh::Committed of a lift
    EXPECT_EQ( Editor::PlanMeshRestore( nullptr, edited ), MeshRestore::Set ) << "commit / redo";
    EXPECT_EQ( Editor::PlanMeshRestore( edited, lifted ), MeshRestore::Clear ) << "undo returns to the asset";
    EXPECT_EQ( Editor::PlanMeshRestore( nullptr, lifted ), MeshRestore::Unchanged )
         << "a deleted asset-only part restored from its snapshot needs no mesh";
}

TEST( ToolTargetUndo, AnEditOnAnEditableMeshRestoresTheOriginal )
{
    const MeshPtr original = Box( 2 );
    const MeshPtr edited   = Box( 3 );
    EXPECT_EQ( Editor::PlanMeshRestore( edited, original ), MeshRestore::Set );
    EXPECT_EQ( Editor::PlanMeshRestore( original, original ), MeshRestore::Unchanged );
}

// ---- Census: no Modeling tool reads StaticMeshComponent::EditableMesh itself. ----
namespace
{
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up, prefix += "../" )
            if ( fs::exists( prefix + "Editor/Source/Editor/Core/Selection/ModelingToolTarget.hpp" ) )
                return prefix;
        return {};
    }

    std::string StripComments( const std::string& text )
    {
        std::string out;
        for ( size_t i = 0; i < text.size(); ++i )
        {
            if ( text.compare( i, 2, "//" ) == 0 )
            {
                while ( i < text.size() && text[i] != '\n' )
                    ++i;
            }
            else if ( text.compare( i, 2, "/*" ) == 0 )
            {
                const size_t end = text.find( "*/", i + 2 );
                i                = end == std::string::npos ? text.size() : end + 1;
                continue;
            }
            if ( i < text.size() )
                out += text[i];
        }
        return out;
    }

    // Member reads of EditableMesh (`.EditableMesh` / `->EditableMesh`); the ECS setters are calls, not reads.
    int CountReads( const std::string& source )
    {
        const std::string code  = StripComments( source );
        const std::string name  = "EditableMesh";
        int               count = 0;
        for ( size_t at = code.find( name ); at != std::string::npos; at = code.find( name, at + 1 ) )
        {
            const bool dot    = at >= 1 && code[at - 1] == '.';
            const bool arrow  = at >= 2 && code.compare( at - 2, 2, "->" ) == 0;
            const char next   = at + name.size() < code.size() ? code[at + name.size()] : ' ';
            const bool suffix = std::isalnum( static_cast<unsigned char>( next ) ) || next == '_';
            if ( ( dot || arrow ) && !suffix )
                ++count;
        }
        return count;
    }

    // The register: one row per file allowed to read the member, with its reason.
    const std::map<std::string, std::string> kAllowed = {
         { "Core/Selection/ModelingToolTargetAsset.cpp", "it IS the rule: GetToolTargetMesh reads the component" },
         { "Core/Selection/MeshSelectionOperations.cpp",
           "the split rollback plans its restore from what the component holds after the failed record" },
    };
} // namespace

TEST( ModelingToolTargetCensus, TheRuleSeesAReadAndIgnoresCommentsAndSetters )
{
    EXPECT_EQ( CountReads( "auto m = smc.EditableMesh;" ), 1 );
    EXPECT_EQ( CountReads( "auto m = target->EditableMesh;" ), 1 );
    EXPECT_EQ( CountReads( "// smc.EditableMesh\n/* x.EditableMesh */ ECS::SetEditableMesh( smc, m );" ), 0 );
    EXPECT_EQ( CountReads( "ECS::ClearEditableMesh( smc ); smc.EditableMeshes;" ), 0 );
}

TEST( ModelingToolTargetCensus, NoModelingToolReadsTheEditableMeshItself )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from " << fs::current_path();
    const fs::path           editor = fs::path( root ) / "Editor/Source/Editor";
    int                      files  = 0;
    std::vector<std::string> seenAllowed;
    for ( const char* dir : { "Panels/ViewportPanel/Tools", "Core/Selection", "Panels/Modeling" } )
        for ( const auto& entry : fs::directory_iterator( editor / dir ) )
        {
            const auto ext = entry.path().extension();
            if ( ext != ".cpp" && ext != ".hpp" )
                continue;
            ++files;
            const std::string rel = fs::relative( entry.path(), editor ).generic_string();
            std::ifstream     in( entry.path(), std::ios::binary );
            std::stringstream ss;
            ss << in.rdbuf();
            const int reads = CountReads( ss.str() );
            if ( kAllowed.count( rel ) )
            {
                seenAllowed.push_back( rel );
                EXPECT_GT( reads, 0 ) << rel << " is registered but reads nothing: remove its row";
                continue;
            }
            EXPECT_EQ( reads, 0 ) << rel << " reads StaticMeshComponent::EditableMesh " << reads
                                  << " time(s): take the target through GetToolTargetMesh";
        }
    EXPECT_GT( files, 20 ) << "the census saw too few sources to mean anything";
    EXPECT_EQ( seenAllowed.size(), kAllowed.size() ) << "a registered file is gone: remove its row";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
