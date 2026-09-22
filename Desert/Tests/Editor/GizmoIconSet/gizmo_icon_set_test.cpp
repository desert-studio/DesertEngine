// ── THE GIZMO ICON CENSUS ──────────────────────────────────────────────────────────────────────────
//
// Ten .svg files were downloaded, licensed and made parseable, and for a day NOTHING READ THEM: the
// viewport billboards were still font glyphs. That is the failure this suite is written against, and
// it has a shape worth naming — the artwork, the parser and the call sites each looked correct in
// isolation, and the property that mattered lived in the relation between them.
//
// So the rows here are all RELATIONS, not values:
//
//   1. every role in the table has a file on disk           (code -> artwork)
//   2. every .svg in the directory has a role in the table  (artwork -> code)
//   3. every one of them is a DUOTONE                        (the property the wiring depends on)
//   4. the MIT licence is still beside them                  (the reason we may ship them at all)
//
// ROW 3 IS THE LOAD-BEARING ONE. `Editor/Core/GizmoIconSet.hpp` says the gizmos read as objects rather
// than symbols because each icon bakes into TWO colour runs, and `LightGizmoRenderer` multiplies each
// run's alpha into the tint on that basis. Replace one file with a flat single-tone icon and every
// statement above stays true while the picture quietly becomes what it was before — no compile error,
// no crash, no visual alarm at 30 px. Nothing but this row can notice.
//
// It reads the FILES IN THE REPOSITORY (located from the repo root, not from the working directory),
// and every one of them is tracked by git, so a green run here is a statement about the repository and
// not about one machine.

#include <Editor/Core/GizmoIconSet.hpp>

#include <Engine/Vector/VectorImage.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    fs::path RepoRoot()
    {
        fs::path here = fs::current_path();
        for ( int up = 0; up < 6; ++up )
        {
            if ( fs::exists( here / ".gitignore" ) && fs::exists( here / "Desert" ) )
                return here;
            here = here.parent_path();
        }
        return {};
    }

    // The editor resolves Resources/... against ITS working directory, which is the Editor/ folder.
    // Derived from GizmoIconPath so the suite and the editor cannot disagree about where the artwork is.
    fs::path IconPathInRepo( Desert::Editor::GizmoIcon role )
    {
        return RepoRoot() / "Editor" / Desert::Editor::GizmoIconPath( role );
    }

    fs::path GizmoIconDirInRepo()
    {
        return IconPathInRepo( Desert::Editor::GizmoIcon::LightPoint ).parent_path();
    }

    std::vector<uint8_t> ReadBytes( const fs::path& path )
    {
        std::ifstream in( path, std::ios::binary );
        if ( !in )
            return {};
        return std::vector<uint8_t>( std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() );
    }
} // namespace

TEST( GizmoIconSet, TheRepositoryIsWhereThisSuiteThinksItIs )
{
    // A NEGATIVE CONTROL FOR THE OTHER THREE. If RepoRoot() answered nothing, every row below would
    // fail for one uninteresting reason and the real findings would be unreadable among them.
    ASSERT_FALSE( RepoRoot().empty() ) << "could not locate the repository root from " << fs::current_path();
    ASSERT_TRUE( fs::is_directory( GizmoIconDirInRepo() ) )
         << GizmoIconDirInRepo() << " is not a directory — the gizmo artwork has moved";
}

TEST( GizmoIconSet, EveryRoleHasArtwork )
{
    for ( const Desert::Editor::GizmoIconRow& row : Desert::Editor::kGizmoIcons )
    {
        const fs::path path = IconPathInRepo( row.Role );
        EXPECT_TRUE( fs::exists( path ) ) << "the '" << row.Label << "' gizmo has no artwork at " << path;
    }
}

TEST( GizmoIconSet, EveryFileHasARole )
{
    // THE OTHER DIRECTION, and it is not redundant. Without it a file can be added to the directory,
    // shipped, licensed and packaged while no billboard ever draws it — which is precisely the state
    // all ten of these were in before they were wired.
    std::set<std::string> claimed;
    for ( const Desert::Editor::GizmoIconRow& row : Desert::Editor::kGizmoIcons )
        claimed.insert( row.File );

    for ( const auto& entry : fs::directory_iterator( GizmoIconDirInRepo() ) )
    {
        if ( entry.path().extension() != ".svg" )
            continue;
        EXPECT_TRUE( claimed.count( entry.path().filename().string() ) == 1 )
             << entry.path().filename() << " is in the gizmo icon directory and no role draws it";
    }
}

TEST( GizmoIconSet, EveryGizmoIconIsADuotone )
{
    for ( const Desert::Editor::GizmoIconRow& row : Desert::Editor::kGizmoIcons )
    {
        const fs::path             path = IconPathInRepo( row.Role );
        const std::vector<uint8_t> svg  = ReadBytes( path );
        ASSERT_FALSE( svg.empty() ) << "cannot read " << path;

        const Desert::Vector::VectorImage image =
             Desert::Vector::ParseSvg( reinterpret_cast<const char*>( svg.data() ), svg.size() );
        ASSERT_TRUE( image.Valid() ) << path << " has no shapes this importer understands";

        // COUNT THE COLOUR RUNS, which is exactly what IconBake turns into layers: consecutive shapes
        // sharing a fill collapse into one. Two or more runs is what "duotone" means to this engine, and
        // what makes the faint body and the solid outline reach the viewport as separate tinted draws.
        size_t   runs = 0;
        uint32_t previous = 0;
        for ( size_t i = 0; i < image.Shapes.size(); ++i )
        {
            if ( i == 0 || image.Shapes[i].FillRGBA != previous )
                ++runs;
            previous = image.Shapes[i].FillRGBA;
        }
        EXPECT_GE( runs, 2u ) << path << " bakes into " << runs
                              << " colour run(s): it is not a duotone, and the gizmo drawn from it is a "
                                 "flat symbol again";
    }
}

TEST( GizmoIconSet, TheLicenceIsStillBesideTheArtwork )
{
    // We may ship these files because they are MIT. A directory whose licence text went missing in a
    // move is a legal defect that nothing else in the build can see.
    const fs::path licence = GizmoIconDirInRepo() / "LICENSE-MIT-Phosphor.txt";
    ASSERT_TRUE( fs::exists( licence ) ) << "the Phosphor MIT licence is missing from " << licence;
    EXPECT_GT( fs::file_size( licence ), 0u ) << licence << " is empty";
}
