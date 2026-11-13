#-------------------------------------------------------------------------------
# TubeNX — YouTube client for Nintendo Switch (Material 3 + ByeByeDPI)
#-------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := TubeNX
BUILD       := build
SOURCES     := source source/core source/md3 source/net source/dpi source/yt source/player source/ui
DATA        :=
INCLUDES    := $(SOURCES) $(DEVKITPRO)/libnx/include $(DEVKITPRO)/portlibs/switch/include
ROMFS       := resources

APP_TITLE   := TubeNX
APP_AUTHOR  := agfk
APP_VERSION := 1.0.0
ICON        := resources/images/icon.jpg

ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS  := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES)
CFLAGS  += $(INCLUDE) -D__SWITCH__ -DTNX_SWITCH
CXXFLAGS := $(CFLAGS) -std=c++17 -fno-exceptions -fno-rtti
# --wrap=sslConnectionSetHostName: см. source/net/sni.cpp — обход SNI-блокировки.
# Остальные --wrap: замеры времени вызовов сервиса ssl (диагностика «почему
# рукопожатие занимает секунды»). На работу не влияют, только пишут в лог.
LDFLAGS  = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) \
           -Wl,--wrap=sslConnectionSetHostName \
           -Wl,--wrap=sslCreateContext \
           -Wl,--wrap=sslContextCreateConnection \
           -Wl,--wrap=socketSslConnectionSetSocketDescriptor \
           -Wl,--wrap=sslContextImportServerPki \
           -Wl,--wrap=sslConnectionDoHandshake \
           -Wl,--wrap=sslInitialize

# Портлибы: libmpv тянет ffmpeg/libass/libplacebo/SDL2/EGL и т.д.
LIBS := -lmpv -lass -lplacebo -lavfilter -lavformat -lavcodec \
        -lswscale -lswresample -lpostproc -ldav1d -lavutil \
        -lharfbuzz -lfribidi -lfreetype -lpng16 -lwebp -larchive \
        -lSDL2 -lcurl -ljansson -lmbedtls -lmbedx509 -lmbedcrypto \
        -lzstd -lbz2 -llzma -llz4 -lexpat -lxml2 \
        -ljpeg -lEGL -ldrm_nouveau -lglapi \
        -lnx -lstdc++ -lm -lz -pthread

LIBDIRS := $(PORTLIBS) $(DEVKITPRO)/libnx

#-------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#-------------------------------------------------------------------------------

export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)

ifeq ($(strip $(CPPFILES)),)
	export LD := $(CC)
else
	export LD := $(CXX)
endif

export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES     := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE := $(foreach dir,$(SOURCES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export APP_ICON  := $(TOPDIR)/$(ICON)
# --nacp обязателен: без него elf2nro не вкладывает в NRO метаданные (название,
# автор, версия), и hbmenu такой файл в списке не показывает.
export NROFLAGS  += --icon=$(APP_ICON) --nacp=$(TOPDIR)/$(TARGET).nacp --romfsdir=$(TOPDIR)/$(ROMFS)

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

#-------------------------------------------------------------------------------
else
#-------------------------------------------------------------------------------
DEPENDS := $(OFILES:.o=.d)

.PHONY: all

all: $(OUTPUT).nro

$(OUTPUT).nro : $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf : $(OFILES)

$(OFILES_SRC) : $(HFILES_BIN)

%.bin.o %_bin.h : %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
#-------------------------------------------------------------------------------