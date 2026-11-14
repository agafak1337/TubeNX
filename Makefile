#-------------------------------------------------------------------------------
# TubeNX — YouTube client for Nintendo Switch (Material 3 + ByeByeDPI)
#-------------------------------------------------------------------------------
# RB-07: host-цели (test-host/preview/doctor/deploy/logs) работают без
# DEVKITPRO — консольный тулчейн нужен только для сборки .nro.
ifeq ($(strip $(DEVKITPRO)),)
HOST_ONLY := 1
endif

TOPDIR ?= $(CURDIR)

ifndef HOST_ONLY
include $(DEVKITPRO)/libnx/switch_rules
endif

TARGET      := TubeNX
BUILD       := build
SOURCES     := source source/core source/md3 source/net source/dpi source/yt source/player source/ui
DATA        :=
INCLUDES    := $(SOURCES) $(DEVKITPRO)/libnx/include $(DEVKITPRO)/portlibs/switch/include
ROMFS       := resources

APP_TITLE   := TubeNX
APP_AUTHOR  := agafak1337
APP_VERSION := 1.0.0
# Иконка должна быть JPEG: hbmenu декодирует иконку NRO как JPEG, а elf2nro
# вкладывает файл как есть, без проверки формата. Мастер — icon.png из
# design/export/icon (вариант A), JPEG-копия собрана tools/png2jpg.cpp.
ICON        := resources/images/icon.jpg

# Идентификация сборки (RB-02/LOG-03): метка git попадает в лог и бинарь.
# Приоритет — git describe --tags (v1.0.0-3-gabc-dirty), иначе короткий sha,
# иначе unknown: сборка из архива без .git не должна ломать make.
GIT_DESC    := $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)
GIT_SHA     := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
BUILD_SHA   ?= $(if $(filter unknown,$(GIT_DESC)),$(GIT_SHA),$(GIT_DESC))
# Дата сборки по умолчанию не вшивается (RB-02: воспроизводимость). Передать
# явно: make BUILD_DATE=2026-11-14 -> в логе будет date=2026-11-14.
BUILD_DATE  ?=
DATE_DEF    := $(if $(BUILD_DATE),-DTNX_BUILD_DATE=\"$(BUILD_DATE)\",)
# DIAG=1 — диагностическая сборка: DEBUG-лог включён (docs/impl-tz.md §7.5).
DIAG        ?= 0
DIAG_DEF    := $(if $(filter 1,$(DIAG)),-DTNX_DIAG=1,)

ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS  := -g -Wall -Wextra -O2 -ffunction-sections $(ARCH) $(DEFINES)
CFLAGS  += $(INCLUDE) -D__SWITCH__ -DTNX_SWITCH \
           -DTNX_VERSION=\"$(APP_VERSION)\" -DTNX_BUILD_SHA=\"$(BUILD_SHA)\" $(DIAG_DEF) \
           $(DATE_DEF)
# RB-02: пути хоста сборки не должны утекать в __FILE__/DWARF — воспроизводимость
# и чистый лог предупреждений (одинаковые строки у всех, кто собирает).
CFLAGS  += -ffile-prefix-map=$(CURDIR)=.
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

# Заголовки libnx/портлиб — через -isystem: RB-03 требует -Wextra для нашего
# кода, но предупреждения из switch.h (HipcMetadata и т.п.) нам не нужны.
export INCLUDE := $(foreach dir,$(SOURCES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-isystem $(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export APP_ICON  := $(TOPDIR)/$(ICON)
# --nacp обязателен: без него elf2nro не вкладывает в NRO метаданные (название,
# автор, версия), и hbmenu такой файл в списке не показывает.
export NROFLAGS  += --icon=$(APP_ICON) --nacp=$(TOPDIR)/$(TARGET).nacp --romfsdir=$(TOPDIR)/$(ROMFS)

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
ifeq ($(HOST_ONLY),1)
	@echo "DEVKITPRO не задан — сборка .nro невозможна." >&2
	@echo "Доступны host-цели: make test-host | make preview | make doctor" >&2
	@exit 1
else
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
endif

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

# --- вспомогательные цели (docs/impl-tz.md §13.2) ---------------------------
.PHONY: test-host preview diag doctor icon deploy logs

# host-тесты без консоли (RB-07, TEST-01); CI вызывает эту цель.
test-host:
	@bash tools/test_host.sh

# offscreen-предпросмотр экранов в PNG (только для разработки на ПК).
preview:
	@bash tools/preview.sh /tmp/tubenx-preview

# диагностический .nro с -DTNX_DIAG=1 (docs/impl-tz.md §7.5).
diag:
	@$(MAKE) --no-print-directory DIAG=1 TARGET=TubeNX-diag BUILD=build-diag

# печатает версии тулчейна и наличие портлиб (RB-04).
doctor:
	@bash tools/doctor.sh

# ресурсы иконки/NACP.
icon:
	@python3 tools/make_icon.py

# копирование на SD (если подключена).
deploy:
	@bash tools/deploy.sh

# хвост лога с SD.
logs:
	@bash tools/pull-log.sh

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