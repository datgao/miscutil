CC ?= gcc
XCC = $(CROSS_COMPILE)$(CC)
CFLAGS ?= -Os -Wall -Wextra -ggdb3
CFLAGS += $(BITNESS)
LIBCFLAGS = $(CFLAGS) -fPIC
LDFLAGS += $(BITNESS)
LDSTATIC = $(LDFLAGS) -static
LIBLDFLAGS = $(LDFLAGS) -shared -Wl,--no-as-needed -ldl
MAKE ?= make
STRIP ?= strip
XSTRIP = $(CROSS_COMPILE)$(STRIP)
CP ?= cp
RM ?= rm -f
MEXE = stdansi
LEXE = dbz resparse
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

define target
$(2)$(1):
	$(3) $(2)
endef

define static
stx$(1) = $(patsubst %,%s,$(1))
$$(stx$(1)): $$(obj$(1))
	$(XCC) -o $$(stx$(1)) $$(obj$(1)) $(2)
	$(STRIP) $$(stx$(1))
endef

define targets
src$(2) = $(patsubst %,%.c,$(2))
obj$(2) = $(patsubst %,%.o,$(2))
dbg$(2) = $(patsubst %,%g$(1),$(2))
stx$(2) = $(if $(5),$(patsubst %,%s,$(2)))
OBJS += $(2)$(1)
$(2)$(1): $$(dbg$(2)) $$(stx$(2))
	$(CP) $$(dbg$(2)) $(2)$(1)
	$(STRIP) $(2)$(1)
OBJS += $$(dbg$(2))
$$(dbg$(2)): $$(obj$(2))
	$(XCC) -o $$(dbg$(2)) $$(obj$(2)) $(4)
OBJS += $$(stx$(2))
$(if $(5),$(eval $(call static,$(2),$(5))))
OBJS += $$(obj$(2))
$$(obj$(2)): $$(src$(2))
	$(XCC) -c -o $$(obj$(2)) $$(src$(2)) $(3)
endef

define metatarget
$(1): $(patsubst %,%$(1),$(2))
$(foreach targ,$(2),$(eval $(call target,$(1),$(targ),$(3))))
endef

$(eval $(call metatarget,32,$(TARG),$(MAKE) BITNESS=-m32))
$(eval $(call metatarget,cross,$(TARG),$(MAKE) CROSS_COMPILE=arm-linux-gnueabihf- CC=gcc))

OBJS =
PFXS = M L
$(foreach pfx,$(PFXS),$(foreach exe,$(value $(pfx)EXE),$(eval $(call targets,,$(exe),$(CFLAGS),$(LDFLAGS),$(LDSTATIC)))))
$(foreach lib,$(LBAS),$(eval $(call targets,$(LIBX),$(lib),$(LIBCFLAGS),$(LIBLDFLAGS),)))
