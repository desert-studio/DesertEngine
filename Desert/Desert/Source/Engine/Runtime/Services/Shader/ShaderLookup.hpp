#pragma once

#include <Engine/Graphic/Shader.hpp>

#include <cstdint>
#include <memory>
#include <string>

// THE GLOBAL SHADER LOOKUP (UE: GetGlobalShaderMap() + TShaderMapRef, a thin header every pass includes, while the
// shader map's owner stays out of the pass headers). A header-only render pass resolves its program by name or
// by variant through these two calls; the service behind them (ShaderService.hpp: the compiler, the shader map
// build, the variant tickets) is complete only in ShaderService.cpp. Both forward to the registry's service.
namespace Desert::Runtime
{
    // ShaderService::GetByName: the default program of @p name; null when no shader of that name is loaded.
    [[nodiscard]] std::shared_ptr<Graphic::Shader> FindShader( const std::string& name );

    // ShaderService::AcquireVariant: the compiled @p variant of @p name, or its refusal.
    [[nodiscard]] Common::ResultStr<std::shared_ptr<Graphic::Shader>>
    AcquireShaderVariant( const std::string& name, const Graphic::ShaderVariant& variant );

    // ShaderService::ProgramGeneration: bumped on every recompile of @p name (a cache built from it is stale).
    [[nodiscard]] std::uint64_t ShaderProgramGeneration( const std::string& name );

    // ShaderService::ClaimRefusalReport: whether this caller is the one to log the refusal of (@p name, @p variant).
    [[nodiscard]] bool ClaimShaderRefusalReport( const std::string& name, const Graphic::ShaderVariant& variant );
} // namespace Desert::Runtime
