AQROOT ?= ../../..
AQARCH ?= ..
REGDIR ?= .
VERBOSE ?= 0

################################################################################
# Define target directories.

HAL_INC	    ?=	$(AQROOT)/hal/inc
CHIP_INC	?=	$(AQARCH)/chip/inc
CMODEL_DIR  ?=  $(AQARCH)/cmodel
CMODEL_INC	?=	$(AQARCH)/cmodel/inc
CMODEL_CPP  ?=  $(AQARCH)/cmodel/common
GEN_SRC     ?=  $(AQARCH)/cmodel/GenSource
XML_PATH	?=	$(AQARCH)/doc/hwSpecs/XML
PROBE_PATH   =  $(REGDIR)/Probe_Vit

at = $(if $(filter 0,$(VERBOSE)),@,)

ifeq ($(shell uname | grep -c Darwin), 1)
REGGEN ?= $(AQROOT)/tools/bin/osx/reggen
else ifeq ($(shell uname | grep -c MINGW64), 1)
REGGEN ?= $(AQROOT)/tools/bin/RegGen.exe
else
REGGEN ?= $(AQROOT)/tools/reggen/build/Linux/RegGen
endif
REGGEN_FILES = $(wildcard $(AQROOT)/tools/reggen/*.cpp \
                          $(AQROOT)/tools/reggen/*.h \
                          )

PRUNE ?= $(AQROOT)/tools/bin/prune_dma.py
VSIM_CONFIG_FILE=$(AQROOT)/tools/bin/gc_vsim_configs.h
################################################################################
# Define a list of .R files.

REGFILES = $(wildcard *.r)

################################################################################
# Define targets.

TARGETS = $(CMODEL_INC)/AQ.h \
		  $(HAL_INC)/probeStatistics_Driver.h
ifeq ($(shell uname | grep -c Darwin), 0)
ifeq ($(shell uname | grep -c MINGW64), 0)
TARGETS += $(CHIP_INC)/AQ.vh \
           $(XML_PATH)/AQ.xml \
           $(XML_PATH)/customer_AQDma.xml
endif
endif

.PHONY: all
all : $(TARGETS)

# possible legacy files which have been removed from xx.r to clean up, better to use build directory then can remove all, or use special output names
# currently use cmodel\inc\register or chip\inc\register will need driver and RTL/DV change, so still output to cmodel\inc
LEGACY_C_FILES = \
	$(CMODEL_INC)/AQHostInterface.h 
	
LEGACY_V_FILES = \
	$(CHIP_INC)/AQHostInterface.vh 
	
LEGACY_X_FILES = \
	$(XML_PATH)/AQHostInterface.xml 

.PHONY: clean
clean :
	$(at)rm -f \
		$(REGFILES:%.r=$(CHIP_INC)/%.vh) \
		$(REGFILES:%.r=$(CHIP_INC)/%Reg.vh) \
		$(REGFILES:%.r=$(CMODEL_INC)/%.h) \
		$(REGFILES:%.r=$(CMODEL_INC)/%Reg.h) \
		$(REGFILES:%.r=$(XML_PATH)/%.xml) \
		$(REGFILES:%.r=$(XML_PATH)/%Reg.xml) \
		$(CHIP_INC)/AQTextDef.vh \
		$(CHIP_INC)/gcreg*.vh \
		$(CHIP_INC)/.generatedFiles \
		$(CHIP_INC)/.gitignore \
		$(CMODEL_INC)/AQTextDef.h \
		$(CMODEL_INC)/gcreg*.h \
		$(CMODEL_INC)/.generatedFiles \
		$(CMODEL_INC)/.gitignore \
		$(XML_PATH)/AQTextDef.xml \
		$(XML_PATH)/gcreg*.xml \
		$(XML_PATH)/customer_AQDma.xml \
		$(XML_PATH)/customer_2DReg.xml \
		$(XML_PATH)/customer_3DReg.xml \
		$(LEGACY_C_FILES) \
		$(LEGACY_V_FILES) \
		$(LEGACY_X_FILES)
	$(at)cd $(PROBE_PATH); make -f Makefile clean; cd -
	$(at)cd $(AQROOT)/tools/reggen; make -f Makefile clean; cd -

define sort_MACRO
	$(at)mv $(1) $(1).us
	$(at)sort -u < $(1).us > $(1)
	$(at)rm $(1).us
endef

################################################################################
# Define dependencies.

$(REGGEN): $(REGGEN_FILES)
	@echo "[ MAKE  ] $(subst $(AQROOT)/,,$(AQROOT)/tools/reggen)"
	$(at)$(MAKE) \
		--no-print-directory \
		-C $(AQROOT)/tools/reggen \
		$(filter all clean,$(MAKECMDGOALS))

$(CHIP_INC)/AQ.vh: $(REGGEN) $(REGFILES) ModulePartition.txt StateLoadTree.txt Makefile
	@echo "[ REG   ] $(subst $(AQROOT)/,,$@)"
	$(at)mkdir -p $(CHIP_INC)
	$(at)$(REGGEN) \
		AQ.r \
		-v $(CHIP_INC)/AQ.vh \
		-DisableCmdAddress \
		-SeparateAHBAddr \
		$(if $(filter 0,$(VERBOSE)),-s)

	$(at)rm -f *.h *.xml
	@ #use file listed in AQ.vh to merge and generate vsiTotalDefine.vh. grep maybe an alias, so need use \grep to run original command
	$(at)cd $(CHIP_INC); echo ""> vsiTotalDefine.vh; \grep "include.*\.vh" AQ.vh |\grep -Eo "(AQ|gc|vsi).*\.vh" | xargs cat >> vsiTotalDefine.vh ; cd - 
	
$(HAL_INC)/probeStatistics_Driver.h:$(PROBE_PATH)/Makefile
	@-mkdir -p ${CMODEL_INC}
	@-mkdir -p ${CHIP_INC} 
	@-mkdir -p $(AQROOT)/hal/inc
	$(at)cd $(PROBE_PATH); make -f Makefile
	
$(XML_PATH)/AQ.xml: $(REGGEN) $(REGFILES) Makefile
	@echo "[ REG   ] $(subst $(AQROOT)/,,$@)"
	$(at)mkdir -p $(XML_PATH)
	$(at)$(REGGEN) \
		AQ.r \
		-DisableCmdAddress \
		-SeparateAHBAddr \
		-x $(XML_PATH)/AQ.xml \
		$(if $(filter 0,$(VERBOSE)),-s)
	$(at)rm -f *.h *.vh
#ifeq ($(shell uname | grep -c Darwin), 0)
#	perl -I $(REGDIR) $(AQROOT)/tools/bin/RegGen_XML_Customer.pl -core_type 2D
#	perl -I $(REGDIR) $(AQROOT)/tools/bin/RegGen_XML_Customer.pl -core_type 3D
#endif

$(CMODEL_INC)/AQ.h: $(REGGEN) $(REGFILES) ModulePartition.txt StateLoadTree.txt Makefile
	@echo "[ REG   ] $(subst $(AQROOT)/,,$@)"
	$(at)mkdir -p $(CMODEL_INC)
	$(at)$(REGGEN) \
		AQ.r \
		-c $(CMODEL_INC)/AQ.h \
		-DisableCmdAddress \
		-SeparateAHBAddr \
		$(if $(filter 0,$(VERBOSE)),-s)
	$(at)cp $(GEN_SRC)/vsiRegisterList.cpp $(CHIP_INC)/vsiRegisterListForRTL.txt
	$(at)rm -f *.vh *.xml

$(XML_PATH)/customer_AQDma.xml: $(XML_PATH)/AQ.xml
	@echo "[ REG   ] $(subst $(AQROOT)/,,$@)"
	$(at)cd $(XML_PATH); python $(PRUNE) > /dev/null

ifeq ($(VSIM_CONFIG_FILE), $(wildcard $(VSIM_CONFIG_FILE)))
	@cp -f $(VSIM_CONFIG_FILE) $(AQROOT)/hal/inc/gc_vsim_configs.h
endif
