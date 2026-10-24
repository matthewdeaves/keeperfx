# Native Apple Silicon (arm64) macOS build for KeeperFX.
# Usage: make -f macos.mk            (uses clang, Homebrew libraries)
#
# Requires Homebrew packages:
#   pkg-config sdl3 sdl3_image sdl3_mixer ffmpeg luajit
#   openal-soft libspng minizip miniupnpc libnatpmp zlib curl
# The source-only deps (astronomy, centijson, enet6) are built into deps/
# as arm64 static libraries by tools/build_macos_deps.sh.

include version.mk

BUILD_NUMBER ?= $(VER_BUILD)
VER_SUFFIX ?= Prototype
VER_STRING = $(VER_MAJOR).$(VER_MINOR).$(VER_RELEASE).$(BUILD_NUMBER) $(VER_SUFFIX)

MKDIR ?= mkdir -p
STRIP ?= strip
ECHO ?= echo
MV ?= mv -f

CC  ?= clang
CXX ?= clang++

BREW ?= $(shell brew --prefix)
# openal-soft, curl and zlib are keg-only in Homebrew; their pkgconfig dirs are
# embedded into the pkg-config invocation so every $(shell) call below sees them.
KFX_PKG_CONFIG_PATH := $(BREW)/opt/openal-soft/lib/pkgconfig:$(BREW)/opt/curl/lib/pkgconfig:$(BREW)/opt/zlib/lib/pkgconfig
PKG_CONFIG ?= PKG_CONFIG_PATH="$(KFX_PKG_CONFIG_PATH):$$PKG_CONFIG_PATH" $(BREW)/bin/pkg-config

# Mirrors build/cmake/modules/BuildTargets.cmake: every .c/.cpp under src/, minus
# the functional-test harness and the Windows-only platform files. Globbing keeps
# this in step with upstream adding, renaming or removing files.
KFX_SOURCES = $(sort $(shell find src \( -name '*.c' -o -name '*.cpp' \) \
	-not -path 'src/ftests/*' \
	-not -name PlatformWindows.cpp -not -name WindowCompositorWin.cpp))

KFX_C_SOURCES = $(filter %.c,$(KFX_SOURCES))
KFX_CXX_SOURCES = $(filter %.cpp,$(KFX_SOURCES))
KFX_C_OBJECTS = $(patsubst src/%.c,obj/%.o,$(KFX_C_SOURCES))
KFX_CXX_OBJECTS = $(patsubst src/%.cpp,obj/%.o,$(KFX_CXX_SOURCES))
KFX_OBJECTS = $(KFX_C_OBJECTS) $(KFX_CXX_OBJECTS)

KFX_INCLUDES = \
	-Isrc \
	-Ideps/centijson/include \
	-Ideps/centitoml \
	-Ideps/astronomy/include \
	-Ideps/enet6/include \
	-Ideps/glad/include \
	-I$(BREW)/include \
	$(shell $(PKG_CONFIG) --cflags-only-I luajit) \
	$(shell $(PKG_CONFIG) --cflags-only-I libavformat) \
	$(shell $(PKG_CONFIG) --cflags-only-I openal) \
	$(shell $(PKG_CONFIG) --cflags-only-I sdl3) \
	$(shell $(PKG_CONFIG) --cflags-only-I sdl3-image) \
	$(shell $(PKG_CONFIG) --cflags-only-I sdl3-mixer)

ARCHFLAGS = -arch arm64

WARNFLAGS = -Wall -Wextra -Wno-unused-parameter -Wno-unknown-pragmas -Wno-sign-compare \
	-Wno-deprecated-declarations -Wno-format-truncation -Wno-nullability-completeness

# macOS's <ucontext.h> requires _XOPEN_SOURCE; _DARWIN_C_SOURCE re-exposes the
# full BSD/Darwin namespace (dladdr, SIGWINCH/SIGIO, etc.) that _XOPEN_SOURCE
# would otherwise hide and which the POSIX code paths rely on.
DARWINFLAGS = -D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE

# Emit a .d sidecar per object listing every header it pulled in, so touching a
# shared header (e.g. a struct-layout change) recompiles every dependent unit.
# Without this, incremental builds across header changes silently link mismatched
# struct layouts together -- which is exactly how a stale build crashes at runtime.
# -MP adds phony targets for headers so a later header rename doesn't wedge the build.
DEPFLAGS = -MMD -MP

KFX_CFLAGS += -DBFDEBUG_LEVEL=0 -O3 $(ARCHFLAGS) -std=gnu11 $(DARWINFLAGS) $(KFX_INCLUDES) $(WARNFLAGS) $(DEPFLAGS)
KFX_CXXFLAGS += -DBFDEBUG_LEVEL=0 -O3 $(ARCHFLAGS) -std=gnu++20 $(DARWINFLAGS) $(KFX_INCLUDES) $(WARNFLAGS) $(DEPFLAGS)

