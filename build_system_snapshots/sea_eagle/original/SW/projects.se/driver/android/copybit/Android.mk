LOCAL_PATH := $(call my-dir)
include $(LOCAL_PATH)/../../../Android.mk.def


#
# copybit.<property>.so
#
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
	gc_copybit.cpp \
	gc_copybit_misc.cpp \
	gc_copybit_blit.cpp \
	gc_copybit_blit_pe1x.cpp

LOCAL_CFLAGS := \
	$(CFLAGS) \
	-DLOG_TAG=\"v_copybit\"

LOCAL_C_INCLUDES := \
	$(AQROOT)/hal/user \
	$(AQROOT)/hal/os/linux/user \
	$(AQROOT)/hal/inc \
	$(AQROOT)/driver/android/gralloc \
	$(AQROOT)/compiler/libVSC/include

LOCAL_SHARED_LIBRARIES := \
	liblog \
	libGAL

LOCAL_MODULE         := copybit.$(GPU_VENDOR)
LOCAL_MODULE_TAGS    := optional
LOCAL_MODULE_PATH    := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_PRELINK_MODULE := false
ifeq ($(PLATFORM_VENDOR),1)
LOCAL_VENDOR_MODULE  := true
endif
include $(BUILD_SHARED_LIBRARY)

