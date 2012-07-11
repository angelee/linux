Q  = @
CC = gcc

WARNS	= -Wformat=2 -Wextra -Wmissing-noreturn \
	  -Wwrite-strings -Wno-unused-parameter -Wmissing-format-attribute \
	  -Wswitch-default -fno-builtin \
	  -Wmissing-prototypes -Wmissing-declarations -Wshadow
CFLAGS	= -Wall $(WARNS) -Werror -O3 -march=opteron -D_GNU_SOURCE
LDFLAGS = -lpthread

HDRS	= systlmm.h
BINS	= prot micro pmap

all: $(BINS)

%: %.c

%: %.c
	@echo "CC       $@"
	$(Q)$(CC) -o $@ $< $(CFLAGS) $(LDFLAGS)

clean:
	@echo "CLEAN"
	$(Q)rm -rf $(BINS)

ignore:
	echo $(BINS) > .gitignore
