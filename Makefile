CC ?= gcc
XCC = $(CROSS_COMPILE)$(CC)
OLVL ?= 3
CFLAGS ?= -O$(OLVL) -Wall -Wextra -ggdb3 $(COPT)
CFLAGS += $(BITNESS)
LIBCFLAGS = $(CFLAGS) -fPIC
LDFLAGS += $(BITNESS) -lrt
LDSTATIC = $(LDFLAGS) -static
LIBLDFLAGS = $(LDFLAGS) -shared -Wl,--no-as-needed -ldl
MAKE ?= make
STRIP ?= strip
XSTRIP = $(CROSS_COMPILE)$(STRIP)
RM ?= rm -f
MEXE = kira stdansi
LEXE = asm dbz fat32 resparse
LLIB = madvmerge nocache
LIBX = .so
LBAS = $(patsubst %,lib%,$(LLIB))
LLSO = $(patsubst %,%.so,$(LBAS))
TARG = misc linux

all: $(TARG)

misc: $(MEXE)

linux: $(LEXE) $(LLSO)

clean:
	$(RM) $(OBJS)

define static
stx$(1) = $(patsubst %,%s,$(1))
$$(stx$(1)): $$(obj$(1))
	$(XCC) -o $$(stx$(1)) $$(obj$(1)) $(2)
	$(XSTRIP) $$(stx$(1))
endef

define targets
src$(1) = $(patsubst %,%.c,$(1))
obj$(1) = $(patsubst %,%.o,$(1))
dbg$(1) = $(patsubst %,%g$(2),$(1))
stx$(1) = $(if $(5),$(patsubst %,%s,$(1)))
OBJS += $(1)$(2)
$(1)$(2): $$(dbg$(1)) # $$(stx$(1))
	$(XSTRIP) -sxo $(1)$(2) $$(dbg$(1))
OBJS += $$(dbg$(1))
$$(dbg$(1)): $$(obj$(1))
	$(XCC) -o $$(dbg$(1)) $$(obj$(1)) $(4)
# OBJS += $$(stx$(1))
$(if $(5),$(eval $(call static,$(1),$(5))))
OBJS += $$(obj$(1))
$$(obj$(1)): $$(src$(1))
	$(XCC) -c -o $$(obj$(1)) $$(src$(1)) $(3)
endef

define subtarget
$(1)$(2):
	$(3) $(1)
endef

define metatarget
$(1): $(patsubst %,%$(1),$(2))
$(foreach targ,$(2),$(eval $(call subtarget,$(targ),$(1),$(3))))
endef

$(eval $(call metatarget,32,$(TARG),$(MAKE) BITNESS=-m32))
$(eval $(call metatarget,arm,$(TARG),$(MAKE) CROSS_COMPILE=arm-linux-gnueabihf- CC=gcc))

OBJS =
EXES = $(MEXE) $(LEXE)
$(foreach exe,$(EXES),$(eval $(call targets,$(exe),,$(CFLAGS),$(LDFLAGS),$(LDSTATIC))))
$(foreach lib,$(LBAS),$(eval $(call targets,$(lib),$(LIBX),$(LIBCFLAGS),$(LIBLDFLAGS),)))
