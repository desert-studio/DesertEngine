#include <Engine/Assets/Serialization/ImportRecord.hpp>

#include <Engine/Assets/TextAssetHeaderCheck.hpp>
#include <Engine/Geometry/LODSelection.hpp>
#include <Common/Content/CanonicalText.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Json/Json.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace Desert::Assets::Serialization
{
    SourceImportSettingsText ImportSettingsToText( const Assets::SourceImportSettings& settings )
    {
        return {
             settings.CombineMeshes,
             settings.Mesh.UniformScale,
             std::string( Assets::MeshSourceUpAxisName( settings.Mesh.UpAxis ) ),
             std::string( Assets::MeshLodPolicyName( settings.Mesh.LodPolicy ) ),
             settings.Skeleton
                  ? std::optional<std::string>( Common::Content::AssetGuidToText( *settings.Skeleton ) )
                  : std::nullopt,
             settings.SpecularMap == Assets::FbxSpecularMap::FromImage
                  ? std::nullopt
                  : std::optional<std::string>( Assets::FbxSpecularMapName( settings.SpecularMap ) ),
             settings.FileUnit == Assets::MeshFileUnit::FromFile
                  ? std::nullopt
                  : std::optional<std::string>( Assets::MeshFileUnitName( settings.FileUnit ) ),
             settings.Mesh.Displacement == Assets::MeshDisplacementMode::None
                  ? std::nullopt
                  : std::optional<std::string>( Assets::MeshDisplacementModeName( settings.Mesh.Displacement ) ),
             settings.Mesh.DisplacementBake == Assets::MeshDisplacementBakeSettings{}
                  ? std::nullopt
                  : std::optional<float>( settings.Mesh.DisplacementBake.EdgeLengthCm ),
             settings.Mesh.VirtualGeometry ? std::optional<bool>( true ) : std::nullopt,
             settings.Mesh.Canopy.LeafSlots.empty()
                  ? std::nullopt
                  : std::optional<std::vector<std::string>>( settings.Mesh.Canopy.LeafSlots ) };
    }

    Common::ResultStr<Assets::SourceImportSettings> ImportSettingsFromText( const SourceImportSettingsText& text )
    {
        using Result    = Assets::SourceImportSettings;
        const auto axis = Assets::MeshSourceUpAxisFromName( text.UpAxis );
        const auto lods = Assets::MeshLodPolicyFromName( text.LodPolicy );
        if ( !axis || !lods )
            return Common::MakeFormattedError<Result>( "import settings name up axis '{}' and LOD policy '{}'; "
                                                       "one is unknown to this build",
                                                       text.UpAxis, text.LodPolicy );
        if ( !std::isfinite( text.UniformScale ) || text.UniformScale <= 0.0f )
            return Common::MakeFormattedError<Result>(
                 "import settings state scale {}, not a finite positive number", text.UniformScale );
        Result out;
        if ( text.Skeleton )
        {
            auto skeleton = Common::Content::AssetGuidFromText( *text.Skeleton );
            if ( !skeleton || skeleton.GetValue().IsNull() )
                return Common::MakeFormattedError<Result>( "import settings name skeleton '{}', not an asset GUID",
                                                           *text.Skeleton );
            out.Skeleton = skeleton.GetValue();
        }
        if ( text.SpecularMap )
        {
            const auto specular = Assets::FbxSpecularMapFromName( *text.SpecularMap );
            if ( !specular )
                return Common::MakeFormattedError<Result>(
                     "import settings name Specular map meaning '{}'; this build knows FromImage, Specular, "
                     "OcclusionRoughnessMetallic and RoughnessMetallic",
                     *text.SpecularMap );
            out.SpecularMap = *specular;
        }
        if ( text.FileUnit )
        {
            const auto unit = Assets::MeshFileUnitFromName( *text.FileUnit );
            if ( !unit )
                return Common::MakeFormattedError<Result>(
                     "import settings name file unit '{}'; this build knows FromFile, Millimetres, "
                     "Centimetres, Metres, Inches and Feet",
                     *text.FileUnit );
            out.FileUnit = *unit;
        }
        if ( text.Displacement )
        {
            const auto mode = Assets::MeshDisplacementModeFromName( *text.Displacement );
            if ( !mode )
                return Common::MakeFormattedError<Result>(
                     "import settings name displacement mode '{}'; this build knows None, Runtime and Baked",
                     *text.Displacement );
            out.Mesh.Displacement = *mode;
        }
        if ( text.DisplacementEdgeLengthCm )
        {
            if ( !std::isfinite( *text.DisplacementEdgeLengthCm ) || *text.DisplacementEdgeLengthCm <= 0.0f )
                return Common::MakeFormattedError<Result>(
                     "import settings state displacement edge length {} cm, not a finite positive number",
                     *text.DisplacementEdgeLengthCm );
            out.Mesh.DisplacementBake.EdgeLengthCm = *text.DisplacementEdgeLengthCm;
        }
        out.Mesh.VirtualGeometry = text.VirtualGeometry.value_or( false );
        if ( text.CanopyLeafSlots )
        {
            for ( const std::string& slot : *text.CanopyLeafSlots )
                if ( slot.empty() || std::ranges::count( *text.CanopyLeafSlots, slot ) != 1 )
                    return Common::MakeFormattedError<Result>(
                         "import settings name canopy leaf slot '{}': a slot is named once and not empty", slot );
            out.Mesh.Canopy.LeafSlots = *text.CanopyLeafSlots;
        }
        out.CombineMeshes     = text.CombineMeshes;
        out.Mesh.UniformScale = text.UniformScale;
        out.Mesh.UpAxis       = *axis;
        out.Mesh.LodPolicy    = *lods;
        return Common::MakeSuccess( out );
    }

    bool IsImportRecordKind( const Common::Content::ContentKind kind )
    {
        using Common::Content::ContentKind;
        return kind == ContentKind::StaticMesh || kind == ContentKind::SkinnedMesh ||
               kind == ContentKind::Skeleton || kind == ContentKind::Animation;
    }

    namespace
    {
        bool Finite3( const std::array<float, 3>& v )
        {
            return std::isfinite( v[0] ) && std::isfinite( v[1] ) && std::isfinite( v[2] );
        }

        // What is wrong with @p light as a record states it; empty when nothing is.
        std::string ImportRecordLightDefect( const ImportRecordLight& light )
        {
            if ( light.Name.empty() )
                return "has no Name";
            const bool known = std::ranges::find( ImportRecordLightTypes, std::string_view( light.Type ) ) !=
                               ImportRecordLightTypes.end();
            if ( !known )
                return std::format( "states Type '{}'; a light is Point, Spot or Directional", light.Type );
            if ( !Finite3( light.Position ) || !Finite3( light.Direction ) || !Finite3( light.Color ) )
                return "states a Position, Direction or Color that is not finite";
            const float length =
                 std::sqrt( light.Direction[0] * light.Direction[0] + light.Direction[1] * light.Direction[1] +
                            light.Direction[2] * light.Direction[2] );
            if ( std::abs( length - 1.0f ) > 1e-3f )
                return std::format( "states a Direction of length {}, not a unit direction", length );
            if ( light.Color[0] < 0.0f || light.Color[1] < 0.0f || light.Color[2] < 0.0f )
                return "states a negative Color";
            if ( !std::isfinite( light.Intensity ) || light.Intensity < 0.0f )
                return std::format( "states Intensity {}, not a finite number >= 0", light.Intensity );
            if ( light.Radius && ( !std::isfinite( *light.Radius ) || *light.Radius <= 0.0f ) )
                return std::format( "states Radius {}, not a finite number > 0", *light.Radius );
            if ( light.Type == "Directional" && light.Radius )
                return "is directional and states a Radius";
            const bool spot = light.Type == "Spot";
            if ( spot != ( light.InnerConeAngle.has_value() && light.OuterConeAngle.has_value() ) ||
                 ( !spot && ( light.InnerConeAngle || light.OuterConeAngle ) ) )
                return "states cone angles: a spot light states both, every other light neither";
            if ( spot && !( *light.InnerConeAngle >= 0.0f && *light.InnerConeAngle <= *light.OuterConeAngle &&
                            *light.OuterConeAngle <= 180.0f ) )
                return std::format( "states cones {} and {} degrees; 0 <= inner <= outer <= 180",
                                    *light.InnerConeAngle, *light.OuterConeAngle );
            return {};
        }
    } // namespace

    Common::ResultStr<ImportRecordData> ParseImportRecord( const std::string& text )
    {
        if ( text.empty() )
            return Common::MakeFormattedError<ImportRecordData>( "the file is empty" );
        if ( auto headed = Assets::RefuseTextWithoutHeader( text, kImportRecordVersion, std::nullopt ); !headed )
            return Common::MakeFormattedError<ImportRecordData>( "import record {}", headed.GetError() );
        const auto parsed = Common::Json::Read<ImportRecordData>( text );
        if ( !parsed )
            return Common::MakeFormattedError<ImportRecordData>( "{}", parsed.GetError() );
        ImportRecordData data = parsed.GetValue();
        // The kind the header states is checked as every text asset's is, against the kinds a record may state.
        const auto stated = data.Header ? Common::Content::ContentKindNamed( data.Header->Kind ) : std::nullopt;
        if ( data.Header && ( !stated || !IsImportRecordKind( *stated ) ) )
            return Common::MakeFormattedError<ImportRecordData>(
                 "import record states kind '{}'; a record states StaticMesh, SkinnedMesh, Skeleton or Animation",
                 data.Header->Kind );
        if ( auto header = Assets::CheckStatedHeader(
                  data.Header, stated.value_or( Common::Content::ContentKind::StaticMesh ),
                  Assets::kImportRecordSchemaTag, kImportRecordVersion, ImportRecordTextSubsystems() );
             !header )
            return Common::MakeFormattedError<ImportRecordData>( "import record {}", header.GetError() );
        if ( data.Source.empty() )
            return Common::MakeFormattedError<ImportRecordData>( "import record names no Source" );
        // The box is the imported MESH's (DIMP 2): required of a record whose source imports as a mesh, not of
        // one that imports a skeleton or clips only (RecordImport writes those with no box - there is no mesh).
        const Common::Content::ContentKind kind = stated.value_or( Common::Content::ContentKind::StaticMesh );
        const bool                         importsMesh =
             kind == Common::Content::ContentKind::StaticMesh || kind == Common::Content::ContentKind::SkinnedMesh;
        if ( importsMesh && !data.Bounds )
            return Common::MakeFormattedError<ImportRecordData>( "import record of a {} states no Bounds",
                                                                 Common::Content::KindName( kind ) );
        if ( data.Nodes )
            for ( const ImportRecordNode& node : *data.Nodes )
            {
                if ( node.Name.empty() )
                    return Common::MakeFormattedError<ImportRecordData>(
                         "import record names a node with no Name" );
                if ( !std::isfinite( node.Placement[0] ) || !std::isfinite( node.Placement[1] ) ||
                     !std::isfinite( node.Placement[2] ) )
                    return Common::MakeFormattedError<ImportRecordData>(
                         "import record: the Placement of node '{}' is not finite", node.Name );
            }
        if ( data.Lights )
        {
            if ( data.Lights->empty() )
                return Common::MakeFormattedError<ImportRecordData>(
                     "import record states an empty Lights: a file with no light is written as no key" );
            for ( const ImportRecordLight& light : *data.Lights )
                if ( const std::string defect = ImportRecordLightDefect( light ); !defect.empty() )
                    return Common::MakeFormattedError<ImportRecordData>( "import record: light '{}' {}",
                                                                         light.Name, defect );
        }
        if ( data.Thumbnail )
        {
            if ( data.Thumbnail->empty() )
                return Common::MakeFormattedError<ImportRecordData>(
                     "import record states an empty Thumbnail: no orbit for any mesh is written as no key" );
            for ( const auto& [mesh, stated] : *data.Thumbnail )
            {
                const ThumbnailOrbit orbit = Resolve( stated );
                if ( !IsValidThumbnailOrbit( orbit ) )
                    return Common::MakeFormattedError<ImportRecordData>(
                         "import record: the Thumbnail orbit of '{}' is not finite or zooms to -1 or in", mesh );
                if ( orbit == ThumbnailOrbit{} )
                    return Common::MakeFormattedError<ImportRecordData>(
                         "import record states the default Thumbnail orbit for '{}': the default is written as no "
                         "entry",
                         mesh );
            }
        }
        if ( data.LodScreenSize )
        {
            if ( data.LodScreenSize->empty() )
                return Common::MakeFormattedError<ImportRecordData>(
                     "import record states an empty LodScreenSize: computed sizes for every mesh are written as "
                     "no "
                     "key" );
            for ( const auto& [mesh, sizes] : *data.LodScreenSize )
                if ( const std::string defect = Geometry::LODScreenSizesDefect( sizes ); !defect.empty() )
                    return Common::MakeFormattedError<ImportRecordData>(
                         "import record: the LodScreenSize of '{}' {}", mesh, defect );
        }
        return Common::MakeSuccess( std::move( data ) );
    }

    Common::ResultStr<std::string> WriteImportRecord( const ImportRecordData&            data,
                                                      const Common::Content::ContentKind kind )
    {
        if ( !IsImportRecordKind( kind ) )
            return Common::MakeFormattedError<std::string>( "an import record cannot state kind '{}'",
                                                            Common::Content::KindName( kind ) );
        ImportRecordData out = data;
        out.Header           = Assets::StampTextHeader( data.Header, kind, ImportRecordTextSubsystems() );
        return Common::MakeSuccess( Common::Json::Write( out ) );
    }

    namespace
    {
        // THE PARSED RECORD IS HELD FOR THE SESSION (UE: an asset's import data is read once and served from
        // memory by the asset registry). A split source's record names every node it wrote - Bistro's states 1296
        // and is 236 KB - and every node mesh's thumbnail, kind, identity and orbit is asked of it: parsed per
        // ask, the content browser's warm-up spent ~0.25 s per node mesh. The record is parsed once and parsed
        // again only when its file changes: what the file is is stated by its write time and size, read on every
        // ask, and a write through this file forgets the held parse outright (a rewrite in the same clock tick
        // with the same size is still seen).
        struct RecordStamp
        {
            std::filesystem::file_time_type Written;
            std::uintmax_t                  Size = 0;

            friend bool operator==( const RecordStamp&, const RecordStamp& ) = default;
        };

        struct HeldRecord
        {
            RecordStamp Stamp;
            /// The parsed record, or nullopt with Error (naming the record) when the file is not a record.
            std::optional<ImportRecordData> Data;
            std::string                     Error;
        };

        std::mutex                                                         g_HeldMutex;
        std::unordered_map<std::string, std::shared_ptr<const HeldRecord>> g_Held;
        std::atomic<uint64_t>                                              g_Parses{ 0 };

        /// The record at @p record as this session holds it, parsed when its file is new or changed; nullptr when
        /// there is no record file.
        std::shared_ptr<const HeldRecord> HeldRecordFor( const std::filesystem::path& record )
        {
            const std::filesystem::path full = Common::Constants::Path::FullPath( record );
            const std::string           key  = full.generic_string();
            std::error_code             ec;
            if ( !std::filesystem::is_regular_file( full, ec ) )
            {
                const std::scoped_lock lock( g_HeldMutex );
                g_Held.erase( key );
                return nullptr;
            }
            // Sampled before the stat: a rewrite inside one file-system tick keeps (write time, size), so a parse
            // taken while the stamp was racy is not held - the next ask parses again
            // (Common::Utils::IsRacyWriteTime).
            const auto        readBegan = std::filesystem::file_time_type::clock::now();
            std::error_code   writtenError;
            std::error_code   sizeError;
            const RecordStamp stamp{ std::filesystem::last_write_time( full, writtenError ),
                                     std::filesystem::file_size( full, sizeError ) };
            const bool        stamped = !writtenError && !sizeError;
            if ( stamped )
            {
                const std::scoped_lock lock( g_HeldMutex );
                if ( const auto it = g_Held.find( key ); it != g_Held.end() && it->second->Stamp == stamp )
                    return it->second;
            }
            auto held       = std::make_shared<HeldRecord>();
            held->Stamp     = stamp;
            const auto text = Common::Utils::FileSystem::ReadFileContent( record );
            if ( !text )
            {
                // Unreadable is not a statement of the file: the next ask reads it again.
                held->Error = std::format( "'{}': {}", record.string(), text.GetError() );
                return held;
            }
            g_Parses.fetch_add( 1, std::memory_order_relaxed );
            if ( auto parsed = ParseImportRecord( text.GetValue() ) )
                held->Data = parsed.ExtractValue();
            else
                held->Error = std::format( "'{}': {}", record.string(), parsed.GetError() );
            if ( stamped && !Common::Utils::IsRacyWriteTime( stamp.Written, readBegan ) )
            {
                const std::scoped_lock lock( g_HeldMutex );
                g_Held[key] = held;
            }
            return held;
        }

        /// Writes @p text as the record at @p record and forgets the held parse of the file it replaces.
        Common::BoolResultStr WriteRecordFile( const std::filesystem::path& record, const std::string& text )
        {
            auto written = Common::Content::WriteCanonicalJsonFileAtomic( record, text );
            {
                const std::scoped_lock lock( g_HeldMutex );
                g_Held.erase( Common::Constants::Path::FullPath( record ).generic_string() );
            }
            if ( !written )
                return Common::MakeFormattedError<bool>( "'{}' could not be written: {}", record.string(),
                                                         written.GetError() );
            return BOOLSUCCESS;
        }
    } // namespace

    uint64_t ImportRecordParseCount()
    {
        return g_Parses.load( std::memory_order_relaxed );
    }

    Common::ResultStr<Common::Content::AssetGuid> ReadImportRecordGuid( const std::filesystem::path& source )
    {
        using Common::Content::AssetGuid;
        const std::filesystem::path record = Common::Content::ImportRecordPathFor( source );
        const auto                  held   = HeldRecordFor( record );
        if ( !held )
            return Common::MakeFormattedError<AssetGuid>(
                 "'{}' has no import record '{}': its mesh has no identity until it is imported (the import "
                 "writes the record)",
                 source.string(), record.string() );
        const std::optional<ImportRecordData>& stored = held->Data;
        if ( !stored )
            return Common::MakeFormattedError<AssetGuid>( "{}", held->Error );
        const ImportRecordData& data = *stored;
        if ( data.Source != source.filename().string() )
            return Common::MakeFormattedError<AssetGuid>( "'{}' is the record of '{}', not of '{}'",
                                                          record.string(), data.Source,
                                                          source.filename().string() );
        if ( !data.Header )
            return Common::MakeFormattedError<AssetGuid>( "'{}' states no header", record.string() );
        const auto guid = Common::Content::AssetGuidFromText( data.Header->Guid );
        if ( !guid || guid.GetValue().IsNull() )
            return Common::MakeFormattedError<AssetGuid>( "'{}' states no usable GUID", record.string() );
        return guid;
    }

    Common::ResultStr<Assets::SourceImportSettings> ReadImportRecordSettings( const std::filesystem::path& source )
    {
        using Result    = Assets::SourceImportSettings;
        const auto held = HeldRecordFor( Common::Content::ImportRecordPathFor( source ) );
        if ( !held ) // the first import: UE's defaults
            return Common::MakeSuccess( Result{} );
        const std::optional<ImportRecordData>& stored = held->Data;
        if ( !stored )
            return Common::MakeFormattedError<Result>( "{}", held->Error );
        const auto& text = stored->Settings;
        if ( !text ) // a record from before THM1l: UE's defaults
            return Common::MakeSuccess( Result{} );
        auto settings = ImportSettingsFromText( *text );
        if ( !settings )
            return Common::MakeFormattedError<Result>(
                 "'{}': {}", Common::Content::ImportRecordPathFor( source ).string(), settings.GetError() );
        return settings;
    }

    Common::ResultStr<Common::Content::ContentKind> ReadImportRecordKind( const std::filesystem::path& source )
    {
        using Common::Content::ContentKind;
        const std::string record = Common::Content::ImportRecordPathFor( source ).string();
        const auto        held   = HeldRecordFor( Common::Content::ImportRecordPathFor( source ) );
        if ( !held )
            return Common::MakeFormattedError<ContentKind>( "'{}' does not exist", record );
        const std::optional<ImportRecordData>& stored = held->Data;
        if ( !stored )
            return Common::MakeFormattedError<ContentKind>( "{}", held->Error );
        const auto& header = stored->Header;
        if ( !header )
            return Common::MakeFormattedError<ContentKind>( "'{}' states no header", record );
        const std::string& name = header->Kind;
        const auto         kind = Common::Content::ContentKindNamed( name );
        if ( !kind || !IsImportRecordKind( *kind ) )
            return Common::MakeFormattedError<ContentKind>( "'{}' states Kind '{}', which no import writes",
                                                            record, name );
        return Common::MakeSuccess( *kind );
    }

    namespace
    {
        std::optional<ImportRecordData::Box> RecordBox( const std::optional<Common::Math::AABB>& bounds )
        {
            if ( !bounds )
                return std::nullopt;
            return ImportRecordData::Box{ { bounds->Min.x, bounds->Min.y, bounds->Min.z },
                                          { bounds->Max.x, bounds->Max.y, bounds->Max.z } };
        }

        // What of an import @p data does not state yet (empty: it states all of it) - the one comparison
        // EnsureImportRecord rewrites by and ReadCurrentImportRecord refuses by.
        std::string ImportRecordDifference( const ImportRecordData& data, const Common::Content::ContentKind kind,
                                            const std::optional<ImportRecordData::Box>& box,
                                            const Assets::SourceImportSettings&         settings )
        {
            // No Settings key IS UE's defaults (ReadImportRecordSettings), so a default import matches it.
            bool sameSettings = !data.Settings && settings == Assets::SourceImportSettings{};
            if ( data.Settings )
                if ( const auto stated = ImportSettingsFromText( *data.Settings ) )
                    sameSettings = stated.GetValue() == settings;
            if ( !sameSettings )
                return "other import options";
            // No box (a file with no mesh: a skeleton and its clips) leaves the stated box as it is.
            if ( box && !( data.Bounds && data.Bounds->Min == box->Min && data.Bounds->Max == box->Max ) )
                return "another box";
            if ( !( data.Header && data.Header->Kind == Common::Content::KindName( kind ) ) )
                return std::format( "another kind than {}", Common::Content::KindName( kind ) );
            return {};
        }
    } // namespace

    Common::ResultStr<Common::Content::AssetGuid>
    ReadCurrentImportRecord( const std::filesystem::path& source, const Common::Content::ContentKind kind,
                             const std::optional<Common::Math::AABB>& bounds,
                             const Assets::SourceImportSettings&      settings )
    {
        using Common::Content::AssetGuid;
        const std::filesystem::path record = Common::Content::ImportRecordPathFor( source );
        if ( auto guid = ReadImportRecordGuid( source ); !guid )
            return guid;
        const auto held = HeldRecordFor( record );
        if ( !held )
            return Common::MakeFormattedError<AssetGuid>( "'{}' was removed while it was read", record.string() );
        if ( !held->Data )
            return Common::MakeFormattedError<AssetGuid>( "{}", held->Error );
        if ( const std::string differs =
                  ImportRecordDifference( *held->Data, kind, RecordBox( bounds ), settings );
             !differs.empty() )
            return Common::MakeFormattedError<AssetGuid>( "'{}' states {} than this import of '{}'",
                                                          record.generic_string(), differs,
                                                          source.generic_string() );
        return ReadImportRecordGuid( source );
    }

    Common::ResultStr<Common::Content::AssetGuid>
    EnsureImportRecord( const std::filesystem::path& source, const Common::Content::ContentKind kind,
                        const std::optional<Common::Math::AABB>& bounds,
                        const Assets::SourceImportSettings&      settings )
    {
        using Common::Content::AssetGuid;
        const std::filesystem::path          record = Common::Content::ImportRecordPathFor( source );
        const std::optional<ImportRecordData::Box> box    = RecordBox( bounds );
        ImportRecordData            data;
        std::error_code             ec;
        if ( std::filesystem::is_regular_file( Common::Constants::Path::FullPath( record ), ec ) )
        {
            // The identity is read through the one reader, so a record of another generation or another
            // source is refused here exactly as everywhere else.
            if ( auto guid = ReadImportRecordGuid( source ); !guid )
                return guid;
            const auto held = HeldRecordFor( record );
            if ( !held )
                return Common::MakeFormattedError<AssetGuid>( "'{}' was removed while it was read",
                                                              record.string() );
            const std::optional<ImportRecordData>& stored = held->Data;
            if ( !stored )
                return Common::MakeFormattedError<AssetGuid>( "{}", held->Error );
            data = *stored;
            if ( ImportRecordDifference( data, kind, box, settings ).empty() )
                return ReadImportRecordGuid( source );
        }
        else
            data.Source = source.filename().string(); // no header: the stamp mints the GUID
        if ( box )
            data.Bounds = box;
        // ONE SPELLING OF THE DEFAULTS: no key (as a default thumbnail orbit is no key). Stating them made every
        // committed record that predates the Settings key a rewrite at its first import - a DDC miss on a fresh
        // checkout rewrote base.fbx.deimport at the first editor start with nothing in it changed.
        data.Settings   = settings == Assets::SourceImportSettings{}
                               ? std::nullopt
                               : std::optional<SourceImportSettingsText>( ImportSettingsToText( settings ) );
        const auto text = WriteImportRecord( data, kind );
        if ( !text )
            return Common::MakeFormattedError<AssetGuid>( "'{}': {}", record.string(), text.GetError() );
        if ( auto written = WriteRecordFile( record, text.GetValue() ); !written )
            return Common::MakeError<AssetGuid>( written.GetError() );
        return ReadImportRecordGuid( source );
    }

    Common::ResultStr<std::optional<ImportRecordData>> ReadImportRecord( const std::filesystem::path& source )
    {
        using Result                       = std::optional<ImportRecordData>;
        const auto held                    = HeldRecordFor( Common::Content::ImportRecordPathFor( source ) );
        if ( !held )
            return Common::MakeSuccess( Result{} );
        if ( !held->Data )
            return Common::MakeFormattedError<Result>( "{}", held->Error );
        return Common::MakeSuccess( Result{ held->Data } );
    }

    Common::ResultStr<ThumbnailOrbit> ReadImportRecordThumbnail( const std::filesystem::path& source,
                                                                 const std::string&           meshFile )
    {
        const auto held = HeldRecordFor( Common::Content::ImportRecordPathFor( source ) );
        if ( !held )
            return Common::MakeFormattedError<ThumbnailOrbit>(
                 "'{}' has no import record, so the orbit of '{}' has no home", source.string(), meshFile );
        const std::optional<ImportRecordData>& stored = held->Data;
        if ( !stored )
            return Common::MakeFormattedError<ThumbnailOrbit>( "{}", held->Error );
        const auto& thumbnail = stored->Thumbnail;
        if ( !thumbnail )
            return Common::MakeSuccess( ThumbnailOrbit{} );
        const auto it = thumbnail->find( meshFile );
        return Common::MakeSuccess( it == thumbnail->end() ? ThumbnailOrbit{} : Resolve( it->second ) );
    }

    Common::BoolResultStr SetImportRecordLights( const std::filesystem::path&          source,
                                                 const std::vector<ImportRecordLight>& lights )
    {
        const std::filesystem::path record = Common::Content::ImportRecordPathFor( source );
        for ( const ImportRecordLight& light : lights )
            if ( const std::string defect = ImportRecordLightDefect( light ); !defect.empty() )
                return Common::MakeFormattedError<bool>( "'{}': light '{}' {}", record.string(), light.Name,
                                                         defect );
        auto data = ReadImportRecord( source );
        if ( !data )
            return Common::MakeError<bool>( data.GetError() );
        const auto& stored = data.GetValue();
        if ( !stored.has_value() )
            return Common::MakeFormattedError<bool>( "'{}' does not exist, so its lights cannot be recorded",
                                                     record.string() );
        ImportRecordData                                    out = *stored;
        const std::optional<std::vector<ImportRecordLight>> stated =
             lights.empty() ? std::nullopt : std::optional<std::vector<ImportRecordLight>>( lights );
        if ( out.Lights == stated )
            return BOOLSUCCESS;
        out.Lights = stated;
        if ( !out.Header.has_value() )
            return Common::MakeFormattedError<bool>( "'{}' states no header", record.string() );
        const auto kind = Common::Content::ContentKindNamed( out.Header->Kind );
        const auto text = WriteImportRecord( out, kind.value_or( Common::Content::ContentKind::StaticMesh ) );
        if ( !text )
            return Common::MakeFormattedError<bool>( "'{}': {}", record.string(), text.GetError() );
        return WriteRecordFile( record, text.GetValue() );
    }

    Common::BoolResultStr SetImportRecordNodes( const std::filesystem::path&                        source,
                                                const std::optional<std::vector<ImportRecordNode>>& nodes )
    {
        const std::filesystem::path record = Common::Content::ImportRecordPathFor( source );
        auto                        data   = ReadImportRecord( source );
        if ( !data )
            return Common::MakeError<bool>( data.GetError() );
        const auto& stored = data.GetValue();
        if ( !stored.has_value() )
            return Common::MakeFormattedError<bool>( "'{}' does not exist, so its node meshes cannot be recorded",
                                                     record.string() );
        ImportRecordData out = *stored;
        if ( out.Nodes == nodes )
            return BOOLSUCCESS;
        out.Nodes = nodes;
        // The kind the record states stays (ParseImportRecord checked it is a record kind).
        if ( !out.Header.has_value() )
            return Common::MakeFormattedError<bool>( "'{}' states no header", record.string() );
        const auto kind = Common::Content::ContentKindNamed( out.Header->Kind );
        const auto text = WriteImportRecord( out, kind.value_or( Common::Content::ContentKind::StaticMesh ) );
        if ( !text )
            return Common::MakeFormattedError<bool>( "'{}': {}", record.string(), text.GetError() );
        return WriteRecordFile( record, text.GetValue() );
    }
    Common::BoolResultStr SetImportRecordSourceHash( const std::filesystem::path& source, const uint64_t hash )
    {
        const std::filesystem::path record = Common::Content::ImportRecordPathFor( source );
        auto                        data   = ReadImportRecord( source );
        if ( !data )
            return Common::MakeError<bool>( data.GetError() );
        const auto& stored = data.GetValue();
        if ( !stored.has_value() )
            return Common::MakeFormattedError<bool>( "'{}' does not exist, so its import's source hash cannot be "
                                                     "recorded",
                                                     record.string() );
        ImportRecordData out = *stored;
        if ( out.SourceHash == hash )
            return BOOLSUCCESS;
        out.SourceHash = hash;
        if ( !out.Header.has_value() )
            return Common::MakeFormattedError<bool>( "'{}' states no header", record.string() );
        const auto kind = Common::Content::ContentKindNamed( out.Header->Kind );
        const auto text = WriteImportRecord( out, kind.value_or( Common::Content::ContentKind::StaticMesh ) );
        if ( !text )
            return Common::MakeFormattedError<bool>( "'{}': {}", record.string(), text.GetError() );
        return WriteRecordFile( record, text.GetValue() );
    }

    Common::BoolResultStr SetImportRecordSkeleton( const std::filesystem::path&     source,
                                                   const Common::Content::AssetGuid skeleton )
    {
        const std::filesystem::path record = Common::Content::ImportRecordPathFor( source );
        if ( skeleton.IsNull() )
            return Common::MakeFormattedError<bool>(
                 "'{}': an import chooses a skeleton by GUID; the GUID is null", record.string() );
        auto data = ReadImportRecord( source );
        if ( !data )
            return Common::MakeError<bool>( data.GetError() );
        const auto& stored = data.GetValue();
        if ( !stored.has_value() )
            return Common::MakeFormattedError<bool>( "'{}' does not exist, so its import's skeleton cannot be "
                                                     "recorded",
                                                     record.string() );
        ImportRecordData             out = *stored;
        Assets::SourceImportSettings chosen; // UE's defaults when the record states no settings
        if ( out.Settings )
        {
            const auto settings = ImportSettingsFromText( *out.Settings );
            if ( !settings )
                return Common::MakeFormattedError<bool>( "'{}': {}", record.string(), settings.GetError() );
            chosen = settings.GetValue();
        }
        if ( out.Settings && chosen.Skeleton == skeleton )
            return BOOLSUCCESS;
        chosen.Skeleton = skeleton;
        out.Settings    = ImportSettingsToText( chosen );
        if ( !out.Header.has_value() )
            return Common::MakeFormattedError<bool>( "'{}' states no header", record.string() );
        const auto kind = Common::Content::ContentKindNamed( out.Header->Kind );
        const auto text = WriteImportRecord( out, kind.value_or( Common::Content::ContentKind::StaticMesh ) );
        if ( !text )
            return Common::MakeFormattedError<bool>( "'{}': {}", record.string(), text.GetError() );
        return WriteRecordFile( record, text.GetValue() );
    }

    Common::BoolResultStr SetImportRecordThumbnail( const std::filesystem::path& source,
                                                    const std::string& meshFile, const ThumbnailOrbit& orbit )
    {
        if ( !IsValidThumbnailOrbit( orbit ) )
            return Common::MakeFormattedError<bool>(
                 "the Thumbnail orbit for '{}' is not finite or zooms to -1 or in", meshFile );
        const std::filesystem::path record = Common::Content::ImportRecordPathFor( source );
        auto                        data   = ReadImportRecord( source );
        if ( !data )
            return Common::MakeError<bool>( data.GetError() );
        const auto& stored = data.GetValue();
        if ( !stored.has_value() )
            return Common::MakeFormattedError<bool>( "'{}' does not exist, so the orbit of '{}' has no home",
                                                     record.string(), meshFile );
        ImportRecordData                            out = *stored;
        std::map<std::string, ThumbnailOrbitRecord> entries =
             out.Thumbnail.value_or( std::map<std::string, ThumbnailOrbitRecord>{} );
        if ( orbit == ThumbnailOrbit{} )
            entries.erase( meshFile );
        else
            entries[meshFile] = ToRecord( orbit );
        std::optional<std::map<std::string, ThumbnailOrbitRecord>> next;
        if ( !entries.empty() )
            next = std::move( entries );
        if ( out.Thumbnail == next )
            return BOOLSUCCESS;
        out.Thumbnail = std::move( next );
        // The record keeps the kind it states (an orbit edit is no re-import).
        const auto kind = ReadImportRecordKind( source );
        if ( !kind )
            return Common::MakeError<bool>( kind.GetError() );
        const auto text = WriteImportRecord( out, kind.GetValue() );
        if ( !text )
            return Common::MakeError<bool>( text.GetError() );
        return WriteRecordFile( record, text.GetValue() );
    }

    Common::ResultStr<std::optional<std::vector<float>>>
    ReadImportRecordLodScreenSizes( const std::filesystem::path& source, const std::string& meshFile )
    {
        using Result    = std::optional<std::vector<float>>;
        const auto held = HeldRecordFor( Common::Content::ImportRecordPathFor( source ) );
        if ( !held )
            return Common::MakeFormattedError<Result>(
                 "'{}' has no import record, so the LOD screen sizes of '{}' have no home", source.string(),
                 meshFile );
        if ( !held->Data )
            return Common::MakeFormattedError<Result>( "{}", held->Error );
        const auto& stated = held->Data->LodScreenSize;
        if ( !stated )
            return Common::MakeSuccess( Result{} );
        const auto it = stated->find( meshFile );
        return Common::MakeSuccess( it == stated->end() ? Result{} : Result{ it->second } );
    }

    Common::BoolResultStr SetImportRecordLodScreenSizes( const std::filesystem::path&             source,
                                                         const std::string&                       meshFile,
                                                         const std::optional<std::vector<float>>& sizes )
    {
        if ( sizes )
            if ( const std::string defect = Geometry::LODScreenSizesDefect( *sizes ); !defect.empty() )
                return Common::MakeFormattedError<bool>( "the LOD screen sizes for '{}': {}", meshFile, defect );
        const std::filesystem::path record = Common::Content::ImportRecordPathFor( source );
        auto                        data   = ReadImportRecord( source );
        if ( !data )
            return Common::MakeError<bool>( data.GetError() );
        const auto& stored = data.GetValue();
        if ( !stored.has_value() )
            return Common::MakeFormattedError<bool>(
                 "'{}' does not exist, so the LOD screen sizes of '{}' have no home", record.string(), meshFile );
        ImportRecordData                          out = *stored;
        std::map<std::string, std::vector<float>> entries =
             out.LodScreenSize.value_or( std::map<std::string, std::vector<float>>{} );
        if ( sizes )
            entries[meshFile] = *sizes;
        else
            entries.erase( meshFile );
        std::optional<std::map<std::string, std::vector<float>>> next;
        if ( !entries.empty() )
            next = std::move( entries );
        if ( out.LodScreenSize == next )
            return BOOLSUCCESS;
        out.LodScreenSize = std::move( next );
        // The record keeps the kind it states (a screen-size edit is no re-import).
        const auto kind = ReadImportRecordKind( source );
        if ( !kind )
            return Common::MakeError<bool>( kind.GetError() );
        const auto text = WriteImportRecord( out, kind.GetValue() );
        if ( !text )
            return Common::MakeError<bool>( text.GetError() );
        return WriteRecordFile( record, text.GetValue() );
    }

    std::optional<MeshLodScreenSizesHome> LodScreenSizesHomeOf( const std::filesystem::path& meshAsset )
    {
        const auto source = Common::Content::MeshSourceBeside( meshAsset );
        if ( !source )
            return std::nullopt;
        std::error_code ec;
        if ( !std::filesystem::is_regular_file( Common::Content::ImportRecordPathFor( *source ), ec ) )
            return std::nullopt;
        return MeshLodScreenSizesHome{ *source, source->filename().string() };
    }

    Common::ResultStr<std::optional<std::vector<float>>>
    ReadMeshLodScreenSizes( const std::filesystem::path& meshAsset )
    {
        const auto home = LodScreenSizesHomeOf( meshAsset );
        if ( !home )
            return Common::MakeSuccess( std::optional<std::vector<float>>{} );
        return ReadImportRecordLodScreenSizes( home->Source, home->MeshFile );
    }
} // namespace Desert::Assets::Serialization
