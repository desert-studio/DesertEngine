# SceneMigrator: compiles the engine sources it needs straight in, data-only reflection
# (DESERT_REFLECTION_DATA_ONLY); built after the engine codegen library, whose header tool writes Reflection*.gen.cpp.
desert_glob(_sources "Source/*.cpp")
file(GLOB _generated CONFIGURE_DEPENDS "${DESERT_ROOT}/Desert/Desert/Source/Engine/Generated/Reflection_*.gen.cpp")
list(SORT _generated)
desert_add_executable(SceneMigrator FOLDER Tools SOURCES ${_sources} ${_generated}
    "${DESERT_ROOT}/Desert/Projects/Source/Projects/StartupLayout.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/Graph/AnimGraphSerialization.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/Graph/PoseGraph.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Assets/Serialization/ControlRig.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/Rig/ControlRigStage.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/Rig/RigGraph.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/Rig/ControlHierarchy.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/Animator.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/Graph/LayeredBlendPerBone.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/Graph/PoseGraphInstance.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/Graph/BoneControlNodes.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/TwoBoneIKControl.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/Graph/AnimGraphEvaluator.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/Graph/AnimGraphValidation.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/Timeline/Player.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/TrackEditing.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/Solvers/TwoBoneIK.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/Retarget/ModelPose.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/Retarget/RetargetPose.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/Retarget/Retargeter.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/Retarget/RetargetSource.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/BoneControl.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Assets/Serialization/Retarget.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Assets/Serialization/FoliageType.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Assets/Serialization/VFXSystem.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Assets/Serialization/VFXEffectType.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/World/Landscape/LandscapeData.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Reflection/ReflectionSerializer.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Reflection/FieldValue.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Reflection/FunctionInfo.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Reflection/ReflectionRegistry.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Generated/Reflection.gen.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Assets/Prefab/PrefabFormat.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Core/Serialize/SceneDependencies.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Core/Serialize/ExternalEntities.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Core/Serialize/EntityDescriptorIndex.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Assets/CloudNoiseVolume.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Assets/SoundFormat.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Assets/CloudTypeData.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Assets/CloudModellingVolume.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Assets/MeshSourceAsset.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Assets/TextureSourceAsset.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Assets/Serialization/AnimationClipBuild.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/Timeline/Channel.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/Timeline/Binding.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/Timeline/Track.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/Timeline/Sequence.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/Timeline/SequenceFormat.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/Timeline/Evaluator.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/KeyInterpolation.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/TimeModel.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/Pose.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/Skeleton.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Animation/SkeletonReference.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Assets/Serialization/MeshBinary.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Assets/Serialization/ImportRecord.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Assets/MeshDerivedData.cpp")
desert_depend_on_codegen(SceneMigrator)
target_compile_definitions(SceneMigrator PRIVATE DESERT_REFLECTION_DATA_ONLY)
target_include_directories(SceneMigrator PRIVATE "${DESERT_ROOT}/Tools/Shared")
desert_engine_includes(SceneMigrator)
desert_external_includes(SceneMigrator "${DESERT_ROOT}/ThirdParty/entt/include" "${DESERT_ROOT}/Editor/Resources/Shaders" ${DESERT_COMMON_INCLUDE_DIRS})
target_link_libraries(SceneMigrator PRIVATE Common Optick ReflectCpp)
desert_platform_defines(SceneMigrator)
