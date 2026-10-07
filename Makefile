# Makefile - Hotspot, a RISC OS desktop controller for WPSD hotspots
#
#   make            build the RISC OS application into build/riscos/!Hotspot
#   make test       build and run the host tests (unit + integration)
#   make clean
#
# The RISC OS build uses the same recipe as the other projects here: the
# GCCSDK 10.2 hard-float toolchain, OSLib, a static ELF (filetype &E1F).

# ---------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------

# Portable core: no RISC OS dependencies, also built and tested on the host.
CORE := util html json http wpsd scan client rows

# RISC OS front end.
RO_SRC := ro_main ro_win ro_dlg ro_menu ro_act ro_choices ro_info

# ---------------------------------------------------------------------
# RISC OS build
# ---------------------------------------------------------------------

CROSS_PREFIX ?= arm-riscos-gnueabihf-

RO_CC      := $(CROSS_PREFIX)gcc
RO_STRIP   := $(CROSS_PREFIX)strip
RO_CFLAGS  := -std=gnu11 -Wall -Wextra -O2 -I$(HOME)/gccsdk/env/include -Isrc
RO_LDFLAGS := -static -L$(HOME)/Development/OSLib/Build
RO_LIBS    := -lOSLib32

RO_BUILD   := build/riscos
APP        := $(RO_BUILD)/!Hotspot

# The version number lives in one place: src/ro.h.
VERSION    := $(shell sed -n 's/^\#define APP_VERSION *"\(.*\)"/\1/p' src/ro.h)
ZIP        := $(RO_BUILD)/Hotspot-$(VERSION).zip
RO_OBJ     := $(addprefix $(RO_BUILD)/obj/,$(addsuffix .o,$(CORE) $(RO_SRC)))

.PHONY: all app clean test dist probe

all: app

app: $(APP)/!RunImage,e1f $(APP)/!Sprites,ff9 app-files

$(RO_BUILD)/obj/%.o: src/%.c $(wildcard src/*.h)
	@mkdir -p $(dir $@)
	$(RO_CC) $(RO_CFLAGS) -c -o $@ $<

$(APP)/!RunImage,e1f: $(RO_OBJ)
	@mkdir -p $(APP)
	$(RO_CC) $(RO_LDFLAGS) -o $@ $(RO_OBJ) $(RO_LIBS)
	$(RO_STRIP) $@

$(APP)/!Sprites,ff9: tools/mksprites.py
	@mkdir -p $(APP)
	python3 tools/mksprites.py $@

# Hotspot-<version>.zip: the application folder and the ReadMe, with the file
# types in the zip's RISC OS ("ARC0") extra fields, as RISC OS zip tools do.
dist: app
	python3 tools/mkdist.py $(ZIP) app/'ReadMe,fff' LICENSE
	@echo "version $(VERSION)"

# The files that are written by hand live in app/!Hotspot with RISC OS file
# type suffixes (,feb Obey; ,fff Text) so smbclient/zip keep the types.
.PHONY: app-files
app-files:
	@mkdir -p $(APP)
	rm -f $(APP)/Choices $(APP)/!Help $(APP)/!Run $(APP)/!Boot
	cp -f app/'!Hotspot'/* $(APP)/

# ---------------------------------------------------------------------
# Host tests
# ---------------------------------------------------------------------

# The GCCSDK environment directory (which the README puts on PATH for the
# RISC OS build) contains a `gcc` that is the *cross* compiler, so the host
# tests name the system compiler explicitly.
HOST_CC     ?= /usr/bin/gcc
HOST_CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
               -fno-omit-frame-pointer -Isrc
HOST_BUILD  := build/host
HOST_OBJ    := $(addprefix $(HOST_BUILD)/,$(addsuffix .o,$(CORE)))

# Short timeouts so the integration tests of the timeout paths are quick.
IT_DEFS := -DHS_FETCH_TIMEOUT=60 -DHS_FORM_TIMEOUT=200 -DHS_SYS_TIMEOUT=200 \
           -DHS_AFTER_ACTION_CS=10

test: $(HOST_BUILD)/test_core $(HOST_BUILD)/it_client $(HOST_BUILD)/ui_sim \
      $(HOST_BUILD)/fuzz_parsers
	test/run_tests.sh

$(HOST_BUILD)/%.o: src/%.c $(wildcard src/*.h)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -c -o $@ $<

$(HOST_BUILD)/client_it.o: src/client.c $(wildcard src/*.h)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(IT_DEFS) -c -o $@ $<

$(HOST_BUILD)/test_core: test/test_core.c $(HOST_OBJ)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $< $(HOST_OBJ)

$(HOST_BUILD)/it_client: test/it_client.c $(HOST_BUILD)/client_it.o \
                         $(filter-out $(HOST_BUILD)/client.o,$(HOST_OBJ))
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $< $(HOST_BUILD)/client_it.o \
	    $(filter-out $(HOST_BUILD)/client.o,$(HOST_OBJ))

FUZZ_OBJ := $(addprefix $(HOST_BUILD)/,util.o html.o json.o http.o wpsd.o scan.o client.o rows.o)

$(HOST_BUILD)/fuzz_parsers: test/fuzz_parsers.c $(FUZZ_OBJ)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $< $(FUZZ_OBJ)

# A read-only command line check of a real hotspot (see the file's header).
PROBE_OBJ := $(addprefix $(HOST_BUILD)/,util.o html.o json.o http.o wpsd.o scan.o client.o rows.o)

probe: $(HOST_BUILD)/hs_probe

$(HOST_BUILD)/hs_probe: test/hs_probe.c $(PROBE_OBJ)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $< $(PROBE_OBJ)

# The real RISC OS front end (ro_*.c) built unchanged against the real OSLib
# headers, with a fake desktop standing in for the Wimp (test/fakewimp.c).
OSLIB_INC := $(HOME)/gccsdk/env/include
SIM_CFLAGS := $(HOST_CFLAGS) -I$(OSLIB_INC) -Itest
SIM_RO := $(addprefix $(HOST_BUILD)/sim_,$(addsuffix .o,$(RO_SRC)))

$(HOST_BUILD)/sim_ro_main.o: src/ro_main.c $(wildcard src/*.h)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CC) $(SIM_CFLAGS) -Dmain=hotspot_main -c -o $@ $<

$(HOST_BUILD)/sim_%.o: src/%.c $(wildcard src/*.h)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CC) $(SIM_CFLAGS) -c -o $@ $<

$(HOST_BUILD)/fakewimp.o: test/fakewimp.c test/fakewimp.h
	@mkdir -p $(HOST_BUILD)
	$(HOST_CC) $(SIM_CFLAGS) -c -o $@ $<

$(HOST_BUILD)/ui_sim: test/ui_sim.c $(HOST_BUILD)/fakewimp.o $(SIM_RO) \
                      $(HOST_BUILD)/client_it.o \
                      $(filter-out $(HOST_BUILD)/client.o,$(HOST_OBJ))
	$(HOST_CC) $(SIM_CFLAGS) -o $@ $< $(HOST_BUILD)/fakewimp.o $(SIM_RO) \
	    $(HOST_BUILD)/client_it.o \
	    $(filter-out $(HOST_BUILD)/client.o,$(HOST_OBJ))

clean:
	rm -rf build
