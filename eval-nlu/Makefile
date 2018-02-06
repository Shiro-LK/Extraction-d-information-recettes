PROG = ne-scoring-gen

OBJS = ne-scoring-gen.o

SRCS = ne-scoring-gen.cc

HDRS =

OPT=-O9

CXX=g++
CXXFLAGS=-Wall -g ${OPT} ##-I/usr/include/lua5.1
##LIBS= -g ${OPT} -llua5.1
LIBS= -g ${OPT} -L/usr/local/include -llua5.2

${PROG} : ${OBJS}
	${CXX} -o $@ ${OBJS} ${LIBS}

clean:
	rm -f ${OBJS} ${PROG}
###