# No -ld_classic needed: the in-memory pack(1) tables that embed pointers are
# unpacked in the shared headers, so the modern linker (ld-prime) emits them fine.
KFX_LDFLAGS += \
	$(ARCHFLAGS) \
	-Wl,-export_dynamic \
	-L$(BREW)/lib \
	-Ldeps/astronomy -lastronomy \
	-Ldeps/centijson -ljson \
	-Ldeps/enet6 -lenet6 \
	$(shell $(PKG_CONFIG) --libs sdl3) \
	$(shell $(PKG_CONFIG) --libs sdl3-mixer) \
	$(shell $(PKG_CONFIG) --libs sdl3-image) \
	$(shell $(PKG_CONFIG) --libs libavformat) \
	$(shell $(PKG_CONFIG) --libs libavcodec) \
	$(shell $(PKG_CONFIG) --libs libswresample) \
	$(shell $(PKG_CONFIG) --libs libavutil) \
	$(shell $(PKG_CONFIG) --libs openal) \
	$(shell $(PKG_CONFIG) --libs luajit) \
	$(shell $(PKG_CONFIG) --libs spng) \
	$(shell $(PKG_CONFIG) --libs minizip) \
	$(shell $(PKG_CONFIG) --libs zlib) \
	$(shell $(PKG_CONFIG) --libs libcurl) \
	-lminiupnpc \
	-lnatpmp \
	-liconv \
	-framework CoreFoundation

# Vendored GL loader for the OpenGL renderer; resolved at runtime through
# SDL_GL_GetProcAddress, so nothing links against OpenGL.framework.
GLAD_OBJECTS = obj/glad/glad.o

# Window icon bytes for WindowSystemSDL, generated like upstream's CMake does on
# every non-Windows target.
ICON_PNG = res/keeperfx_icon256-24bpp.png
ICON_OBJECTS = obj/generated/window_icon.o

TOML_SOURCES = \
	deps/centitoml/toml_api.c

TOML_OBJECTS = $(patsubst deps/centitoml/%.c,obj/centitoml/%.o,$(TOML_SOURCES))

TOML_INCLUDES = \
	-Ideps/centijson/include

TOML_CFLAGS += -O3 $(ARCHFLAGS) -std=gnu11 $(TOML_INCLUDES) -Wall -Wextra -Wno-unused-parameter $(DEPFLAGS)

all: bin/keeperfx

clean:
	rm -rf obj bin src/ver_defs.h

.PHONY: all clean

bin/keeperfx: $(KFX_OBJECTS) $(TOML_OBJECTS) $(GLAD_OBJECTS) $(ICON_OBJECTS) | bin
	$(CXX) -o $@ $(KFX_OBJECTS) $(TOML_OBJECTS) $(GLAD_OBJECTS) $(ICON_OBJECTS) $(KFX_LDFLAGS)
	$(STRIP) -x $@

$(KFX_C_OBJECTS): obj/%.o: src/%.c src/ver_defs.h | obj
	$(MKDIR) $(dir $@)
	$(CC) $(KFX_CFLAGS) -c $< -o $@

$(KFX_CXX_OBJECTS): obj/%.o: src/%.cpp src/ver_defs.h | obj
	$(MKDIR) $(dir $@)
	$(CXX) $(KFX_CXXFLAGS) -c $< -o $@

$(TOML_OBJECTS): obj/centitoml/%.o: deps/centitoml/%.c | obj/centitoml
	$(CC) $(TOML_CFLAGS) -c $< -o $@

$(GLAD_OBJECTS): obj/glad/%.o: deps/glad/src/%.c
	$(MKDIR) $(dir $@)
	$(CC) -O2 $(ARCHFLAGS) -Ideps/glad/include -c $< -o $@

obj/generated/window_icon.c: $(ICON_PNG)
	$(MKDIR) $(dir $@)
	{ printf 'const unsigned char kfx_window_icon_png[] = {\n'; \
	  od -An -v -tx1 $< | sed -e 's/ *\([0-9a-f][0-9a-f]\)/0x\1,/g'; \
	  printf '};\nconst unsigned int kfx_window_icon_png_size = sizeof(kfx_window_icon_png);\n'; } > $@

$(ICON_OBJECTS): obj/generated/window_icon.c
	$(CC) -O2 $(ARCHFLAGS) -c $< -o $@

# Pull in the auto-generated header-dependency sidecars (.d) from -MMD. Absent on
# the first build (the pattern rules still fire); present on every rebuild after.
-include $(KFX_OBJECTS:.o=.d) $(TOML_OBJECTS:.o=.d)

bin obj obj/centitoml:
	$(MKDIR) $@

src/ver_defs.h: version.mk
	$(ECHO) "#define VER_MAJOR   $(VER_MAJOR)" > $@.swp
	$(ECHO) "#define VER_MINOR   $(VER_MINOR)" >> $@.swp
	$(ECHO) "#define VER_RELEASE $(VER_RELEASE)" >> $@.swp
	$(ECHO) "#define VER_BUILD   $(BUILD_NUMBER)" >> $@.swp
	$(ECHO) "#define VER_STRING  \"$(VER_STRING)\"" >> $@.swp
	$(ECHO) "#define PACKAGE_SUFFIX  \"$(VER_SUFFIX)\"" >> $@.swp
	$(ECHO) "#define GIT_REVISION  \"$(shell git describe  --always)\"" >> $@.swp
	$(MV) $@.swp $@
