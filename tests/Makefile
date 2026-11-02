PROGS_CXX=	terminal_test core_test
SRCS.terminal_test=	terminal_test.cpp terminal.cpp
SRCS.core_test=	core_test.cpp terminal.cpp
MAN=

.PATH: ${.CURDIR}/..
CXXFLAGS+=	-I${.CURDIR}/..
CXXSTD=		c++23
LDADD.terminal_test=	-lutil
DPADD.terminal_test=	${LIBUTIL}
LDADD.core_test=	-lkvm -lpthread
DPADD.core_test=	${LIBKVM} ${LIBPTHREAD}

.include <bsd.progs.mk>
