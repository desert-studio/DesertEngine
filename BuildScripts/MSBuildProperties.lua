-- MSBuild project properties every generated .vcxproj carries, and the generation-time census that
-- refuses a .vcxproj without them. Included from Workspace.lua inside the workspace scope, so every
-- project — first-party, test suite and vendored (BuildScripts/ThirdParty/*.lua) — inherits it.
--
-- ── IN-PROJECT PARALLEL COMPILE (BUILD1 P0, BuildScripts/BUILD1-CONTRACT.md §3) ─────────────────────
--
-- WHAT WAS WRONG. The default MSBuild CL task hands EVERY source of a project to ONE cl.exe, which
-- compiles them one after another. `msbuild -m` parallelises projects, not the files inside them, so
-- Desert's 394 translation units compiled on one core (696 s serial, measured on the 2026-10-06 Debug
-- tree), and so did Editor's 161 at the end of every build, when nothing else is left to overlap with.
-- Until this file the only in-project parallelism was in CI, as a side effect of the ccache wiring.
--
-- WHAT THESE TWO PROPERTIES DO, AND WHY NOT /MP.
--   UseMultiToolTask=true                 one cl.exe PER SOURCE, scheduled by MSBuild itself.
--   EnforceProcessCountAcrossBuilds=true  those compilers draw from ONE machine-wide budget shared by
--                                         every `msbuild -m` node (and by Visual Studio's parallel
--                                         project build), sized to the logical core count.
-- `/MP` (premake's MultiProcessorCompile) would also run several compilers, but each project counts on
-- its own, so `-m` x `/MP` can put cores x cores compilers on the machine — the hazard the note in
-- Workspace.lua records. The shared budget is what removes it, so neither `/MP` nor a per-invocation
-- `-p:CL_MPCount` is needed or wanted.
--
-- WHY `vsprops` AND NOT A props FILE. premake 5.0-beta8 spells arbitrary project properties as
-- `vsprops { Name = "value" }` and writes them into the per-configuration `Label="Globals"`
-- PropertyGroup, ahead of Microsoft.Cpp.Default.props. That position matters: Ccache.props records that
-- an import placed after the project body (ForceImportBeforeCppTargets) left UseMultiToolTask without
-- effect. Being in the generated project, it is also what a developer's Visual Studio build gets, with
-- no command-line property to remember.
vsprops {
    UseMultiToolTask = "true",
    EnforceProcessCountAcrossBuilds = "true",
}

-- ── THE CENSUS: A GENERATED .vcxproj THAT LOST EITHER PROPERTY, OR GOT /ZI BACK, FAILS GENERATION ──
--
-- Read from the FILE premake just wrote, not from the configuration model: the question is what MSBuild
-- will see, and a project-level `vsprops` or `editandcontinue` that overrides the workspace one only
-- shows up there. premake exits non-zero, so `premake5 vs2022` — locally, in Setup.bat and in CI — is
-- red before any compiler runs.
--
-- /ZI (EditAndContinue) is refused for the reason Configurations.lua turns it off: it makes every
-- compiler of a project write into one shared vc143.pdb through mspdbsrv, a serialisation point the
-- parallel compile above runs straight into, and the owner does not use Edit and Continue (2026-10-06).
--
-- A project with no <ClCompile Include=...> item (a Utility project such as BuildAllTests) compiles
-- nothing, so the compile properties are not required of it.
local function DesertCheckGeneratedVcxproj(filename)
    local text = io.readfile(filename)
    if text == nil then
        error(string.format("BUILD1 census: cannot read the .vcxproj premake just wrote: %s", filename), 0)
    end
    if text:find("<DebugInformationFormat>EditAndContinue</DebugInformationFormat>", 1, true) then
        error(string.format("BUILD1 census: %s compiles with /ZI (EditAndContinue); Debug must be /Zi "
            .. "(editandcontinue \"Off\", BuildScripts/Configurations.lua)", filename), 0)
    end
    if text:find("<ClCompile Include=", 1, true) == nil then
        return
    end
    for _, property in ipairs({ "UseMultiToolTask", "EnforceProcessCountAcrossBuilds" }) do
        if not text:find(string.format("<%s>true</%s>", property, property), 1, true) then
            error(string.format("BUILD1 census: %s has no <%s>true</%s>; its sources would compile on "
                .. "one core (BuildScripts/MSBuildProperties.lua)", filename, property, property), 0)
        end
    end
end

premake.override(premake, "generate", function(base, obj, ext, callback)
    local result = base(obj, ext, callback)
    if ext == ".vcxproj" then
        DesertCheckGeneratedVcxproj(premake.filename(obj, ext))
    end
    return result
end)
