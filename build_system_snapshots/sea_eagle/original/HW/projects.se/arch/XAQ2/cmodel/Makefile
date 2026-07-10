#******************************************************************************#
#* Copyright (c) 2008 by Vivante Corporarion.  All Rights Reserved.           *#
#*                                                                            *#
#* The material in this file is confidential and contains trade secrets of    *#
#* of Vivante Corporation.  This is proprietary information owned by Vivante  *#
#* Corporation.  No part of this work may be disclosed, reproduced, copied,   *#
#* transmitted, or used in any way for any purpose, without the express       *#
#* written permission of Vivante Corporation.                                 *#
#*                                                                            *#
#* Main make file for C-Model and test tool.                                  *#
#*                                                                            *#
#* NOTE:                                                                      *#
#*                                                                            *#
#*   There is a BUG in GCC compiler that sometimes causes the generated       *#
#*   executable binary to be generated incorrectly when optimization is       *#
#*   turned on. For that reason we do not use optimization optipons here.     *#
#*                                                                            *#
#******************************************************************************#

################################################################################
# VGPU Hacks


FILENAME := $(wildcard $(AQARCH)/.aqtree)


ifeq "$(FILENAME)" ""
  CHIP ?= gc860
else
  CHIP ?= $(word 2,$(shell cat $(AQARCH)/.aqtree | grep "^ARCH:"))
endif

CHIP := $(addsuffix _hw, $(CHIP))
################################################################################
# Determine verbosity.

VERBOSE ?= 0
at       = $(if $(filter 0,$(VERBOSE)),@)

ifeq ($(shell uname | grep -c MINGW64), 1)
	CMAKE_TARET = -G "MSYS Makefiles" 
else
	CMAKE_TARET = ""
endif

################################################################################
# Compiler definitions.

cmake = cmake

gcDefGen = $(AQROOT)/tools/bin/gcDefineGen.py

LIB_CMODEL_PATH = $(AQARCH)/cmodel/build_cmodel

DEPENDENCIES := $(AQARCH)/cmodel/inc/gcDefines.h inc/AQ.h GenSource/vsiRegisterList.cpp 

LIB_CMODEL = $(LIB_CMODEL_PATH)/libarch.cmodel.so

REG_DIR       = $(AQARCH)/reg

all:$(LIB_CMODEL_PATH) $(DEPENDENCIES) $(LIB_CMODEL)

clean:
	$(at)-rm -rf $(BUILD_ROOT)
	$(at)-rm -rf $(BUILD64_ROOT)
	$(at)-rm -f $(DEPENDENCIES)
	$(at)-rm -rf $(LIB_CMODEL_PATH)
	$(at)cd $(REG_DIR); make clean; cd -;
	


################################################################################
# Internal dependent targets.



$(LIB_CMODEL_PATH):
	@echo "$(MAKELEVEL) [ MKDIR ] $(call path_MACRO,$@)"
	$(at)mkdir -p $@

$(LIB_CMODEL_PATH)/Makefile:$(DEPENDENCIES)
	@echo "$(MAKELEVEL) [ CMAKE ] $(call path_MACRO,$@)"
	$(at)cmake . -B $(LIB_CMODEL_PATH) $(CMAKE_TARET) -DAQARCH=$(AQARCH) -DAQTOOLS=$(AQROOT)/tools -DDV_DUMP=ON -DCMAKE_VERBOSE_MAKEFILE=OFF  -DCMAKE_CXX_FLAGS="-w -DgcdDV_DUMP=1 -g -fPIC" --log-level=ERROR

inc/AQ.h: $(wildcard $(REG_DIR)/*.r $(REG_DIR)/Makefile)
	@echo "$(MAKELEVEL) [ MAKE  ] $(call path_MACRO,$(REG_DIR))"
	$(at)$(MAKE) --no-print-directory -C $(REG_DIR) $(filter all clean,$(MAKECMDGOALS)) 

$(AQARCH)/cmodel/inc/gcDefines.h: $(AQARCH)/cmodel/CModelFeatureDB.csv $(gcDefGen) $(AQTREE) inc/AQ.h
	@echo "$(MAKELEVEL) [ GcDefine ] $(CHIP) $(call path_MACRO,inc)"
	@python $(gcDefGen) $(AQARCH)/cmodel $(CHIP)

GenSource/vsiRegisterList.cpp: $(wildcard $(REG_DIR)/*.r $(REG_DIR)/Makefile) inc/AQ.h
	@echo "$(MAKELEVEL) [ MAKE  ] $(call path_MACRO,$(REG_DIR))"
	$(at)$(MAKE) --no-print-directory -C $(REG_DIR) $(filter all clean,$(MAKECMDGOALS)) 

$(LIB_CMODEL):$(DEPENDENCIES) $(LIB_CMODEL_PATH)/Makefile
	@echo "$(MAKELEVEL) [ MAKE  ] $(call path_MACRO,$(REG_DIR))"
	$(at)$(MAKE) --no-print-directory -C $(LIB_CMODEL_PATH) -s -j 20

