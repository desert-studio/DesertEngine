#include "RetargetDocumentModel.hpp"

#include <Editor/Core/CommandHistory.hpp>

#include <Engine/Animation/Retarget/Retargeter.hpp>
#include <Engine/Animation/Skeleton.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <optional>
#include <utility>

namespace Desert::Editor
{
    /// One edit of the document: the whole data before and after (see the class comment for why whole).
    class RetargetEditRecord final : public ICommand
    {
    public:
        RetargetEditRecord( RetargetDocumentModel& model, RetargetDocumentModel::Data before,
                            RetargetDocumentModel::Data after, std::string label )
             : m_Model( model ), m_Before( std::move( before ) ), m_After( std::move( after ) ),
               m_Label( std::move( label ) )
        {
        }

        bool Undo() override
        {
            m_Model.Restore( m_Before );
            return true;
        }

        bool Redo() override
        {
            m_Model.Restore( m_After );
            return true;
        }

        [[nodiscard]] const void* EditedObject() const override
        {
            return &m_Model;
        }

        [[nodiscard]] std::string GetLabel() const override
        {
            return m_Label;
        }

    private:
        RetargetDocumentModel&      m_Model;
        RetargetDocumentModel::Data m_Before;
        RetargetDocumentModel::Data m_After;
        std::string                 m_Label;
    };

