LOCAL_PATH := $(call my-dir)
include $(LOCAL_PATH)/../../../Android.mk.def

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
    gc_cl.c \
    gc_cl_log.c  \
    gc_cl_command.c \
    gc_cl_context.c \
    gc_cl_device.c \
    gc_cl_enqueue.c \
    gc_cl_event.c \
    gc_cl_extension.c \
    gc_cl_api.c \
    gc_cl_kernel.c \
    gc_cl_chip.c \
    gc_cl_mem.c \
    gc_cl_platform.c \
    gc_cl_program.c \
    gc_cl_profiler.c \
    gc_cl_tracer.c \
    gc_cl_sampler.c \
    gc_cl_gl.c \
    gc_cl_command_buffer.c \
    gc_cl_resource.c

LOCAL_CFLAGS := \
    $(CFLAGS) \
    -Wno-sign-compare

LOCAL_CFLAGS += \
    -DCL_USE_DEPRECATED_OPENCL_1_0_APIS \
    -DCL_USE_DEPRECATED_OPENCL_1_1_APIS \
    -DCL_TARGET_OPENCL_VERSION=300

LOCAL_C_INCLUDES := \
    $(AQROOT)/sdk/inc \
    $(AQROOT)/hal/inc \
    $(AQROOT)/hal/os/linux/user \
    $(AQROOT)/compiler/libVSC/include \
    $(AQROOT)/hal/user \
    $(AQARCH)/cmodel/inc \
    $(AQROOT)/compiler/libSPIRV/spvconverter \
    $(AQROOT)/hal/user/arch \
    $(LOCAL_PATH)

ifeq ($(shell expr $(PLATFORM_SDK_VERSION) ">=" 26),1)
LOCAL_C_INCLUDES += \
    frameworks/native/libs/nativewindow/include \
    frameworks/native/libs/nativebase/include \
    frameworks/native/libs/arect/include
endif

ifeq ($(shell expr $(PLATFORM_SDK_VERSION) ">=" 28),1)
LOCAL_C_INCLUDES += \
        system/core/include
endif

ifeq ($(BUILD_OPENCL_ICD),1)
CL_SUFFIX=_ICD
else
CL_SUFFIX=30
endif

LOCAL_LDFLAGS := \
    -Wl,-z,defs \
    -Wl,--version-script=$(LOCAL_PATH)/libOpenCL$(CL_SUFFIX).map
    
LOCAL_SHARED_LIBRARIES := \
    liblog \
    libdl \
    libVSC \
    libGAL \
    libCLC \
    libSPIRV_viv

ifeq ($(ENABLE_CL_GL), 1)
LOCAL_SHARED_LIBRARIES += \
    libEGL 
endif

ifeq ($(BUILD_OPENCL_ICD),1)
LOCAL_CFLAGS         += -DBUILD_OPENCL_ICD=1
LOCAL_MODULE         := libVivanteOpenCL
else
LOCAL_MODULE         := libOpenCL
endif

ifneq ("$(VIV_GPU_CONFIG)","")
LOCAL_CFLAGS         += -DOCL_EMBED_RESOURCE=1
endif

ifeq ($(ENABLE_CL_GL), 1)
LOCAL_CFLAGS         += -DgcdENABLE_CL_GL=1
else
LOCAL_CFLAGS         += -DgcdENABLE_CL_GL=0
endif

LOCAL_MODULE_TAGS    := optional
LOCAL_PRELINK_MODULE := false
ifeq ($(PLATFORM_VENDOR),1)
LOCAL_VENDOR_MODULE  := true
endif
include $(BUILD_SHARED_LIBRARY)

