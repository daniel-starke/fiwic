COMMA = ,

FIWIC_VERSION = 1.0.0
FIWIC_VERSION_NUMS = $(subst .,$(COMMA),$(FIWIC_VERSION)),0
FIWIC_VERSION_QUAD = $(FIWIC_VERSION).0
FIWIC_VERSION_DATE = 2026-10-26
FIWIC_AUTHOR = Daniel Starke

CPPMETAFLAGS = '-DFIWIC_VERSION="$(FIWIC_VERSION) ($(FIWIC_VERSION_DATE))"' '-DFIWIC_VERSION_NUMS=$(FIWIC_VERSION_NUMS)' '-DFIWIC_VERSION_QUAD="$(FIWIC_VERSION_QUAD)"' '-DFIWIC_AUTHOR="$(FIWIC_AUTHOR)"'
CPPFLAGS += $(CPPMETAFLAGS) -MMD -MP

# The version resource is preprocessed separately and needs the debug marker.
RCFLAGS = $(CPPMETAFLAGS) $(if $(DEBUG),-DDEBUG)

# FreeImage is built from its source distribution. The official binaries are
# not used because they need Windows Vista or newer. They rely on implicit TLS,
# which Windows XP does not set up for DLLs loaded at run time.
FREEIMAGE_VERSION = 3.18.0
FREEIMAGE_ZIP = FreeImage$(subst .,,$(FREEIMAGE_VERSION)).zip
FREEIMAGE_URL = https://downloads.sourceforge.net/freeimage/$(FREEIMAGE_ZIP)
FREEIMAGE_SHA256 = f41379682f9ada94ea7b34fe86bf9ee00935a3147be41b6569c9605a53e438fd

FIZIP = bin/$(FREEIMAGE_ZIP)
FISRC = $(DSTDIR)/FreeImage
FIOBJ = $(DSTDIR)/fiobj

# Source list (`SRCS`) and include paths (`INCLUDE`) of FreeImage, relative to FISRC.
-include $(FISRC)/Makefile.srcs

FI_SRC = $(patsubst ./%,%,$(SRCS))
FI_OBJ = $(addprefix $(FIOBJ)/,$(addsuffix $(OBJEXT),$(FI_SRC)))
# The `guiddef.h` of LibJXR must not replace the one of the system headers.
FI_INCLUDE = $(patsubst -I%,-I$(FISRC)/%,$(filter-out -ISource/LibJXR/common/include,$(INCLUDE))) -idirafter $(FISRC)/Source/LibJXR/common/include
# Flags as in Makefile.mingw of FreeImage, with these changes:
# - `-O2` instead of `-O3` keeps the DLL about 0.7 MB smaller.
# - `_WIN32_WINNT` keeps LibWebP from using the condition variables of Windows Vista.
# - `__MINGW64_TOOLCHAIN__` makes LibJXR use `_byteswap_ulong` of the system headers.
# - The bundled libraries predate C++17.
# - Function local statics stay thread-safe, which OpenEXR relies on. The
#   synchronization needs src/xpcompat.cpp on Windows XP.
# - `-fpermissive` accepts the pointer to `unsigned long` casts of the OpenEXR alignment
#   checks on 64 bit, which only test the lowest bits.
# - src/fimingw.h provides what MinGW lacks.
FI_DEFINES = -DNDEBUG -DWINVER=0x0501 -D_WIN32_WINNT=0x0501 -DFREEIMAGE_EXPORTS -DOPJ_STATIC -DLIBRAW_NODLL -DDISABLE_PERF_MEASUREMENT -D__MINGW64_TOOLCHAIN__
FI_CFLAGS = $(ARCHFLAGS) -O2 -fexceptions -w $(FI_DEFINES) $(FI_INCLUDE)
FI_CXXFLAGS = $(FI_CFLAGS) -std=gnu++11 -fpermissive -include stdexcept -include $(SRCDIR)/fimingw.h
FI_LDFLAGS = $(ARCHFLAGS) $(if $(DEBUG),,-s) -shared -static -Wl,--subsystem,windows:5.0,--major-os-version,5
# Match exported names of the official binaries: `_Function@N` on 32 bit.
FI_DLLTOOLFLAGS = $(DLLTOOLFLAGS) $(if $(filter x86,$(ARCH)),--add-stdcall-underscore)

CPPFLAGS += -I$(FISRC)/Source

FIWIC_OBJ = \
	fiwic \
	fiwicdec \
	fiwicenc \
	fiwicmd \
	fiwicreg \
	fiwicutil \
	xpcompat \

FIWIC_LIB = \
	libole32 \
	liboleaut32 \
	libuuid \
	libadvapi32 \
	libshell32 \
	libshlwapi \

WICTEST_OBJ = \
	wictest \
	xpcompat \

WICTEST_LIB = \
	libole32 \
	liboleaut32 \
	libuuid \

