#
# NetSurf javascript source file inclusion
#
# Included by Makefile
#

S_JAVASCRIPT_BINDING:=

ifeq ($(NETSURF_USE_QUICKJS),YES)
# QuickJS-ng (engine scaffold; development only until DOM bindings exist)
include content/handlers/javascript/quickjs/Makefile
else ifeq ($(NETSURF_USE_DUKTAPE),YES)
# Duktape
include content/handlers/javascript/duktape/Makefile
else
# None
include content/handlers/javascript/none/Makefile
endif

# Fetcher for javascript scheme is always required
S_JAVASCRIPT += fetcher.c