    namespace
    {
        // A bone name with the rig namespace (`mixamorig:Hips`, `Armature|Hips`) and case and separators gone.
        std::string Normalised( std::string_view name )
        {
            if ( const auto cut = name.find_last_of( ":|" ); cut != std::string_view::npos )
                name.remove_prefix( cut + 1 );
            std::string out;
            out.reserve( name.size() );
            for ( const char c : name )
            {
                if ( c == '_' || c == ' ' || c == '.' || c == '-' )
                    continue;
                out.push_back( static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) ) );
            }
            return out;
        }

        std::optional<std::string> TargetFor( const std::string&                 sourceBone,
                                              const RetargetDocumentModel::Data& data,
                                              const Animation::Skeleton&         target )
        {
            if ( sourceBone.empty() )
                return std::nullopt;
            for ( const auto& rename : data.BoneRenames )
            {
                if ( rename.SourceBone == sourceBone && target.FindBoneIndex( rename.TargetBone ) )
                    return rename.TargetBone;
            }
            if ( target.FindBoneIndex( sourceBone ) )
                return sourceBone;
            const std::string          wanted = Normalised( sourceBone );
            std::optional<std::string> found;
            for ( const auto& bone : target.GetBones() )
            {
                if ( Normalised( bone.Name ) != wanted )
                    continue;
                if ( found )
                    return std::nullopt; // two candidates: no answer, not the first one
                found = bone.Name;
            }
            return found;
        }

        Common::BoolResultStr Initialise( const RetargetDocumentModel::Data& data,
                                          const Animation::Skeleton& source, const Animation::Skeleton& target )
        {
            auto setup = Assets::Serialization::BuildRetargetSetup( data );
            if ( !setup.IsSuccess() )
                return Common::MakeFormattedError<bool>( "{}", setup.GetError() );
            Animation::Retarget::Retargeter retargeter;
            return retargeter.Initialize( source, target, setup.ExtractValue() );
        }
    } // namespace

    RetargetDocumentModel::RetargetDocumentModel( Data data, CommandHistory& history )
         : m_Data( std::move( data ) ), m_Saved( m_Data ), m_History( history )
    {
    }

    RetargetDocumentModel::~RetargetDocumentModel()
    {
        m_History.DropFor( this );
    }

    void RetargetDocumentModel::Commit( Data before, std::string label )
    {
        if ( before == m_Data )
            return;
        ++m_Revision;
        m_History.PushCommand(
             std::make_unique<RetargetEditRecord>( *this, std::move( before ), m_Data, std::move( label ) ) );
    }

    void RetargetDocumentModel::Restore( const Data& data )
    {
        m_Data = data;
        ++m_Revision;
    }

    Common::BoolResultStr RetargetDocumentModel::EditChain( const size_t index, Chain chain )
    {
        if ( index >= m_Data.Chains.size() )
            return Common::MakeFormattedError<bool>( "there is no chain {}; the retarget has {}", index,
                                                     m_Data.Chains.size() );
        if ( chain.Name.empty() )
            return Common::MakeFormattedError<bool>( "a chain needs a name: the resolver names it in every "
                                                     "message it writes" );
        for ( size_t i = 0; i < m_Data.Chains.size(); ++i )
        {
            if ( i != index && m_Data.Chains[i].Name == chain.Name )
                return Common::MakeFormattedError<bool>( "another chain is already called '{}'", chain.Name );
        }
        Data before          = m_Data;
        m_Data.Chains[index] = std::move( chain );
        Commit( std::move( before ), std::format( "Edit chain '{}'", m_Data.Chains[index].Name ) );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr RetargetDocumentModel::AddChain( std::string name )
    {
        if ( name.empty() )
            return Common::MakeFormattedError<bool>( "a chain needs a name" );
        if ( std::ranges::any_of( m_Data.Chains, [&]( const Chain& c ) { return c.Name == name; } ) )
            return Common::MakeFormattedError<bool>( "another chain is already called '{}'", name );
        Data before = m_Data;
        m_Data.Chains.push_back( Chain{ .Name = name } );
        Commit( std::move( before ), std::format( "Add chain '{}'", name ) );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr RetargetDocumentModel::RemoveChain( const size_t index )
    {
        if ( index >= m_Data.Chains.size() )
            return Common::MakeFormattedError<bool>( "there is no chain {}; the retarget has {}", index,
                                                     m_Data.Chains.size() );
        Data              before = m_Data;
        const std::string name   = m_Data.Chains[index].Name;
        m_Data.Chains.erase( m_Data.Chains.begin() + static_cast<std::ptrdiff_t>( index ) );
        Commit( std::move( before ), std::format( "Remove chain '{}'", name ) );
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<size_t> RetargetDocumentModel::AutoMap( const Animation::Skeleton& source,
                                                              const Animation::Skeleton& target )
    {
        Data   before = m_Data;
        size_t mapped = 0;
        for ( Chain& chain : m_Data.Chains )
        {
            if ( !source.FindBoneIndex( chain.SourceStartBone ) || !source.FindBoneIndex( chain.SourceEndBone ) )
                continue;
            const auto start = TargetFor( chain.SourceStartBone, m_Data, target );
            const auto end   = TargetFor( chain.SourceEndBone, m_Data, target );
            if ( !start || !end )
                continue;
            chain.TargetStartBone = *start;
            chain.TargetEndBone   = *end;
            ++mapped;
        }
        if ( m_Data.SourcePelvisBone.empty() == false )
        {
            if ( const auto pelvis = TargetFor( m_Data.SourcePelvisBone, m_Data, target ) )
                m_Data.TargetPelvisBone = *pelvis;
        }
        Commit( std::move( before ), "Auto-map chains" );
        return Common::MakeSuccess( mapped );
    }

    Common::BoolResultStr RetargetDocumentModel::Validate( const Animation::Skeleton& source,
                                                           const Animation::Skeleton& target ) const
    {
        return Initialise( m_Data, source, target );
    }

    std::vector<std::string> RetargetDocumentModel::ChainProblems( const Animation::Skeleton& source,
                                                                   const Animation::Skeleton& target ) const
    {
        std::vector<std::string> problems;
        problems.reserve( m_Data.Chains.size() );
        for ( const Chain& chain : m_Data.Chains )
        {
            Data alone         = m_Data;
            alone.Chains       = { chain };
            const auto verdict = Initialise( alone, source, target );
            problems.push_back( verdict.IsSuccess() ? std::string() : verdict.GetError() );
        }
        return problems;
    }

    Common::BoolResultStr RetargetDocumentModel::Save( const std::filesystem::path& path )
    {
        if ( auto saved = Assets::Serialization::SaveRetargetFile( path, m_Data ); !saved.IsSuccess() )
            return saved;
        m_Saved = m_Data;
        return Common::MakeSuccess( true );
    }

    void RetargetDocumentModel::Discard()
    {
        m_History.DropFor( this );
        if ( m_Data != m_Saved )
            Restore( m_Saved );
    }
} // namespace Desert::Editor
