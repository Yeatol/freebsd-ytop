PROG_CXX=	ytop
SRCS=		main.cpp tui.cpp top.cpp term.cpp
MAN=

CXXSTD=		c++23
LDADD+=		-lprocstat
DPADD+=		${LIBPROCSTAT}

PREFIX?=	/usr/local
BINDIR?=	${PREFIX}/bin

.include <bsd.prog.mk>
