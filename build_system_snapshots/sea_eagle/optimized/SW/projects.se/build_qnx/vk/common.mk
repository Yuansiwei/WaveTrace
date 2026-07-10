#
# Copyright 2010, QNX Software Systems Ltd.  All Rights Reserved
#
# This source code has been published by QNX Software Systems Ltd.
# (QSSL).  However, any use, reproduction, modification, distribution
# or transfer of this software, or any software which includes or is
# based upon any of this code, is only permitted under the terms of
# the QNX Open Community License version 1.0 (see licensing.qnx.com for
# details) or as otherwise expressly authorized by a written license
# agreement from QSSL.  For more information, please email licensing@qnx.com.
#

# find the driver's root directory, which is 3 levels below the current make file
driver_root:=$(abspath ../../$(dir $(lastword $(MAKEFILE_LIST))))
qnx_build_dir:=$(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)

ifndef QCONFIG
QCONFIG=qconfig.mk
endif
include $(QCONFIG)

define PINFO
PINFO DESCRIPTION="Vivante Vulkan"
endef

include $(qnx_build_dir)/common.mk
ifeq ($(VIVANTE_ENABLE_3D), 1)
NAME=libvulkan_viv
EXTRA_INCVPATH += $(driver_root)/hal/inc
EXTRA_INCVPATH += $(driver_root)/hal/user
EXTRA_INCVPATH += $(driver_root)/hal/user/arch
EXTRA_INCVPATH += $(driver_root)/hal/os/qnx/user
EXTRA_INCVPATH += $(driver_root)/sdk/inc
EXTRA_INCVPATH += $(driver_root)/compiler/libVSC/include
EXTRA_INCVPATH += $(driver_root)/compiler/libSPIRV/spvconverter
EXTRA_INCVPATH += $(driver_root)/driver/khronos/libVulkan13/include
EXTRA_INCVPATH += $(driver_root)/arch/XAQ2/cmodel/inc

# from libVulkan (trunk/driver/khronos/libVulkan13/makefile.linux)
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/chip/gc_halti2_chip.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/chip/gc_halti3_chip.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/chip/gc_halti5_chip.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/chip/gc_halti5_pipeline.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/chip/gc_halti5_resource.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/chip/gc_halti5_computeblit.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/chip/gc_halti5_tweak.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/chip/gc_halti5_cmdbuf.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/gc_vk_api.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/gc_vk_chip.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/gc_vk_cmdbuf.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/gc_vk_context.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/gc_vk_devqueue.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/gc_vk_descriptor.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/gc_vk_dispatch.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/gc_vk_framebuffer.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/gc_vk_icd.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/gc_vk_instance.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/gc_vk_noop.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/gc_vk_object.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/gc_vk_phydevice.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/gc_vk_pipeline.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/gc_vk_query.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/gc_vk_resource.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/gc_vk_shader.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/gc_vk_sync.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/gc_vk_utils.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/gc_vk_validation.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/gc_vk_trace.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/wsi/gc_wsi_surface.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/wsi/gc_wsi_screen.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libVulkan13/src/wsi/gc_wsi_swapchain.o

EXTRA_SRCVPATH += $(driver_root)/driver/khronos/libVulkan13/src
EXTRA_SRCVPATH += $(driver_root)/driver/khronos/libVulkan13/src/chip
EXTRA_SRCVPATH += $(driver_root)/driver/khronos/libVulkan13/src/wsi

EXTRA_LIBVPATH += $(LOCAL_INSTALL)

CCFLAGS += -DVK_USE_PLATFORM_SCREEN_QNX
CCFLAGS += -Wno-error=format=2 -Wno-error=format-extra-args

OBJECTS_FROM_SRCVPATH := $(basename $(wildcard $(foreach dir, $(EXTRA_SRCVPATH), $(addprefix $(dir)/*., s S c cc cpp))))
MISSING_OBJECTS := $(filter-out $(OBJECTS_FROM_SRCVPATH), $(basename $(SOURCE_OBJECTS)))
#ifneq ($(MISSING_OBJECTS), )
#$(error ***** Missing source objects:  $(MISSING_OBJECTS))
#endif

EXCLUDE_OBJS += $(addsuffix .o, $(notdir $(filter-out $(basename $(SOURCE_OBJECTS)), $(OBJECTS_FROM_SRCVPATH))))
#$(warning ***** Excluded objects: $(EXCLUDE_OBJS))

include $(MKFILES_ROOT)/qmacros.mk

LIBS += VSC GAL GLSLC SPIRV

include $(qnx_build_dir)/math.mk

LDFLAGS += -Wl,--version-script=$(driver_root)/driver/khronos/libVulkan13/libVulkan.map

# NOTE: the install_common_driver.sh invoked from screen_install
# will install the libraries from the local install to stage.
ifeq ($(filter so dll, $(VARIANT_LIST)),)
INSTALLDIR=/dev/null
endif

POST_BUILD =$(CP_HOST) $@ $(LOCAL_INSTALL)/$(@F)

include $(MKFILES_ROOT)/qtargets.mk
endif
