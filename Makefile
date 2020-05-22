ifndef CFLAGS
CFLAGS		= -O2 -Wall
endif

INCLUDE		+= -I.
LDFLAGS         += -lpthread -lnuma -lm

is_ppc		:= $(shell (uname -m || uname -p) | grep ppc)
is_x86		:= $(shell (uname -m || uname -p) | grep i.86)
is_x86_64	:= $(shell (uname -m || uname -p) | grep x86_64)

ifneq ($(is_x86),)
# Need to tell gcc we have a reasonably recent cpu to get the atomics.
CFLAGS += -march=i686
endif

all: oslat

oslat: main.o rt-utils.o error.o trace.o
	$(CC) -o $@ $(LDFLAGS) $^

clean:
	rm -f *.o oslat cscope.*

cscope:
	cscope -bq *.c
