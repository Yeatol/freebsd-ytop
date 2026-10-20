PROG_CXX=	ytop
SRCS=		main.cpp tui.cpp top.cpp
MAN=

CXXSTD=		c++23
CXXFLAGS+=	-Wall -Wextra -Wpedantic -fno-exceptions
LDADD+=		-lkvm -lncursesw
DPADD+=		${LIBKVM} ${LIBNCURSESW}

PREFIX?=	/usr/local
BINDIR?=	${PREFIX}/bin

.include <bsd.prog.mk>
