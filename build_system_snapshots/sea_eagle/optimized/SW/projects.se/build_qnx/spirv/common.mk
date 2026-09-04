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
PINFO DESCRIPTION="Vivante SPIRV"
endef

include $(qnx_build_dir)/common.mk
ifeq ($(VIVANTE_ENABLE_3D), 1)
NAME=libSPIRV

#    -I$(AQARCH)/cmodel/inc

EXTRA_INCVPATH += $(driver_root)/hal/inc
EXTRA_INCVPATH += $(driver_root)/hal/user
EXTRA_INCVPATH += $(driver_root)/hal/user/arch
EXTRA_INCVPATH += $(driver_root)/hal/os/qnx/user
EXTRA_INCVPATH += $(driver_root)/sdk/inc
EXTRA_INCVPATH += $(driver_root)/compiler/libVSC/include
EXTRA_INCVPATH += $(driver_root)/compiler/libSPIRV/spvconverter
EXTRA_INCVPATH += $(driver_root)/driver/khronos/libVulkan11/include

# from libSPIRV (trunk/compiler/libSPIRV/makefile.linux)

SOURCE_OBJECTS += $(driver_root)/compiler/libSPIRV/spvconverter/gc_spirv_mempool.o
SOURCE_OBJECTS += $(driver_root)/compiler/libSPIRV/spvconverter/gc_spirv_to_vir.o
SOURCE_OBJECTS += $(driver_root)/compiler/libSPIRV/spvconverter/gc_spirv_ext_inst.o
SOURCE_OBJECTS += $(driver_root)/compiler/libSPIRV/spvconverter/gc_spirv_utils_base.o
SOURCE_OBJECTS += $(driver_root)/compiler/libSPIRV/spvconverter/gc_spirv_misc.o
SOURCE_OBJECTS += $(driver_root)/compiler/libSPIRV/spvconverter/gc_spirv_opcode.o
SOURCE_OBJECTS += $(driver_root)/compiler/libSPIRV/spvconverter/gc_spirv_disassemble.o
SOURCE_OBJECTS += $(driver_root)/compiler/libSPIRV/spvconverter/gc_spirv_spec_constant_op.o

EXTRA_SRCVPATH += $(driver_root)/compiler/libSPIRV/spvconverter

#EXTRA_LIBVPATH += $(INSTALL_ROOT_$(OS))/$(CPU)$(filter le be, $(VARIANT_LIST))$(CPUVARDIR_SUFFIX)/$(INSTALLDIR)
EXTRA_LIBVPATH += $(LOCAL_INSTALL)

OBJECTS_FROM_SRCVPATH := $(basename $(wildcard $(foreach dir, $(EXTRA_SRCVPATH), $(addprefix $(dir)/*., s S c cc cpp))))
MISSING_OBJECTS := $(filter-out $(OBJECTS_FROM_SRCVPATH), $(basename $(SOURCE_OBJECTS)))

EXCLUDE_OBJS += $(addsuffix .o, $(notdir $(filter-out $(basename $(SOURCE_OBJECTS)), $(OBJECTS_FROM_SRCVPATH))))
#$(warning ***** Excluded objects: $(EXCLUDE_OBJS))

include $(MKFILES_ROOT)/qmacros.mk

LIBS += VSC GAL GLSLC

include $(qnx_build_dir)/math.mk

LDFLAGS += -Wl,--version-script=$(driver_root)/compiler/libSPIRV/spvconverter/spvconverter.map

# NOTE: the install_common_driver.sh invoked from screen_install
# will install the libraries from the local install to stage.
ifeq ($(filter so dll, $(VARIANT_LIST)),)
INSTALLDIR=/dev/null
endif

POST_BUILD =$(CP_HOST) $@ $(LOCAL_INSTALL)/$(@F)

include $(MKFILES_ROOT)/qtargets.mk
endif
