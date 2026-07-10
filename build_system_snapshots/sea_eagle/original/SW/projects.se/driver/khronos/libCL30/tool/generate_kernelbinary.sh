#!/bin/bash

if [ "${VIV_GPU_CONFIG}" == "" ]; then
    exit -1
fi

build_dir=buildc
libcl30_dir=`pwd`
export AQROOT=${libcl30_dir}/../../../
export AQARCH=${AQROOT}/arch/XAQ2
export VIVANTE_SDK_DIR=${libcl30_dir}/tool/${build_dir}


VIV_ENV="AQROOT=${AQROOT} AQARCH=${AQROOT}/arch/XAQ2 VIVANTE_SDK_DIR=${VIVANTE_SDK_DIR}"
VIV_FLAGS="                                 \
    QNX=0                                  \
    gcdkREPORT_VIDMEM_USAGE=0              \
    USE_ARMCC=0                            \
    NO_DMA_COHERENT=1                      \
    ENDIANNESS=                            \
    ENABLE_ARM_L2_CACHE=0                  \
    USE_PLATFORM_DRIVER=1                  \
    USE_VDK=1                              \
    EGL_API_FB=1                           \
    EGL_API_DRI=0                          \
    X11_DRI3=0                             \
    EGL_API_WL=0                           \
    EGL_API_DFB=0                          \
    EGL_API_X=0                            \
    EGL_API_GBM=0                          \
    EGL_API_NULLWS=0                       \
    LINUX_OABI=0                           \
    VIVANTE_ENABLE_VX_GPU_SH=1             \
    VIVANTE_ENABLE_DEV_BUILD=0             \
    VIVANTE_NO_3D=0                        \
    VIVANTE_ENABLE_3D=1                    \
    VIVANTE_ENABLE_2D=1                    \
    VIVANTE_ENABLE_VG=0                    \
    VIVANTE_NO_VG=0                        \
    VIVANTE_ENABLE_DRM=0                   \
    USE_KMS=0                              \
    MXC_FBDEV=0                            \
    GC355_MEM_PRINT=0                      \
    GC355_PROFILER=0                       \
    ABI=0                                  \
    GL4_DRI_BUILD=0                        \
    DEBUG=1                                \
    CUSTOM_PIXMAP=0                        \
    ANDROID=0                              \
    EGL_API_ANDROID=0                      \
    ANDROID_VERSION_ECLAIR=0               \
    DIRECTFB_MAJOR_VERSION=1               \
    DIRECTFB_MINOR_VERSION=4               \
    DIRECTFB_MICRO_VERSION=0               \
    USE_NEW_LINUX_SIGNAL=0                 \
    ENABLE_GPU_CLOCK_BY_DRIVER=0           \
    gcdNO_POWER_MANAGEMENT=0               \
    USE_POWER_MANAGEMENT=1                 \
    gcdCACHE_FUNCTION_UNIMPLEMENTED=0      \
    CACHE_FUNCTION_UNIMPLEMENTED=0         \
    USE_OPENVX=0                           \
    USE_VIP_ONLY=0                         \
    USE_OVXLIB=0                           \
    USE_VULKAN=0                           \
    USE_LOADTIME_OPT=1                     \
    USE_355_VG=0                           \
    FPGA_BUILD=0                           \
    USE_DESTRUCTOR=1                       \
    LINUX_EMULATOR=0                       \
    LINUX_VSIMULATOR=0                     \
    VDB_SERVER=0                           \
    USE_BANK_ALIGNMENT=0                   \
    USE_VXC_BINARY=0                       \
    GPU_CONFIG=default                     \
    USE_VXC_EMBEDDED=0                     \
    USE_VSC_LITE=0                         \
    USE_STATIC_STDCxx=0                    \
    REMOVE_CL_LIBS=0                       \
    ENABLE_VIDEO_MEMORY_MIRROR=0           \
                                           \
    gcdIGNORE_DRIVER_VERSIONS_MISMATCH=0   \
    USE_OPENCL=1                           \
    NO_KERNEL=1                            \
    VIVANTE_ENABLE_40BITS_VA=0             \
    STATIC_LINK=1                          \
    gcdSTATIC_LINK=1                       \
    ENABLE_CL_GL=0                         \
    gcdENABLE_CL_GL=0                      \
    "