TARGETS = \
	$(DSTDIR)/fiwic$(DLLEXT) \
	$(DSTDIR)/FreeImage$(DLLEXT) \
	$(DSTDIR)/wictest$(BINEXT)

all: $(TARGETS)

# Interrupted or failed steps must not leave outputs which look up to date.
.DELETE_ON_ERROR:

.PHONY: clean
clean:
	$(RM) -r $(DSTDIR)

$(DSTDIR)/fiwic$(DLLEXT): $(addprefix $(DSTDIR)/,$(addsuffix $(OBJEXT),$(FIWIC_OBJ))) $(DSTDIR)/version$(OBJEXT) $(SRCDIR)/fiwic.def $(FIOBJ)/FreeImage.lib
	$(LD) $(LDFLAGS) -shared -Wl,--enable-stdcall-fixup -o $@ $(addprefix $(DSTDIR)/,$(addsuffix $(OBJEXT),$(FIWIC_OBJ))) $(DSTDIR)/version$(OBJEXT) $(SRCDIR)/fiwic.def $(FIOBJ)/FreeImage.lib $(FIWIC_LIB:lib%=-l%)

$(DSTDIR)/wictest$(BINEXT): $(addprefix $(DSTDIR)/,$(addsuffix $(OBJEXT),$(WICTEST_OBJ)))
	$(LD) $(LDFLAGS) $(LDUNICODE) -o $@ $+ $(WICTEST_LIB:lib%=-l%)

.PHONY: fetch
fetch: $(FIZIP)

$(FIZIP):
	mkdir -p "$(dir $@)"
	curl -fsSL -o $@.tmp $(FREEIMAGE_URL)
	mv $@.tmp $@

$(FISRC)/Makefile.srcs: $(FIZIP)
	sha256sum $< | grep -q "^$(FREEIMAGE_SHA256) " || { echo "$<: checksum mismatch" >&2; exit 1; }
	rm -rf "$(FISRC)"
	mkdir -p "$(DSTDIR)"
	unzip -q $< -d "$(DSTDIR)"
	touch $@

$(FIOBJ)/%.c$(OBJEXT): $(FISRC)/%.c
	mkdir -p "$(dir $@)"
	$(CC) $(FI_CFLAGS) -o $@ -c $<

$(FIOBJ)/%.cpp$(OBJEXT): $(FISRC)/%.cpp $(SRCDIR)/fimingw.h
	mkdir -p "$(dir $@)"
	$(CXX) $(FI_CXXFLAGS) -o $@ -c $<

$(FIOBJ)/FreeImage.res$(OBJEXT): $(FISRC)/FreeImage.rc
	mkdir -p "$(dir $@)"
	$(WINDRES) $(WINDRESFLAGS) -DNDEBUG -o $@ $<

# Windows XP support of the static GCC runtime (see src/xpcompat.cpp).
$(FIOBJ)/xpcompat$(OBJEXT): $(SRCDIR)/xpcompat$(CXXEXT)
	mkdir -p "$(dir $@)"
	$(CXX) $(ARCHFLAGS) -O2 -std=c++11 $(CDFLAGS) -o $@ -c $<

$(FIOBJ)/FreeImage.lib: $(FIOBJ)/FreeImage.exp

$(FIOBJ)/FreeImage.exp: $(FI_OBJ)
	$(DLLTOOL) -e $@ -l $(FIOBJ)/FreeImage.lib -D FreeImage$(DLLEXT) $(FI_DLLTOOLFLAGS) $(FI_OBJ)

# The codec needs FreeImage.dll next to fiwic.dll.
$(DSTDIR)/FreeImage$(DLLEXT): $(FIOBJ)/FreeImage.exp $(FI_OBJ) $(FIOBJ)/FreeImage.res$(OBJEXT) $(FIOBJ)/xpcompat$(OBJEXT)
	$(LD) $(FI_LDFLAGS) -o $@ $^ -lws2_32

$(DSTDIR)/%$(OBJEXT): $(SRCDIR)/%$(CXXEXT)
	mkdir -p "$(dir $@)"
	$(CXX) $(CWFLAGS) $(CPPFLAGS) $(CXXFLAGS) -o $@ -c $<

$(DSTDIR)/%$(OBJEXT): $(SRCDIR)/%$(RCEXT)
	mkdir -p "$(dir $@)"
	$(CXX) -x c++ -E $(RCFLAGS) -o $@.ii $<
	$(WINDRES) $(WINDRESFLAGS) $@.ii $@

$(DSTDIR)/version$(OBJEXT): common.mk Makefile

$(addprefix $(DSTDIR)/,$(addsuffix $(OBJEXT),$(FIWIC_OBJ) $(WICTEST_OBJ))): | $(FISRC)/Makefile.srcs

# header dependencies generated by the compiler
-include $(wildcard $(DSTDIR)/*.d)
