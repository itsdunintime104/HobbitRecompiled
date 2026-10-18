# Compile and link the generated C into build.nosync/hobbit.
# Run as: make -C build -j$(sysctl -n hw.ncpu)
ROOT := $(abspath ..)
GEN := $(ROOT)/generated.nosync
OUT := $(ROOT)/build.nosync
OBJ := $(OUT)/obj

CC := clang
# TSO=1 (default): lifted guest loads/stores keep x86 ordering (ldapr/stlr).
# TSO=0 is plain loads/stores, for bisecting or timing. Make does not track
# CFLAGS: delete build.nosync/obj/*.o after changing it.
TSO ?= 1
# OPT: clang optimisation level for the game code and the runtime (the overlay shows it).
OPT ?= 2
CFLAGS := -std=gnu11 -DRECOMP_TSO=$(TSO) -O$(OPT) -DRECOMP_OPT=$(OPT) -fwrapv -fno-strict-aliasing -ffp-contract=off -arch arm64 -I $(ROOT)/tools/xrecomp/include -I $(GEN) -I $(ROOT)/runtime/include -w -DRECOMP_GLUE_NO_MAIN \
          -I /opt/homebrew/opt/sdl2/include/SDL2 -D_THREAD_SAFE

LDFLAGS_FW := -framework OpenGL -framework CoreFoundation -framework ApplicationServices \
              -framework Carbon -framework IOKit -framework AudioToolbox -framework CoreAudio \
              -L /opt/homebrew/opt/sdl2/lib -lSDL2

GEN_SRCS := $(wildcard $(GEN)/*.c)
# Every runtime source. Native replacements of lifted functions are listed in
# recon/native_overrides.txt; the lifter declares those weak.
RT_SRCS := $(wildcard $(ROOT)/runtime/src/*.c)
STUB_SRC := $(OUT)/import_stubs.c

# sample.c repeats 200 functions that the chunk files define (a --sample
# leftover). It is compiled as a check but not linked, or ld sees duplicates.
SAMPLE_OBJ := $(OBJ)/gen_sample.o
OBJS := $(filter-out $(SAMPLE_OBJ),$(patsubst $(GEN)/%.c,$(OBJ)/gen_%.o,$(GEN_SRCS))) \
        $(patsubst $(ROOT)/runtime/src/%.c,$(OBJ)/rt_%.o,$(RT_SRCS)) \
        $(OBJ)/import_stubs.o

HEADERS := $(wildcard $(GEN)/*.h) $(wildcard $(ROOT)/runtime/include/*.h) $(ROOT)/tools/xrecomp/include/recomp_types.h

all: $(OUT)/hobbit $(OUT)/cghelper $(if $(wildcard $(GEN)/sample.c),$(SAMPLE_OBJ))

# x86_64 helper that runs the game's bundled Cg runtime under Rosetta
# (runtime/src/thunk_cg.c starts it from next to the executable).
$(OUT)/cghelper: $(ROOT)/tools/cghelper/cghelper.c $(ROOT)/tools/cghelper/cg_wire.h | $(OBJ)
	$(CC) -std=gnu11 -O1 -arch x86_64 -o $@ $<

$(OUT)/hobbit: $(OBJS)
	$(CC) -arch arm64 -o $@ $^ $(LDFLAGS_FW)

$(OBJ):
	mkdir -p $(OBJ)

$(OBJ)/gen_%.o: $(GEN)/%.c $(HEADERS) | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

# The SDL2 front end replaces 19 lifted functions in this chunk; the header
# renames their lifted definitions so callers bind to frontend_sdl.c.
$(OBJ)/gen_hobbit_0025.o: CFLAGS += -include $(ROOT)/runtime/include/frontend_rename.h

# HEAP_DEBUG=1 adds dlmalloc's per-call heap checks; a failed check prints its
# expression and line. Delete $(OBJ)/rt_dlmalloc.o when you switch it.
HEAP_DEBUG ?= 0
ifeq ($(HEAP_DEBUG),1)
$(OBJ)/rt_dlmalloc.o: CFLAGS += -DDEBUG=1 -DABORT_ON_ASSERT_FAILURE=0
endif

$(OBJ)/rt_%.o: $(ROOT)/runtime/src/%.c $(HEADERS) | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(STUB_SRC): $(GEN)/imports.h $(ROOT)/tools/hobbit/emit_import_stubs.py | $(OBJ)
	python3 -I $(ROOT)/tools/hobbit/emit_import_stubs.py $(GEN)/imports.h $@

$(OBJ)/import_stubs.o: $(STUB_SRC) $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# Double-clickable launcher bundle in "build.nosync/Hobbit Recompiled.app".
app: $(OUT)/hobbit $(OUT)/cghelper
	$(ROOT)/tools/package/make_app.sh

.PHONY: all app
