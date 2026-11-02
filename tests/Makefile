PROG_CXX=	terminal_test
SRCS=		terminal_test.cpp terminal.cpp
MAN=

.PATH: ${.CURDIR}/..
CXXFLAGS+=	-I${.CURDIR}/..
CXXSTD=		c++23
LDADD+=		-lutil
DPADD+=		${LIBUTIL}

.include <bsd.prog.mk>
