#
# NetSurf javascript source file inclusion
#
# Included by javascript/Makefile
#

# QuickJS-ng backend: engine glue, core DOM bindings, native primitives
# and the JavaScript web platform runtime (runtime.js, embedded)

content/handlers/javascript/quickjs/qjs_dom.c: \
	$(OBJROOT)/quickjs/runtime.js.inc

$(OBJROOT)/quickjs/runtime.js.inc: content/handlers/javascript/quickjs/runtime.js $(TOOLROOT)/xxd
	$(Q)$(MKDIR) -p $(OBJROOT)/quickjs
	$(VQ)echo "     XXD: $<"
	$(Q)$(TOOLROOT)/xxd -i $< $@.tmp
	$(Q)sed -e 's/content_handlers_javascript_quickjs_runtime_js/runtime_js/' $@.tmp > $@

S_JAVASCRIPT += content.c quickjs/quickjs.c quickjs/qjs_dom.c quickjs/qjs_native.c \
	quickjs/qjs_module.c quickjs/qjs_canvas.c

# the engine library comes from the port's local prefix (libquickjs.a);
# repeat -lm after it since this lands late in the link line
LDFLAGS += -lquickjs -lm
