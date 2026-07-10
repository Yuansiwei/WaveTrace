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


ifndef QCONFIG
QCONFIG=qconfig.mk
endif
include $(QCONFIG)

NAME=libOpenCL

include $(qnx_build_dir)/common.mk

ifeq ($(BUILD_OPENCL_ICD),1)
define PINFO
PINFO DESCRIPTION="Vivante OpenCL ICD"
endef
else
define PINFO
PINFO DESCRIPTION="Vivante OpenCL"
endef
endif

EXTRA_INCVPATH += $(driver_root)/hal/inc
EXTRA_INCVPATH += $(driver_root)/hal/user
EXTRA_INCVPATH += $(driver_root)/sdk/inc
EXTRA_INCVPATH += $(driver_root)/compiler/libVSC/include
EXTRA_INCVPATH += $(driver_root)/hal/os/qnx/user
EXTRA_INCVPATH += $(driver_root)/arch/XAQ2/cmodel/inc

# from libCL (trunk/driver/khronos/libCL/makefile.linux)
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libCL/gc_cl.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libCL/gc_cl_log.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libCL/gc_cl_command.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libCL/gc_cl_context.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libCL/gc_cl_device.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libCL/gc_cl_enqueue.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libCL/gc_cl_event.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libCL/gc_cl_extension.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libCL/gc_cl_icd.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libCL/gc_cl_kernel.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libCL/gc_cl_mem.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libCL/gc_cl_platform.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libCL/gc_cl_program.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libCL/gc_cl_profiler.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libCL/gc_cl_sampler.o
SOURCE_OBJECTS += $(driver_root)/driver/khronos/libCL/gc_cl_gl.o

EXTRA_SRCVPATH += $(driver_root)/driver/khronos/libCL

EXTRA_LIBVPATH += $(LOCAL_INSTALL)

OBJECTS_FROM_SRCVPATH := $(basename $(wildcard $(foreach dir, $(EXTRA_SRCVPATH), $(addprefix $(dir)/*., s S c cc cpp))))
MISSING_OBJECTS := $(filter-out $(OBJECTS_FROM_SRCVPATH), $(basename $(SOURCE_OBJECTS)))
ifneq ($(MISSING_OBJECTS), )
$(error ***** Missing source objects:  $(MISSING_OBJECTS))
endif

EXCLUDE_OBJS += $(addsuffix .o, $(notdir $(filter-out $(basename $(SOURCE_OBJECTS)), $(OBJECTS_FROM_SRCVPATH))))
#$(warning ***** Excluded objects: $(EXCLUDE_OBJS))

include $(MKFILES_ROOT)/qmacros.mk

LIBS += $(STATIC_LIBS)
LDOPTS += -lGAL -lGLESv2 -lEGL -lVSC -lCLC

CCFLAGS += -DCL_USE_DEPRECATED_OPENCL_1_0_APIS
CCFLAGS += -DCL_USE_DEPRECATED_OPENCL_1_1_APIS
CCFLAGS += -DCL_TARGET_OPENCL_VERSION=120

# Turn off -Werror=format=2 because the many CL_LOG_API lines have
# many build errors within them.  All are '%d' and '%x' used with wrong
# integer types, so they are harmless as long as nothing tries to consume
# their output.
CCFLAGS += -Wno-error=format=2

# Turn off -Werror=format-nonliteral because of clfPrintData and clfPrintParseData.
# clfPrintData is unfixable without effort comparable with reimplementing printf.
# clfPrintParseData does some strange computed format string thing.
CCFLAGS += -Wno-error=format-nonliteral

include $(qnx_build_dir)/math.mk

ifeq ($(BUILD_OPENCL_ICD),1)
CL_SUFFIX=ICD
else
CL_SUFFIX=
endif

LDFLAGS += -Wl,--version-script=$(driver_root)/driver/khronos/libCL/libOpenCL$(CL_SUFFIX)12.map

ifeq ($(filter so dll, $(VARIANT_LIST)),)
INSTALLDIR=/dev/null
endif

include $(MKFILES_ROOT)/qtargets.mk
