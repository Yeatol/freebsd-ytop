PROG_CXX=	ytop
SRCS=		main.cpp tui.cpp top.cpp
MAN=

CXXSTD=		c++23
LDADD+=		-lkvm -lncursesw
DPADD+=		${LIBKVM} ${LIBNCURSESW}

PREFIX?=	/usr/local
BINDIR?=	${PREFIX}/bin

.include <bsd.prog.mk>
