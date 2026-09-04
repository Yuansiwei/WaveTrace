#!/bin/bash

build_path=$(pwd)
workspace=$(realpath "$build_path/../../../")
LLVM_ROOT=$workspace/compiler/vcc2/LLVM-Master

cd $LLVM_ROOT
export LLVM_ROOT=$LLVM_ROOT
export VCC_CORE_PATH=$workspace/compiler/cuda/VCC2/VCC_Core
cp -rfv $VCC_CORE_PATH/include/drvi/VivanteGPUKEP.h $LLVM_ROOT/llvm/lib/Target/VivanteGPU/
export LLVM_PROJECT_HOME=$LLVM_ROOT/project
export LLVM_VEC1_SDK=$LLVM_ROOT/llvm_vec1_sdk
cmake -S llvm -B $LLVM_PROJECT_HOME -DCMAKE_INSTALL_PREFIX=$LLVM_VEC1_SDK -DLLVM_ENABLE_PROJECTS="llvm;clang;lld" -DLLVM_TARGET_TO_BUILD="X86" -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD="VivanteGPU" -DLLVM_ENABLE_ASSERTIONS=ON -DLLVM_PARALLEL_TABLEGEN_JOBS=8 -DLLVM_BUILD_LLVM_C_DYLIB=OFF -DLLVM_INSTALL_UTILS=ON -DCMAKE_BUILD_TYPE=Release 
cd project
cmake --build . -j 20 2>&1 | tee build.log
cd $build_path 


