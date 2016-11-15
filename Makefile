WFLAGS_BASE=-Wall -Wextra -Wmissing-field-initializers -Wunused-parameter \
            -Wold-style-definition -Wmissing-declarations -Wmissing-prototypes \
            -Wredundant-decls -Wmissing-noreturn -Wshadow -Wpointer-arith \
            -Wwrite-strings -Winline -Wformat-nonliteral -Wformat-security \
            -Wswitch-default -Winit-self -Wnested-externs -Wstrict-prototypes \
            -Wmissing-include-dirs -Wundef -Wmissing-format-attribute
ifeq ($(CI),1)
WFLAGS=-Werror $(WFLAGS_BASE)
else
WFLAGS=$(WFLAGS_BASE)
endif

WFLAGS2=-Wswitch-enum -Wswitch -Waggregate-return

CFLAGS=-g -std=gnu99 -fPIE -Iinclude -Iinclude/priv $(WFLAGS)
LDFLAGS=-g -pie

# always compile examples and DBrew snippets with optimizations
# this allows other DBrew code to still be debuggable
OPTFLAGS=-O2 -mavx

## flags dependent on compiler
CCNAME:=$(strip $(shell $(CC) --version | head -c 3))
ifeq ($(CCNAME),$(filter $(CCNAME),gcc cc icc))
 # gcc/cc
 $(info ** gcc compatible compiler detected: $(CC))
 # some snippets 'switch' to AVX mode. hack to avoid 32-byte stack alignment
 # FIXME: rewriter should move such stack alignment to outermost level
 # clang does not know these options
 SNIPPETSFLAGS=$(OPTFLAGS) -mno-vzeroupper -mpreferred-stack-boundary=5

else ifeq ($(shell $(CC) --version | head -c 5),clang)
 # clang
 $(info ** clang detected: $(CC))
 SNIPPETSFLAGS=$(OPTFLAGS)
else
 $(error Compiler $(CC) not supported)
endif

SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:.c=.o)
HEADERS = $(wildcard include/*.h)

SUBDIRS=tests examples
.PHONY: $(SUBDIRS)

all: libdbrew.a $(SUBDIRS)

libdbrew.a: $(OBJS)
	ar rcs libdbrew.a $(OBJS)

test: libdbrew.a
	$(MAKE) test -C tests CC=$(CC)

src/snippets.o: src/snippets.c
	$(CC) $(CFLAGS) $(SNIPPETSFLAGS) -c $< -o $@

examples: libdbrew.a
	cd examples && $(MAKE) OPTS='$(OPTFLAGS)' CC=$(CC)

clean:
	rm -rf *~ *.o $(OBJS) libdbrew.a
	$(MAKE) clean -C tests
	cd examples && make clean
