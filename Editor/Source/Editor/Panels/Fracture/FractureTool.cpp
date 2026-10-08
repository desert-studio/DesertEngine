#include "FractureTool.hpp"

#include <Editor/Core/CommandHistory.hpp>

#include <Engine/Assets/FractureAsset.hpp>

#include <Common/Core/Constants.hpp>

#include <format>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace Desert::Editor
{
    namespace
    {
        /// One Generate or interior-material edit: the file's bytes before and after (FractureAsset::FileStep).
        class FractureFileCommand final : public ICommand
        {
        public:
            FractureFileCommand( Assets::FractureAsset::FileStep step, std::string label )
                 : m_Step( std::move( step ) ), m_Label( std::move( label ) )
            {
            }

            bool Undo() override
            {
                return static_cast<bool>( Assets::FractureAsset::RestoreBytes( m_Step.File, m_Step.Before ) );
            }

            bool Redo() override
            {
                return static_cast<bool>( Assets::FractureAsset::RestoreBytes( m_Step.File, m_Step.After ) );
            }

            std::string GetLabel() const override
            {
                return m_Label;
            }

        private:
            Assets::FractureAsset::FileStep m_Step;
            std::string                     m_Label;
        };

        /// The file's bytes; empty when there is no file.
        std::vector<unsigned char> ReadBytes( const std::filesystem::path& file )
        {
            std::ifstream in( file, std::ios::binary );
            if ( !in )
                return {};
            return { std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() };
        }
    } // namespace

    FractureTool::FractureTool()
    {
        Settings.Levels.emplace_back();
    }

    FractureTool& FractureTool::Get()
    {
        static FractureTool s_Tool;
        return s_Tool;
    }

    std::filesystem::path FractureTool::File() const
    {
        // operator/ keeps an absolute right-hand side as is.
        return std::filesystem::path( Common::Constants::Path::ASSETS_PATH ) / Path;
    }

    FractureTool::FileStamp FractureTool::StampOf( const std::filesystem::path& file )
    {
        FileStamp       stamp;
        std::error_code ec;
        stamp.Exists = std::filesystem::is_regular_file( file, ec );
        if ( !stamp.Exists )
            return stamp;
        stamp.Size    = std::filesystem::file_size( file, ec );
        stamp.Written = std::filesystem::last_write_time( file, ec );
        return stamp;
    }

    void FractureTool::Load()
    {
        const auto file = File();
        m_ReadRevision  = CommandHistory::Get().Revision();
        m_ReadStamp     = StampOf( file );
        Adopt( file, ReadBytes( file ) );
    }

    void FractureTool::ReadKeepingSettings()
    {
        // The file as it is now (it may have changed since the panel read it), without dropping the settings
        // the user has edited since: they are what the next Generate bakes.
        Destruction::FractureSettings edited = Settings;
        Load();
        Settings = std::move( edited );
    }

    bool FractureTool::Refresh()
    {
        if ( m_ReadFile.empty() )
            return false;
        // Cheap triggers first (the undo stack moved, or the file's size / write time did); the bytes decide.
        const uint64_t  revision = CommandHistory::Get().Revision();
        const FileStamp stamp    = StampOf( m_ReadFile );
        if ( revision == m_ReadRevision && stamp == m_ReadStamp )
            return false;
        m_ReadRevision                   = revision;
        m_ReadStamp                      = stamp;
        std::vector<unsigned char> bytes = ReadBytes( m_ReadFile );
        if ( bytes == m_ReadBytes )
            return false;
        Adopt( m_ReadFile, bytes );
        return true;
    }

    void FractureTool::Adopt( const std::filesystem::path& file, const std::vector<unsigned char>& bytes )
    {
        m_ReadFile  = file;
        m_ReadBytes = bytes;
        if ( bytes.empty() )
        {
            m_Fracture = {};
            m_Loaded   = false;
            m_Status   = std::format( "'{}' does not exist yet: Generate creates it.", file.string() );
            return;
        }
        auto decoded = Destruction::DecodeFracture( bytes );
        if ( !decoded )
        {
            m_Fracture = {};
            m_Loaded   = false;
            m_Status   = std::format( "'{}' refused: {}", file.string(), decoded.GetError() );
            return;
        }
        m_Fracture = decoded.GetValue();
        Settings   = m_Fracture.Settings;
        m_Loaded   = true;
        m_Status.clear();
    }

    Common::BoolResultStr FractureTool::Generate( const Geometry::DynamicMesh3&     source,
                                                  const Common::Content::AssetGuid& sourceMesh )
    {
        if ( sourceMesh.IsNull() )
            return Refuse( "Generate refused: the mesh to fracture is not a static mesh asset with a GUID." );
        ReadKeepingSettings();
        auto next = Destruction::GenerateFracture( source, sourceMesh, m_Fracture, Settings );
        if ( !next )
            return Refuse( next.GetError() );
        return Commit( next.GetValue(), "Fracture (Generate)" );
    }

    Common::BoolResultStr FractureTool::SetInteriorMaterial( const Common::Content::AssetGuid& material )
    {
        ReadKeepingSettings();
        if ( !m_Loaded )
            return Refuse( "Set the interior material after Generate: there is no fracture to set it on." );
        Destruction::FractureData next = m_Fracture;
        next.InteriorMaterial          = material;
        return Commit( next, "Set fracture interior material" );
    }

    Common::BoolResultStr FractureTool::Commit( const Destruction::FractureData& next, const char* label )
    {
        // The edited settings belong to this step; Load below takes them back from the file it wrote.
        auto step = Assets::FractureAsset::WriteStep( File(), next );
        if ( !step )
            return Refuse( step.GetError() );
        CommandHistory::Get().PushCommand( std::make_unique<FractureFileCommand>( step.GetValue(), label ) );
        Load();
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr FractureTool::Refuse( std::string why )
    {
        m_Status = why;
        return Common::MakeError<bool>( std::move( why ) );
    }
} // namespace Desert::Editor
