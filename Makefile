# Builds fiwic.dll and wictest.exe for 64 bit and 32 bit Windows.
#
#   make                   both architectures into bin/x64 and bin/x86
#   make ARCH=x64          one architecture
#   make DEBUG=1           debug build
#
# A multilib GCC (e.g. MSYS2 mingw-w64 distributions with -m32 support) works
# out of the box. With separate cross compilers, name their prefixes:
#
#   make PREFIX64=x86_64-w64-mingw32- PREFIX32=i686-w64-mingw32-
PREFIX64 =
PREFIX32 =

ifeq (,$(ARCH))

.PHONY: all clean x64 x86
all: x64 x86

# The architectures are built one after the other. Each sub-make still builds
# in parallel. Concurrent recursive makes sharing one job limit can deadlock
# sporadically (seen with GNU Make 3.81 on Windows). The sequence also lets
# the first one download the FreeImage sources for both.
.NOTPARALLEL:

x64 x86:
	+$(MAKE) ARCH=$@

clean:
	rm -rf bin

else

ifeq ($(ARCH),x64)
 PREFIX = $(PREFIX64)
 ARCHFLAGS = -m64
 WINDRESFLAGS = -F pe-x86-64
 DLLTOOLFLAGS = -m i386:x86-64 --as-flags=--64
else ifeq ($(ARCH),x86)
 PREFIX = $(PREFIX32)
 ARCHFLAGS = -m32
 WINDRESFLAGS = -F pe-i386
 DLLTOOLFLAGS = -m i386 --as-flags=--32
else
 $(error ARCH must be x64 or x86)
endif

export CC = $(PREFIX)gcc
export CXX = $(PREFIX)g++
export LD = $(PREFIX)g++
export WINDRES = $(PREFIX)windres
export DLLTOOL = $(PREFIX)dlltool
export RM = rm -f
export CP = cp -p

CXXEXT = .cpp
OBJEXT = .o
RCEXT  = .rc
BINEXT = .exe
DLLEXT = .dll
SRCDIR = src
DSTDIR = bin/$(ARCH)

ENABLE_LTO = 1

CWFLAGS = -Wall -Wextra -Wformat -pedantic -Wshadow -Wconversion -Wparentheses -Wunused -Wno-missing-field-initializers
CDFLAGS = -DNOMINMAX -D_USE_MATH_DEFINES -DWIN32 -D_WIN32_WINNT=0x0601 -DWIN32_LEAN_AND_MEAN -DUNICODE -D_UNICODE
CPPFLAGS = -I$(SRCDIR)
ifeq (,$(DEBUG))
 ifeq (,$(ENABLE_LTO))
  BASE_CFLAGS = $(ARCHFLAGS) -O2 -mstackrealign -fno-ident -ffunction-sections -fdata-sections -fomit-frame-pointer -DNDEBUG $(CDFLAGS)
  LDFLAGS = $(ARCHFLAGS) -O2 -s -static -Wl,--gc-sections
 else
  BASE_CFLAGS = $(ARCHFLAGS) -O2 -mstackrealign -fno-ident -ffunction-sections -fdata-sections -flto -flto-partition=none -fomit-frame-pointer -DNDEBUG $(CDFLAGS)
  LDFLAGS = $(ARCHFLAGS) -O2 -s -static -flto -flto-partition=none -fuse-linker-plugin -Wl,--gc-sections
 endif
else
 BASE_CFLAGS = $(ARCHFLAGS) -Wa,-mbig-obj -Og -g3 -ggdb -gdwarf-3 -mstackrealign -fno-omit-frame-pointer $(CDFLAGS)
 LDFLAGS = $(ARCHFLAGS) -static -fno-omit-frame-pointer
endif
# Export undecorated names on 32 bit as well (see src/fiwic.def).
LDFLAGS += -Wl,--kill-at
LDUNICODE = -municode
export CXXFLAGS = -Wcast-qual -Wnon-virtual-dtor -Wold-style-cast -Wno-unused-parameter -Wno-long-long -Wno-maybe-uninitialized -std=c++11 $(BASE_CFLAGS)

include common.mk

endif
