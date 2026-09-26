#include "FbxMeshSplitter.hpp"

// First: the JSON facade pulls reflect-cpp, which must precede anything that could include <windows.h>.
#include <Editor/Panels/Collections/CollectionManifest.hpp>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

// Header-only: the shared source-format priority list. This tool still links no engine code; the list
// lives with the editor's import pipeline because AssimpImporter reads the SAME order, and two copies
// of one priority had already drifted apart once (this file preferred TGA, the importer preferred JPG).
#include <Editor/Import/TextureSourceFormats.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace FbxSplit
{
    namespace
    {
        // Filesystem-safe name (keeps letters/digits/_-, collapses the rest to '_').
        std::string Sanitize( const std::string& in )
        {
            std::string out;
            out.reserve( in.size() );
            for ( char c : in )
                out += ( std::isalnum( static_cast<unsigned char>( c ) ) || c == '-' || c == '_' ) ? c : '_';
            if ( out.empty() )
                out = "mesh";
            return out;
        }

        // Walk up from the FBX until a directory literally named "Resources" is found. Its parent is the
        // project working dir the engine launches from, so manifest paths come out "Resources/Mesh/...".
        // Returns false if there's no such ancestor (caller falls back to the FBX's own folder).
        bool FindResourcesRoot( const std::filesystem::path& fbxAbs, std::filesystem::path& resourcesDir )
        {
            for ( std::filesystem::path p = fbxAbs.parent_path(); !p.empty() && p != p.root_path();
                  p                       = p.parent_path() )
            {
                if ( p.filename() == "Resources" )
                {
                    resourcesDir = p;
                    return true;
                }
            }
            return false;
        }

        std::string ToLower( std::string s )
        {
            for ( char& c : s )
                c = static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );
            return s;
        }

        bool EndsWith( const std::string& s, const std::string& suffix )
        {
            return s.size() >= suffix.size() && s.compare( s.size() - suffix.size(), suffix.size(), suffix ) == 0;
        }

        // PBR texture slots we recognise from filename suffixes (Poly Haven / industry convention).
        enum class MapType
        {
            None,
            Albedo,
            Opacity,
            Normal,
            Roughness,
            Metallic,
            AO
        };

        // Strip a trailing resolution token like "_4k"/"_2k"/"_1k"/"_8k" (case-insensitive, lowercased input).
        std::string StripResolution( const std::string& stem )
        {
            const size_t us = stem.rfind( '_' );
            if ( us == std::string::npos || us + 2 > stem.size() )
                return stem;
            const std::string tok = stem.substr( us + 1 );
            if ( tok.back() != 'k' )
                return stem;
            for ( size_t i = 0; i + 1 < tok.size(); ++i )
                if ( !std::isdigit( static_cast<unsigned char>( tok[i] ) ) )
                    return stem;
            return stem.substr( 0, us );
        }

        // Classify a texture by its filename suffix and return both the map type and the "material stem"
        // (the shared base name once the map suffix + resolution token are removed). E.g.
        // "grass_medium_01_nor_gl_4k" -> { Normal, "grass_medium_01" }. Longest suffixes are tested first so
        // "_nor_gl" wins over "_nor". `_arm`/`_orm` (channel-packed) are intentionally NOT handled here.
        MapType ClassifyTexture( const std::string& fileStemLower, std::string& materialStemOut )
        {
            const std::string base = StripResolution( fileStemLower );

            // Ordered longest/most-specific first.
            static const std::pair<const char*, MapType> kSuffixes[] = {
                 { "_nor_gl", MapType::Normal },
                 { "_nor_dx", MapType::Normal },
                 { "_normal", MapType::Normal },
                 { "_nrm", MapType::Normal },
                 { "_nor", MapType::Normal },
                 { "_roughness", MapType::Roughness },
                 { "_rough", MapType::Roughness },
                 { "_rgh", MapType::Roughness },
                 { "_metalness", MapType::Metallic },
                 { "_metallic", MapType::Metallic },
                 { "_metal", MapType::Metallic },
                 { "_basecolor", MapType::Albedo },
                 { "_albedo", MapType::Albedo },
                 { "_diffuse", MapType::Albedo },
                 { "_diff", MapType::Albedo },
                 { "_color", MapType::Albedo },
                 { "_col", MapType::Albedo },
                 { "_alb", MapType::Albedo },
                 { "_opacity", MapType::Opacity },
                 { "_alpha", MapType::Opacity },
                 { "_mask", MapType::Opacity },
                 { "_opac", MapType::Opacity },
                 { "_ao", MapType::AO },
            };
            for ( const auto& [suffix, type] : kSuffixes )
            {
                if ( EndsWith( base, suffix ) )
                {
                    materialStemOut = base.substr( 0, base.size() - std::strlen( suffix ) );
                    return type;
                }
            }
            return MapType::None;
        }

        // Lower rank = preferred when the same map exists in several formats. The order (and its
        // rationale) is the shared list in Editor/Import/TextureSourceFormats.hpp — lossless first,
        // lossy after, extended-range last; kNotAnImage means "not an image source we use".
        constexpr std::size_t kNotAnImage = Desert::Editor::kTextureSourceExtensionCount;

        std::size_t FormatRank( const std::string& extLower )
        {
            return Desert::Editor::TextureSourceFormatRank( extLower );
        }

        struct MaterialDef
        {
            std::string Stem; // shared base name (also the material Name)
            // `= {}` and not merely "they default anyway": every slot below is OPTIONAL, and the
            // aggregate initialisations of this struct name only the stem. Stating it at the declaration
            // is what makes the empty ones a decision rather than a truncated initialiser list.
            std::string Albedo = {}, Opacity = {}, Normal = {}, Roughness = {}, Metallic = {}, AO = {};
            std::string AlbedoExt = {}, OpacityExt = {}, NormalExt = {}, RoughnessExt = {};
            std::string MetallicExt = {}, AOExt = {};
        };

        // Scan the collection folder (+ a "textures" subdir) for PBR maps, group by material stem, pick the
        // best format per slot. Returns the materials in deterministic (insertion) order.
        std::vector<MaterialDef> ScanMaterials( const std::filesystem::path& collDir,
                                                const std::filesystem::path& projectDir )
        {
            std::vector<MaterialDef> mats;
            std::error_code          ec;

            const std::filesystem::path dirs[] = { collDir, collDir / "textures" };
            for ( const auto& dir : dirs )
            {
                if ( !std::filesystem::is_directory( dir, ec ) )
                    continue;
                for ( const auto& entry : std::filesystem::directory_iterator( dir, ec ) )
                {
                    if ( !entry.is_regular_file( ec ) )
                        continue;
                    const std::string extLower = ToLower( entry.path().extension().string() );
                    if ( FormatRank( extLower ) == kNotAnImage )
                        continue;

                    std::string   materialStem;
                    const MapType type = ClassifyTexture( ToLower( entry.path().stem().string() ), materialStem );
                    if ( type == MapType::None || materialStem.empty() )
                        continue;

                    std::filesystem::path rel     = std::filesystem::relative( entry.path(), projectDir, ec );
                    const std::string     relPath = ec ? entry.path().generic_string() : rel.generic_string();

                    MaterialDef* def = nullptr;
                    for ( auto& m : mats )
                        if ( m.Stem == materialStem )
                        {
                            def = &m;
                            break;
                        }
                    if ( !def )
                    {
                        mats.push_back( MaterialDef{ materialStem } );
                        def = &mats.back();
                    }

                    // Assign, keeping the better format if this slot is already filled.
                    auto take = [&]( std::string& slot, std::string& slotExt )
                    {
                        if ( slot.empty() || FormatRank( extLower ) < FormatRank( slotExt ) )
                        {
                            slot    = relPath;
                            slotExt = extLower;
                        }
                    };
                    switch ( type )
                    {
                        case MapType::Albedo:
                            take( def->Albedo, def->AlbedoExt );
                            break;
                        case MapType::Opacity:
                            take( def->Opacity, def->OpacityExt );
                            break;
                        case MapType::Normal:
                            take( def->Normal, def->NormalExt );
                            break;
                        case MapType::Roughness:
                            take( def->Roughness, def->RoughnessExt );
                            break;
                        case MapType::Metallic:
                            take( def->Metallic, def->MetallicExt );
                            break;
                        case MapType::AO:
                            take( def->AO, def->AOExt );
                            break;
                        default:
                            break;
                    }
                }
            }
            return mats;
        }

        // Case-insensitive longest-prefix match of a mesh Category against the material stems. With one
        // material (the common atlas case) every mesh maps to material 0. Returns -1 if there are none.
        int BestMaterialForCategory( const std::string& category, const std::vector<MaterialDef>& mats )
        {
            if ( mats.empty() )
                return -1;
            const std::string cat     = ToLower( category );
            int               best    = 0;
            size_t            bestLen = 0;
            for ( size_t i = 0; i < mats.size(); ++i )
            {
                const std::string& stem = mats[i].Stem; // already lowercased
                if ( cat.compare( 0, stem.size(), stem ) == 0 && stem.size() > bestLen )
                {
                    best    = static_cast<int>( i );
                    bestLen = stem.size();
                }
            }
            return best;
        }

        // THE WRITE VERDICT, IN ONE PLACE FOR THIS TOOL (Д35).
        //
        // Both writers below go through this, and this is the only place a stream is decided about. The
        // rest of the tree writes files through Common::Utils::FileSystem::WriteContentToFileAtomic —
        // there is exactly one write primitive and that is it — but this tool deliberately links NO
        // engine code (see FbxMeshSplitter's premake5.lua: Assimp plus one header-only Editor constant
        // table, and the file list is what CHECKS that). Reaching the primitive means linking libCommon,
        // and with it spdlog, the JobSystem and, on macOS, AppKit and the Objective-C runtime, into a
        // splitter that needs none of them.
        //
        // What is NOT out of reach is the RULE the primitive exists for: close() explicitly and test the
        // stream AFTER the close. `write()` fills the filebuf; whether the bytes reached the disk is
        // known only at the flush, and without this close that flush is ~ofstream, running after the
        // function has already returned true. What is given up by not being the primitive is atomicity,
        // and it is given up knowingly: this tool regenerates its whole output directory, so a failed
        // run is re-run rather than recovered from.
        bool WriteWholeFile( const std::filesystem::path& path, const std::string& content, std::string& error )
        {
            std::ofstream out( path, std::ios::binary | std::ios::trunc );
            if ( !out )
            {
                error = "could not open " + path.string() + " for writing";
                return false;
            }

            out.write( content.data(), static_cast<std::streamsize>( content.size() ) );
            out.close();
            if ( !out )
            {
                error = "could not write " + std::to_string( content.size() ) + " bytes to " + path.string();
                return false;
            }
            return true;
        }

        // Write one aiMesh as a Wavefront .obj, with the node WORLD transform `xf` baked into the geometry
        // (FBX stores tiny local-space verts + the real size/orientation in the node transform), then recenter
        // so the clump sits at the origin with its base at Y=0 — ideal for foliage instance placement. Indices
        // are 1-based and shared across v/vt/vn (one of each per vertex, same order), so face k -> vertex k+1.
        bool WriteObj( const aiMesh* mesh, const aiMatrix4x4& xf, float scale,
                       const std::filesystem::path& outPath, const std::string& objName, std::string& error )
        {
            // Built whole in memory, then written once through WriteWholeFile. It used to stream straight
            // into a local std::ofstream that nobody closed or re-tested, so a short .obj went out as a
            // success — and this tool PRODUCES the corpus other tasks measure on, which makes a silently
            // truncated mesh a wrong measurement with nothing pointing back at its cause.
            //
            // WHAT THE BUFFER COSTS, measured rather than guessed: Editor/Resources/Assets/Meshes/base.fbx
            // produces a 36.0 MB .obj, so that is the peak this holds on the biggest mesh in the
            // repository. It is paid for uniformity — one place in this tool decides whether a write
            // happened — and it is small beside the Assimp scene already resident. If a mesh ever makes
            // that the wrong trade, the fix is to stream and close-and-check here too, not to go back to
            // deciding in two places.
            std::ostringstream f;

            const bool hasUV     = mesh->HasTextureCoords( 0 );
            const bool hasNormal = mesh->HasNormals();

            // Transform positions by the full matrix; find the bbox to recenter (XZ center, Y base).
            std::vector<aiVector3D> pos( mesh->mNumVertices );
            aiVector3D              mn( std::numeric_limits<float>::max() );
            aiVector3D              mx( -std::numeric_limits<float>::max() );
            for ( unsigned i = 0; i < mesh->mNumVertices; ++i )
            {
                const auto& v = mesh->mVertices[i];
                pos[i].x      = xf.a1 * v.x + xf.a2 * v.y + xf.a3 * v.z + xf.a4;
                pos[i].y      = xf.b1 * v.x + xf.b2 * v.y + xf.b3 * v.z + xf.b4;
                pos[i].z      = xf.c1 * v.x + xf.c2 * v.y + xf.c3 * v.z + xf.c4;
                mn.x          = std::min( mn.x, pos[i].x );
                mn.y          = std::min( mn.y, pos[i].y );
                mn.z          = std::min( mn.z, pos[i].z );
                mx.x          = std::max( mx.x, pos[i].x );
                mx.y          = std::max( mx.y, pos[i].y );
                mx.z          = std::max( mx.z, pos[i].z );
            }
            const aiVector3D offset( ( mn.x + mx.x ) * 0.5f, mn.y, ( mn.z + mx.z ) * 0.5f );

            f << "# Exported by DesertEngine FbxMeshSplitter\n";
            f << "o " << objName << '\n';

            for ( unsigned i = 0; i < mesh->mNumVertices; ++i )
                f << "v " << ( ( pos[i].x - offset.x ) * scale ) << ' ' << ( ( pos[i].y - offset.y ) * scale )
                  << ' ' << ( ( pos[i].z - offset.z ) * scale ) << '\n';
            if ( hasUV )
                for ( unsigned i = 0; i < mesh->mNumVertices; ++i )
                {
                    const auto& uv = mesh->mTextureCoords[0][i];
                    f << "vt " << uv.x << ' ' << uv.y << '\n';
                }
            if ( hasNormal )
                for ( unsigned i = 0; i < mesh->mNumVertices; ++i )
                {
                    const auto& n = mesh->mNormals[i];
                    // Rotate/scale by the 3x3 (no translation), then renormalize.
                    float       nx  = xf.a1 * n.x + xf.a2 * n.y + xf.a3 * n.z;
                    float       ny  = xf.b1 * n.x + xf.b2 * n.y + xf.b3 * n.z;
                    float       nz  = xf.c1 * n.x + xf.c2 * n.y + xf.c3 * n.z;
                    const float len = std::sqrt( nx * nx + ny * ny + nz * nz );
                    if ( len > 1e-8f )
                    {
                        nx /= len;
                        ny /= len;
                        nz /= len;
                    }
                    f << "vn " << nx << ' ' << ny << ' ' << nz << '\n';
                }

            for ( unsigned fi = 0; fi < mesh->mNumFaces; ++fi )
            {
                const aiFace& face = mesh->mFaces[fi];
                if ( face.mNumIndices != 3 )
                    continue; // triangulated on import; skip non-tris
                f << 'f';
                for ( unsigned k = 0; k < 3; ++k )
                {
                    const unsigned v = face.mIndices[k] + 1; // OBJ is 1-based
                    if ( hasUV && hasNormal )
                        f << ' ' << v << '/' << v << '/' << v;
                    else if ( hasUV )
                        f << ' ' << v << '/' << v;
                    else if ( hasNormal )
                        f << ' ' << v << "//" << v;
                    else
                        f << ' ' << v;
                }
                f << '\n';
            }

            return WriteWholeFile( outPath, f.str(), error );
        }
    } // namespace

    FbxSplitResult SplitFbxIntoMeshes( const std::filesystem::path& fbxPath, float scale )
    {
        FbxSplitResult  result;
        std::error_code ec;

        const std::filesystem::path fbxAbs = std::filesystem::absolute( fbxPath, ec );
        if ( !std::filesystem::exists( fbxAbs, ec ) )
        {
            result.Error = "FBX not found: " + fbxPath.string();
            return result;
        }

        Assimp::Importer importer;
        // Triangulate + smooth normals. The real size/orientation lives in the NODE transforms (baked per
        // mesh below), not a unit-scale flag — so NO GlobalScale (it double-shrinks here). DO NOT
        // PreTransformVertices either (it merges meshes by material and would collapse the ~10 meshes).
        const aiScene* scene =
             importer.ReadFile( fbxAbs.string(), aiProcess_Triangulate | aiProcess_GenSmoothNormals );
        if ( !scene || !scene->mMeshes || scene->mNumMeshes == 0 )
        {
            result.Error = std::string( "Assimp failed to load meshes: " ) + importer.GetErrorString();
            return result;
        }

        const std::string stem = fbxAbs.stem().string();

        // Keep the pack self-contained: write the .obj outputs INTO the collection folder (next to the FBX),
        // not Resources/Mesh — so the engine's Assets view isn't polluted with pack contents. Manifest paths
        // are relative to the project dir (parent of "Resources") so they read "Resources/Collections/.../...".
        std::filesystem::path       resourcesDir, projectDir;
        const std::filesystem::path meshDir = fbxAbs.parent_path() / "meshes";
        if ( FindResourcesRoot( fbxAbs, resourcesDir ) )
        {
            projectDir = resourcesDir.parent_path();
        }
        else
        {
            projectDir = fbxAbs.parent_path();
            std::cerr << "[FbxMeshSplitter] WARN: no 'Resources' ancestor; manifest paths may be absolute.\n";
        }
        std::filesystem::create_directories( meshDir, ec );

        // Map each mesh index -> its node's WORLD transform (first node that references it). FBX keeps geometry
        // in tiny local space and the real size/placement in the node hierarchy.
        std::vector<aiMatrix4x4> meshXf( scene->mNumMeshes ); // identity by default
        std::vector<bool>        meshHasXf( scene->mNumMeshes, false );
        std::function<void( const aiNode*, const aiMatrix4x4& )> walk =
             [&]( const aiNode* node, const aiMatrix4x4& parent )
        {
            const aiMatrix4x4 world = parent * node->mTransformation;
            for ( unsigned i = 0; i < node->mNumMeshes; ++i )
            {
                const unsigned mi = node->mMeshes[i];
                if ( mi < meshXf.size() && !meshHasXf[mi] )
                {
                    meshXf[mi]    = world;
                    meshHasXf[mi] = true;
                }
            }
            for ( unsigned i = 0; i < node->mNumChildren; ++i )
                walk( node->mChildren[i], world );
        };
        if ( scene->mRootNode )
            walk( scene->mRootNode, aiMatrix4x4() );

        // Detect PBR materials from the collection's texture files (by filename-suffix convention). With an
        // atlas (one diffuse for many cards) this yields ONE material that every mesh shares.
        const std::vector<MaterialDef> materials = ScanMaterials( fbxAbs.parent_path(), projectDir );

        std::unordered_set<std::string> usedNames;
        Desert::Editor::CollectionManifest manifest; // written through the shared writer the panel reads back

        for ( unsigned i = 0; i < scene->mNumMeshes; ++i )
        {
            const aiMesh* mesh = scene->mMeshes[i];
            if ( !mesh || mesh->mNumVertices == 0 || mesh->mNumFaces == 0 )
                continue;

            std::string rawName = mesh->mName.length > 0 ? mesh->mName.C_Str() : "mesh_" + std::to_string( i );
            std::string base    = Sanitize( rawName );
            std::string name    = base;
            for ( int n = 1; usedNames.count( name ); ++n ) // de-dup repeated mesh names
                name = base + "_" + std::to_string( n );
            usedNames.insert( name );

            const std::filesystem::path objPath = meshDir / ( name + ".obj" );
            std::string                 objError;
            if ( !WriteObj( mesh, meshXf[i], scale, objPath, name, objError ) )
            {
                // Named, and the mesh is left OUT of the manifest below — a collection that lists a mesh
                // whose .obj is short or absent is the wrong measurement this tool must not produce.
                std::cerr << "[FbxMeshSplitter] WARN: " << objError << '\n';
                continue;
            }

            // Working-dir-relative source path (forward slashes) — the manifest payload + engine cook key.
            std::filesystem::path rel      = std::filesystem::relative( objPath, projectDir, ec );
            const std::string     meshRel  = ec ? objPath.generic_string() : rel.generic_string();
            const int             matIndex = BestMaterialForCategory( stem, materials );
            Desert::Editor::CollectionManifestItem item;
            item.Name     = name;
            item.Category = stem;
            item.Mesh     = meshRel;
            if ( matIndex >= 0 )
                item.Material = matIndex;
            manifest.Items.push_back( std::move( item ) );
            ++result.MeshCount;
        }

        if ( result.MeshCount == 0 )
        {
            result.Error = "No non-empty meshes to export.";
            return result;
        }

        // Write the collection manifest next to the FBX so the engine's Collections panel picks it up.
        const std::filesystem::path manifestPath = fbxAbs.parent_path() / "collection.json";
        // The Materials array (only the slots that were actually found; cutout => AlphaCutoff + TwoSided so
        // foliage cards render right out of the box).
        auto slot = []( const std::string& path ) -> std::optional<std::string>
        {
            if ( path.empty() )
                return std::nullopt;
            return path;
        };
        if ( !materials.empty() )
        {
            std::vector<Desert::Editor::CollectionManifestMaterial> offered;
            for ( const auto& m : materials )
            {
                const bool cutout = !m.Opacity.empty();
                offered.push_back( { .Name        = m.Stem,
                                     .Albedo      = slot( m.Albedo ),
                                     .Opacity     = slot( m.Opacity ),
                                     .Normal      = slot( m.Normal ),
                                     .Roughness   = slot( m.Roughness ),
                                     .Metallic    = slot( m.Metallic ),
                                     .AO          = slot( m.AO ),
                                     .AlphaCutoff = cutout ? 0.5f : 0.0f,
                                     .TwoSided    = cutout } );
            }
            manifest.Materials = std::move( offered );
        }
        manifest.Name   = fbxAbs.parent_path().filename().string();
        manifest.Author = "FbxMeshSplitter";

        // The manifest DID call close() — and then never looked at the stream again, so a failed flush
        // still produced result.Success = true and a ManifestPath the caller would go on to read. A
        // close whose result nobody reads is the same silence as no close at all; that is why the
        // verdict lives in WriteWholeFile and not at the call sites.
        if ( std::string error;
             !WriteWholeFile( manifestPath, Desert::Editor::WriteCollectionManifest( manifest ), error ) )
        {
            result.Error = "Could not write manifest: " + error;
            return result;
        }

        result.MaterialCount = static_cast<int>( materials.size() );
        std::cout << "[FbxMeshSplitter] " << materials.size() << " material(s) detected from textures.\n";

        result.Success      = true;
        result.ManifestPath = manifestPath.generic_string();
        return result;
    }
} // namespace FbxSplit
