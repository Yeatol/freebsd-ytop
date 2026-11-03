PROG_CXX=	ytop
SRCS=		main.cpp tui.cpp top.cpp terminal.cpp
MAN=

CXXSTD=		c++23
LDADD+=		-lkvm
DPADD+=		${LIBKVM}

PREFIX?=	/usr/local
BINDIR?=	${PREFIX}/bin

.include <bsd.prog.mk>
