#
# NetSurf javascript source file inclusion
#
# Included by javascript/Makefile
#

# QuickJS-ng backend (engine glue + hand-written core DOM bindings)

S_JAVASCRIPT += quickjs/quickjs.c quickjs/qjs_dom.c

# the engine library comes from the port's local prefix (libquickjs.a);
# repeat -lm after it since this lands late in the link line
LDFLAGS += -lquickjs -lm
