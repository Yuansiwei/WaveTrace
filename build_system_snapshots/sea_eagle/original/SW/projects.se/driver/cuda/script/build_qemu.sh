export ENABLE_HMM=1
build_path=$(pwd)
workspace=$(realpath "$build_path/../../../")
VCC_ROOT=$workspace/build/sdk/

if [ ! -d "gpgpu" ]; then
    mkdir gpgpu && cd gpgpu
else
    cd gpgpu
fi

base="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ ! -f CMakeCache.txt ]; then
    cmake -DGCDEFINE=cc10200L_0066 -DPLATFORM_TYPE=QEMU -DBUILD_DRM_KMD=ON -DCMAKE_BUILD_TYPE=Debug ../../
fi

cmake --build . -j 20 2>&1 | tee build.log
cd ../

cp -f $LLVM_PROJECT_HOME/lib/libVivanteGPUKernelLaunch.so* $VCC_ROOT/bin/
cp -f $LLVM_PROJECT_HOME/bin/vcc $VCC_ROOT/bin/
cp -f gpgpu/bin/libvcc-core.so $VCC_ROOT/bin/
