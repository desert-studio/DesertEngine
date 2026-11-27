#include <Engine/Reflection/FunctionThunk.hpp>

namespace Desert::Reflection
{
    namespace
    {
        thread_local std::optional<std::string> t_CallError;
    } // namespace

    void RaiseCallError( std::string message )
    {
        t_CallError = std::move( message );
    }

    namespace Detail
    {
        void ClearCallError()
        {
            t_CallError.reset();
        }

        std::optional<std::string> TakeCallError()
        {
            return std::exchange( t_CallError, std::nullopt );
        }
    } // namespace Detail
} // namespace Desert::Reflection
