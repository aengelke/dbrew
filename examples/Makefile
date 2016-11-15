EXAMPLES = stencil matrix strcmp simple vector
CPPFLAGS=-I../include
#LDLIBS=-L.. -ldbrew # with libs, dependencies do not work

# optimization flags should be the same as for DBrew snippets.
# thus, the Makefile from the top directory overrides OPTS for this.
# this is to avoid e.g. SSE/AVX transition penalties
OPTS=-O2 -mavx
CFLAGS=-std=gnu99 $(OPTS)

all: $(EXAMPLES)

stencil: stencil.o ../libdbrew.a

matrix: matrix.o ../libdbrew.a

strcmp: strcmp.o ../libdbrew.a

simple: simple.o ../libdbrew.a

vector: vector.o ../libdbrew.a

clean:
	rm -f *.o *~ $(EXAMPLES)
