LOCAL_PATH := $(call my-dir)
include $(LOCAL_PATH)/../../../Android.mk.def

#
# libhalarchuser
#
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
    gc_hal_user_hardware_blt.c \
    gc_hal_user_hardware.c \
    gc_hal_user_hardware_pipe.c \
    gc_hal_user_hardware_primitive.c \
    gc_hal_user_hardware_query.c

ifeq ($(VIVANTE_ENABLE_3D), 1)
LOCAL_SRC_FILES += \
    gc_hal_user_hardware_clear.c \
    gc_hal_user_hardware_shader.c \
    gc_hal_user_hardware_engine.c \
    gc_hal_user_hardware_frag_proc.c \
    gc_hal_user_hardware_texture.c \
    gc_hal_user_hardware_texture_upload.c \
    gc_hal_user_hardware_stream.c
endif

LOCAL_GENERATED_SOURCES := \
    $(AQREG)

LOCAL_CFLAGS := \
    $(CFLAGS) \
    -Wno-unused-function \
    -Werror \

LOCAL_C_INCLUDES := \
    $(AQROOT)/hal/inc \
    $(AQROOT)/hal/user \
    $(AQROOT)/hal/os/linux/user \
    $(AQARCH)/cmodel/inc

ifeq ($(VIVANTE_ENABLE_3D),1)
LOCAL_C_INCLUDES += $(AQROOT)/compiler/libVSC/include
endif

LOCAL_C_INCLUDES += $(LOCAL_PATH)
LOCAL_CFLAGS := \
    $(CFLAGS)

endif

LOCAL_MODULE         := libhalarchuser
LOCAL_MODULE_TAGS    := optional
ifeq ($(PLATFORM_VENDOR),1)
LOCAL_VENDOR_MODULE  := true
endif
include $(BUILD_STATIC_LIBRARY)
