################################################################################
# Macros.

MODULE_NAME ?= drmcore

AQROOT              ?= ./
UAPI_DIR			:= include/uapi/drm
ARCH_DIR			:= $(AQROOT)/../../arch
export AQROOT

OBJS := galcore_drv.o \
	galcore_device.o \
	galcore_debugfs.o \
	galcore_gem.o \
	galcore_irq.o \
	galcore_gpu.o \
	galcore_ttm.o \
	galcore_vm.o \
	galcore_mmu.o \
	galcore_event.o \




EXTRA_CFLAGS += -DDBG=1 -DDEBUG -D_DEBUG -g
EXTRA_CFLAGS += -I$(AQROOT)
EXTRA_CFLAGS += -I$(AQROOT)/include/uapi/drm
EXTRA_CFLAGS += -I$(ARCH_DIR)/XAQ2/cmodel/inc
EXTRA_CFLAGS += -Werror -Wno-implicit-fallthrough
ccflags-$(CONFIG_WERROR) += -Werror

obj-m = $(MODULE_NAME).o

$(MODULE_NAME)-objs  = $(OBJS)

.PHONY: all

all:
	@rm -rf $(OBJS)
	@rm -f $(MODULE_NAME).ko $(MODULE_NAME).o $(MODULE_NAME).mod.[co] .galcore.*.cmd
	@$(MAKE) -C $(KERNEL_DIR) M=`pwd` modules
