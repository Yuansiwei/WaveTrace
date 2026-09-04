LOCAL_PATH := $(call my-dir)
include $(LOCAL_PATH)/../../../Android.mk.def

#
# hwcomposer.<property>.so
#
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
    hwc2_debug.cpp \
    hwc2_common.cpp \
    hwc2_blitter.cpp \
    hwc2_fbdev.cpp

LOCAL_CFLAGS := \
    $(CFLAGS) \
    -Wall \
    -Wextra \
    -Wno-unused-parameter \
    -Wno-unused-variable \
    -Wno-unused-function \
    -DLOG_TAG=\"hwc\"

LOCAL_C_INCLUDES := \
    $(AQROOT)/hal/inc \
    $(AQROOT)/hal/user \
    $(AQROOT)/hal/os/linux/user \
    $(AQROOT)/driver/android/gralloc \
    $(AQROOT)/compiler/libVSC/include

LOCAL_SHARED_LIBRARIES := \
    libcutils \
    libutils \
    liblog \
    libhardware \
    libsync \
    liblog \
    libGAL

LOCAL_MODULE_RELATIVE_PATH := hw

LOCAL_MULTILIB             := 32

LOCAL_MODULE         := hwcomposer.$(GPU_VENDOR)
LOCAL_MODULE_TAGS    := optional
LOCAL_PRELINK_MODULE := false
ifeq ($(PLATFORM_VENDOR),1)
LOCAL_VENDOR_MODULE  := true
endif
include $(BUILD_SHARED_LIBRARY)


