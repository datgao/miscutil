CC ?= gcc
XCC = $(CROSS_COMPILE)$(CC)
CFLAGS ?= -Os -Wall -Wextra -ggdb3
LDFLAGS += 
LDXTRA += 
LDSLNK = $(LDFLAGS) -static
LDXSLN = $(LDXTRA) -static
MAKE ?= make
STRIP ?= strip
CP ?= cp
RM ?= rm -f
EXES = stdansi
SRCS = $(patsubst %,%.c,$(EXES))
OBJS = $(patsubst %,%.o,$(EXES))
DBGX = $(patsubst %,%dbg,$(EXES))
SLNK = $(patsubst %,%static,$(EXES))

all: $(EXES)

static: $(SLNK)

cross:
	$(MAKE) CROSS_COMPILE=arm-linux-gnueabihf- CC=gcc static

$(EXES): $(DBGX)
	$(CP) $< $@
	$(STRIP) $@

$(DBGX): $(OBJS)
	$(XCC) -o $@ $^ $(LDFLAGS)

$(SLNK): $(OBJS)
	$(XCC) -o $@ $^ $(LDSLNK)
	$(STRIP) $@

$(OBJS): $(SRCS)
	$(XCC) -c -o $@ $< $(CFLAGS)

clean:
	$(RM) $(OBJS) $(EXES) $(DBGX) $(SLNK)