cd $AQARCH/reg
make -f makefile.linux ${VIV_FLAGS} ${VIV_ENV}

mkdir -p ${libcl30_dir}/tool/${build_dir}
cd ${libcl30_dir}/tool/${build_dir}

local depends=(hal/user/ hal/user/arch/ hal/os/linux/user/ compiler/libVSC/ compiler/libCLC/common/ compiler/libCLC/compiler/ compiler/libCLC/entry/ compiler/libCLC/preprocessor/ compiler/libSPIRV/ driver/khronos/libCL30)
for project in ${depends[*]}
do
    if [ -e "$AQROOT/$p" ]; then
        cd $AQROOT/$project
        echo "compile $project"
        VIV_EXTRA="${VIV_ENV} OBJ_DIR=${VIVANTE_SDK_DIR}/${project}-build SDK_DIR=${VIVANTE_SDK_DIR} ARCH_TYPE=x86 CPU_ARCH=0 CPU_TYPE=0 CROSS_COMPILE= "
        make -C $AQROOT/$project -f makefile.linux ${VIV_FLAGS} ${VIV_EXTRA} install
    fi
done

inc="-I${AQROOT}/arch/XAQ2/cmodel/inc -I${AQROOT}/sdk/inc -I${AQROOT}/hal/inc -I${AQROOT}/hal/user -I${AQROOT}/hal/user/arch \
     -I${AQROOT}/compiler/libVSC/include -I${AQROOT}/hal/os/emulator/user -I${AQROOT}/compiler/libSPIRV/spvconverter"
flags="-DNO_KERNEL -DgcdSTATIC_LINK -DCL_TARGET_OPENCL_VERSION=300
    -DCL_USE_DEPRECATED_OPENCL_1_0_APIS -DCL_USE_DEPRECATED_OPENCL_1_1_APIS"
all_link_dep="-lOpenCL -lSPIRV_viv -lCLC -lclCompiler -lclPreprocessor -lclCommon -lVSC -lGAL -lhalarchuser -lhalosuser"

cd ${libcl30_dir}/tool/${build_dir}
echo `pwd`
gcc  ../gc_cl_tools.c ${flags} ${inc} -L${VIVANTE_SDK_DIR}/drivers/ -Wl,--start-group ${all_link_dep} -Wl,--end-group -lm -lpthread -o kernelBinaryGenerator


echo "generate libclc"
IFS=";"
configs=(${VIV_GPU_CONFIG})

idx=0
if [ "${configs}" != "" ]; then
    for config in ${configs[*]}
    do
    echo "compile for config ${config}"
    echo ${config} | sed -E "s/chipModel=(0x[0-9a-fA-F]*),chipRevision=(0x[0-9a-fA-F]*),productID=(0x[0-9a-fA-F]*),ecoID=(0x[0-9a-fA-F]*),customerID=(0x[0-9a-fA-F]*)/\
chipModel       = \1;\n\
chipRevision    = \2;\n\
productID       = \3;\n\
ecoID           = \4;\n\
customerID      = \5;\n/" > viv_gpu_config_${idx}.config
     export VIV_GPU_FILE=./viv_gpu_config_${idx}.config
    ./kernelBinaryGenerator
    ((idx = ${idx} + 1))
    done
fi


files=`ls libclc*.lib`

./kernelBinaryGenerator -merge libclc ${files}
cp -vf libclc.inc ${libcl30_dir}


cd ${libcl30_dir}
echo "static const char viv_resource_libclc [RESOURCE_CAPABILITY] = { " > resource.h
cat ./libclc.inc >> resource.h
echo "};" >> resource.h
