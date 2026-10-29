set -x
cd /c/aftmp/gdwt || exit 9
export VULKAN_SDK="C:\VulkanSDK\1.3.296.0" PATH="/c/VulkanSDK/1.3.296.0/Bin:$PATH"
CI=true ./vendor/bin/premake5.exe vs2022 > /c/aftmp/p1/build_nx2.txt 2>&1
MSB="/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/amd64/MSBuild.exe"
"$MSB" build/Projects/DesertHeaderTool.vcxproj -m:2 -p:CL_MPCount=2 -p:Configuration=Release -p:Platform=x64 -v:m >> /c/aftmp/p1/build_nx2.txt 2>&1
"$MSB" build/Projects/Editor.vcxproj -m:2 -p:CL_MPCount=2 -p:Configuration=Release -p:Platform=x64 -v:m >> /c/aftmp/p1/build_nx2.txt 2>&1
rc0=$?; [ $rc0 -ne 0 ] && "$MSB" build/Projects/Editor.vcxproj -p:BuildProjectReferences=false -t:ClCompile -m:2 -p:CL_MPCount=2 -p:Configuration=Release -p:Platform=x64 -v:m >> /c/aftmp/p1/build_nx2.txt 2>&1; (exit $rc0)
rc=$?; echo "rc=$rc $(date +%T)" > /c/aftmp/p1/NX2_DONE; echo rc=$rc; grep -c "Editor.vcxproj ->\|Editor.exe" /c/aftmp/p1/build_nx2.txt
grep -E "error C|error LNK|error MSB" /c/aftmp/p1/build_nx2.txt | sort -u | head -12
