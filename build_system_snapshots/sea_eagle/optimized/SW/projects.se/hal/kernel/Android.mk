LOCAL_PATH := $(call my-dir)
include $(LOCAL_PATH)/../../Android.mk.def

ifeq ($(VIVANTE_ENABLE_VSIMULATOR),0)


#
# galcore.ko
#
include $(CLEAR_VARS)

GALCORE_OUT := $(TARGET_OUT_INTERMEDIATES)/GALCORE_OBJ
GALCORE := \
	$(GALCORE_OUT)/galcore.ko

KERNELENVSH := $(GALCORE_OUT)/kernelenv.sh
$(KERNELENVSH):
	rm -rf $(KERNELENVSH)
	echo 'export KERNEL_DIR=$(KERNEL_DIR)' > $(KERNELENVSH)
	echo 'export CROSS_COMPILE=$(CROSS_COMPILE)' >> $(KERNELENVSH)
	echo 'export ARCH_TYPE=$(ARCH_TYPE)' >> $(KERNELENVSH)

GALCORE_LOCAL_PATH := $(LOCAL_PATH)

$(GALCORE): $(KERNELENVSH)
	@cd $(AQROOT)
	source $(KERNELENVSH); $(MAKE) -f Kbuild -C $(AQROOT) \
		AQROOT=$(abspath $(AQROOT)) \
		AQARCH=$(abspath $(AQARCH)) \
		ARCH_TYPE=$(ARCH_TYPE) \
		DEBUG=$(DEBUG) \
		VIVANTE_ENABLE_DRM=$(DRM_GRALLOC) \
		cp $(GALCORE_LOCAL_PATH)/../../galcore.ko $(GALCORE)

LOCAL_PREBUILT_MODULE_FILE := \
	$(GALCORE)

LOCAL_GENERATED_SOURCES := \
	$(AQREG)

LOCAL_GENERATED_SOURCES += \
	$(GALCORE)

ifeq ($(shell expr $(PLATFORM_SDK_VERSION) ">=" 21),1)
  LOCAL_MODULE_RELATIVE_PATH := modules
else
  LOCAL_MODULE_PATH          := $(TARGET_OUT_SHARED_LIBRARIES)/modules
endif

LOCAL_MODULE        := galcore
LOCAL_MODULE_SUFFIX := .ko
LOCAL_MODULE_TAGS   := optional
LOCAL_MODULE_CLASS  := SHARED_LIBRARIES
LOCAL_STRIP_MODULE  := false
include $(BUILD_PREBUILT)

else

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
          gc_hal_kernel_command.c \
          gc_hal_kernel_db.c \
          gc_hal_kernel_event.c \
          gc_hal_kernel_heap.c \
          gc_hal_kernel.c \
          gc_hal_kernel_gmmu.c \
          gc_hal_kernel_video_memory.c

LOCAL_CFLAGS := \
    $(CFLAGS) \
    -DNO_VDT_PROXY

LOCAL_C_INCLUDES := \
	$(AQROOT)/vsimulator/os/linux/inc \
	$(AQROOT)/vsimulator/os/linux/emulator \
    $(AQROOT)/hal/inc \
	$(AQROOT)/hal/user \
	$(AQROOT)/hal/kernel/arch \
	$(AQROOT)/hal/kernel \

LOCAL_MODULE         := libhalkernel

LOCAL_MODULE_TAGS    := optional

LOCAL_PRELINK_MODULE := false

ifeq ($(PLATFORM_VENDOR),1)
LOCAL_VENDOR_MODULE  := true
endif
include $(BUILD_STATIC_LIBRARY)

endif

