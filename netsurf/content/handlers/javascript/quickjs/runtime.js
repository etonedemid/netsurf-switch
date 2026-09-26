/*
 * NetSurf QuickJS web runtime.
 *
 * Browser APIs implemented in JavaScript on top of the C DOM bindings
 * (qjs_dom.c) and the native primitives object __ns (qjs_native.c).
 * Evaluated once per page context, after the core bindings are set up.
 */
(function (global) {
'use strict';

var ns = global.__ns;
var NodeP = global.__ns_Node_proto;
var ElementP = global.__ns_Element_proto;
var DocP = global.__ns_Document_proto;
var TextP = global.__ns_Text_proto;
var EventP = global.__ns_Event_proto;
var MediaP = global.__ns_Media_proto;

function def(obj, name, desc) {
	desc.configurable = true;
	if (!('get' in desc) && !('set' in desc) && !('writable' in desc))
		desc.writable = true;
	try { Object.defineProperty(obj, name, desc); } catch (e) {}
}
function method(obj, name, fn) { def(obj, name, { value: fn, enumerable: false }); }
function accessor(obj, name, get, set) {
	def(obj, name, { get: get, set: set || function () {}, enumerable: true });
}
function hasOwn(o, k) { return Object.prototype.hasOwnProperty.call(o, k); }
function isElement(n) { return n != null && n.nodeType === 1; }

/* dynamic import(): module sources rewrite `import(` to this so the
 * (possibly computed) specifier's graph is fetched before importing */
var modulePrefetch = ns.modulePrefetch;
Object.defineProperty(global, '__ns_import', {
	value: function (base, spec) {
		var s;
		try { s = String(spec); } catch (e) { return Promise.reject(e); }
		return modulePrefetch(base, s).then(function (url) {
			return import(url);
		});
	},
	configurable: false, enumerable: false, writable: false
});
function toStr(v) { return v === undefined || v === null ? '' : String(v); }

/* ------------------------------------------------------------------ */
/* Node constants and tree helpers */

var NODE_CONSTS = {
	ELEMENT_NODE: 1, ATTRIBUTE_NODE: 2, TEXT_NODE: 3,
	CDATA_SECTION_NODE: 4, PROCESSING_INSTRUCTION_NODE: 7,
	COMMENT_NODE: 8, DOCUMENT_NODE: 9, DOCUMENT_TYPE_NODE: 10,
	DOCUMENT_FRAGMENT_NODE: 11,
	DOCUMENT_POSITION_DISCONNECTED: 1, DOCUMENT_POSITION_PRECEDING: 2,
	DOCUMENT_POSITION_FOLLOWING: 4, DOCUMENT_POSITION_CONTAINS: 8,
	DOCUMENT_POSITION_CONTAINED_BY: 16
};
Object.keys(NODE_CONSTS).forEach(function (k) {
	def(NodeP, k, { value: NODE_CONSTS[k], writable: false });
	if (global.Node) def(global.Node, k, { value: NODE_CONSTS[k], writable: false });
});

function elementChildren(node) {
	var out = [];
	for (var c = node.firstChild; c; c = c.nextSibling)
		if (c.nodeType === 1) out.push(c);
	return out;
}

/* all descendant elements in document order */
function descendants(root) {
	if (ns.elements) return ns.elements(root);
	var out = [];
	(function walk(n) {
		for (var c = n.firstChild; c; c = c.nextSibling) {
			if (c.nodeType === 1) { out.push(c); walk(c); }
		}
	})(root);
	return out;
}

function toNode(v, doc) {
	if (v !== null && typeof v === 'object' && typeof v.nodeType === 'number')
		return v;
	return (doc || document).createTextNode(toStr(v));
}

function nodesToFragment(args, doc) {
	if (args.length === 1) return toNode(args[0], doc);
	var frag = (doc || document).createDocumentFragment();
	for (var i = 0; i < args.length; i++) frag.appendChild(toNode(args[i], doc));
	return frag;
}

/* ------------------------------------------------------------------ */
/* Selector engine */

var selectorCache = Object.create(null);
var selectorCacheSize = 0;

function SelectorError(msg) {
	var e = new Error(msg);
	e.name = 'SyntaxError';
	return e;
}

function parseSelectorList(text) {
	var cached = selectorCache[text];
	if (cached) return cached;
	var p = new SelParser(text);
	var list = p.list();
	p.ws();
	if (p.i < p.s.length) throw SelectorError("'" + text + "' is not a valid selector");
	if (selectorCacheSize++ > 500) { selectorCache = Object.create(null); selectorCacheSize = 0; }
	selectorCache[text] = list;
	return list;
}

function SelParser(s) { this.s = s; this.i = 0; }
SelParser.prototype.ws = function () {
	while (this.i < this.s.length && /\s/.test(this.s[this.i])) this.i++;
};
SelParser.prototype.ident = function () {
	var m = /^(?:\\.|[\w\u00a0-\uffff-])+/.exec(this.s.slice(this.i));
	if (!m) return null;
	this.i += m[0].length;
	return m[0].replace(/\\(.)/g, '$1');
};
SelParser.prototype.list = function () {
	var list = [];
	do {
		this.ws();
		list.push(this.complex());
		this.ws();
	} while (this.s[this.i] === ',' && ++this.i);
	return list;
};
SelParser.prototype.complex = function () {
	/* returns array of {comb, compound}, rightmost last */
	var parts = [];
	var comb = ' ';
	this.ws();
	if ('>+~'.indexOf(this.s[this.i]) >= 0) {
		/* relative selector (used by :has) */
		comb = this.s[this.i++];
		parts.relative = comb;
		this.ws();
	}
	for (;;) {
		var c = this.compound();
		if (!c) throw SelectorError('bad selector: ' + this.s);
		parts.push({ comb: parts.length ? comb : null, c: c });
		var save = this.i;
		this.ws();
		var ch = this.s[this.i];
		if (ch === '>' || ch === '+' || ch === '~') {
			comb = ch; this.i++; this.ws();
		} else if (save !== this.i && ch !== undefined && ch !== ',' && ch !== ')') {
			comb = ' ';
		} else {
			this.i = save;
			break;
		}
	}
	return parts;
};
SelParser.prototype.compound = function () {
	var c = { tag: null, id: null, classes: [], attrs: [], pseudos: [] };
	var any = false;
	if (this.s[this.i] === '*') { this.i++; any = true; }
	else {
		var t = this.ident();
		if (t) { c.tag = t.toLowerCase(); any = true; }
	}
	for (;;) {
		var ch = this.s[this.i];
		if (ch === '#') {
			this.i++; c.id = this.ident(); any = true;
		} else if (ch === '.') {
			this.i++; c.classes.push(this.ident()); any = true;
		} else if (ch === '[') {
			this.i++; this.ws();
			var name = this.ident();
			if (!name) throw SelectorError('bad attribute selector');
			this.ws();
			var op = null, val = null, flag = '';
			var m = /^([~|^$*]?=)/.exec(this.s.slice(this.i));
			if (m) {
				op = m[1]; this.i += op.length; this.ws();
				var q = this.s[this.i];
				if (q === '"' || q === "'") {
					var end = this.i + 1;
					while (end < this.s.length && this.s[end] !== q) {
						if (this.s[end] === '\\') end++;
						end++;
					}
					val = this.s.slice(this.i + 1, end).replace(/\\(.)/g, '$1');
					this.i = end + 1;
				} else {
					val = this.ident();
				}
				this.ws();
				if (/[iIsS]/.test(this.s[this.i] || '') &&
				    this.s[this.i + 1] !== undefined) {
					var f2 = /^([iIsS])\s*\]/.exec(this.s.slice(this.i));
					if (f2) { flag = f2[1].toLowerCase(); this.i++; this.ws(); }
				}
			}
			if (this.s[this.i] !== ']') throw SelectorError('bad attribute selector');
			this.i++;
			c.attrs.push({ name: name.toLowerCase(), op: op, val: val, i: flag === 'i' });
			any = true;
		} else if (ch === ':') {
			this.i++;
			var el = false;
			if (this.s[this.i] === ':') { this.i++; el = true; }
			var pn = (this.ident() || '').toLowerCase();
			var arg = null;
			if (this.s[this.i] === '(') {
				var depth = 1, j = this.i + 1;
				while (j < this.s.length && depth) {
					if (this.s[j] === '(') depth++;
					else if (this.s[j] === ')') depth--;
					j++;
				}
				arg = this.s.slice(this.i + 1, j - 1);
				this.i = j;
			}
			c.pseudos.push(compilePseudo(pn, arg, el));
			any = true;
		} else break;
	}
	return any ? c : null;
};

function parseNth(arg) {
	arg = arg.trim().toLowerCase();
	var of = null;
	var ofm = /^(.*?)\s+of\s+(.*)$/.exec(arg);
	if (ofm) { arg = ofm[1]; of = parseSelectorList(ofm[2]); }
	if (arg === 'odd') return { a: 2, b: 1, of: of };
	if (arg === 'even') return { a: 2, b: 0, of: of };
	var m = /^([+-]?\d*)?n\s*(?:([+-])\s*(\d+))?$/.exec(arg);
	if (m) {
		var a = m[1] === undefined || m[1] === '' || m[1] === '+' ? 1 :
			m[1] === '-' ? -1 : parseInt(m[1], 10);
		var b = m[3] ? parseInt(m[3], 10) * (m[2] === '-' ? -1 : 1) : 0;
		return { a: a, b: b, of: of };
	}
	return { a: 0, b: parseInt(arg, 10) || 0, of: of };
}
function nthMatch(nth, pos) {
	if (nth.a === 0) return pos === nth.b;
	var k = (pos - nth.b) / nth.a;
	return k >= 0 && Math.floor(k) === k;
}
function siblingIndex(el, fromEnd, sameType, filter) {
	var pos = 1;
	var n = fromEnd ? el.nextSibling : el.previousSibling;
	for (; n; n = fromEnd ? n.nextSibling : n.previousSibling) {
		if (n.nodeType !== 1) continue;
		if (sameType && n.tagName !== el.tagName) continue;
		if (filter && !matchesList(n, filter)) continue;
		pos++;
	}
	return pos;
}

function compilePseudo(name, arg, isElementPseudo) {
	if (isElementPseudo) {
		/* pseudo-elements never match real elements */
		return function () { return false; };
	}
	switch (name) {
	case 'first-child': return function (e) { return siblingIndex(e, false) === 1; };
	case 'last-child': return function (e) { return siblingIndex(e, true) === 1; };
	case 'only-child': return function (e) { return siblingIndex(e, false) === 1 && siblingIndex(e, true) === 1; };
	case 'first-of-type': return function (e) { return siblingIndex(e, false, true) === 1; };
	case 'last-of-type': return function (e) { return siblingIndex(e, true, true) === 1; };
	case 'only-of-type': return function (e) { return siblingIndex(e, false, true) === 1 && siblingIndex(e, true, true) === 1; };
	case 'nth-child': var n1 = parseNth(arg); return function (e) {
		if (n1.of && !matchesList(e, n1.of)) return false;
		return nthMatch(n1, siblingIndex(e, false, false, n1.of)); };
	case 'nth-last-child': var n2 = parseNth(arg); return function (e) {
		if (n2.of && !matchesList(e, n2.of)) return false;
		return nthMatch(n2, siblingIndex(e, true, false, n2.of)); };
	case 'nth-of-type': var n3 = parseNth(arg); return function (e) { return nthMatch(n3, siblingIndex(e, false, true)); };
	case 'nth-last-of-type': var n4 = parseNth(arg); return function (e) { return nthMatch(n4, siblingIndex(e, true, true)); };
	case 'not': var nl = parseSelectorList(arg); return function (e) { return !matchesList(e, nl); };
	case 'is': case 'where': case 'matches': case '-webkit-any': case '-moz-any':
		var il; try { il = parseSelectorList(arg); } catch (x) { il = []; }
		return function (e) { return matchesList(e, il); };
	case 'has': var hl = parseSelectorList(arg); return function (e) { return hasMatch(e, hl); };
	case 'empty': return function (e) {
		for (var c = e.firstChild; c; c = c.nextSibling)
			if (c.nodeType === 1 || (c.nodeType === 3 && c.data.length)) return false;
		return true; };
	case 'root': return function (e) { return e.parentNode && e.parentNode.nodeType === 9; };
	case 'scope': return function (e, scope) { return scope ? e === scope : (e.parentNode && e.parentNode.nodeType === 9); };
	case 'checked': return function (e) { return !!(e.checked || (e.tagName === 'OPTION' && e.selected)); };
	case 'disabled': return function (e) { return e.hasAttribute('disabled'); };
	case 'enabled': return function (e) { return /^(INPUT|BUTTON|SELECT|TEXTAREA|OPTION|FIELDSET)$/.test(e.tagName) && !e.hasAttribute('disabled'); };
	case 'required': return function (e) { return e.hasAttribute('required'); };
	case 'optional': return function (e) { return /^(INPUT|SELECT|TEXTAREA)$/.test(e.tagName) && !e.hasAttribute('required'); };
	case 'read-only': return function (e) { return !(/^(INPUT|TEXTAREA)$/.test(e.tagName) && !e.hasAttribute('readonly')) && !e.isContentEditable; };
	case 'read-write': return function (e) { return (/^(INPUT|TEXTAREA)$/.test(e.tagName) && !e.hasAttribute('readonly')) || !!e.isContentEditable; };
	case 'link': case 'any-link': return function (e) { return (e.tagName === 'A' || e.tagName === 'AREA') && e.hasAttribute('href'); };
	case 'focus': case 'focus-visible': case 'focus-within':
		return function (e) { var a = document.activeElement; return !!a && a !== document.body && (a === e || (name === 'focus-within' && e.contains(a))); };
	case 'target': return function (e) { var h = location.hash; return !!h && e.id === decodeURIComponent(h.slice(1)); };
	case 'lang': var lang = (arg || '').trim().toLowerCase(); return function (e) {
		for (var n = e; n && n.nodeType === 1; n = n.parentNode) {
			var l = n.getAttribute('lang');
			if (l !== null) { l = l.toLowerCase(); return l === lang || l.indexOf(lang + '-') === 0; }
		}
		return false; };
	case 'defined': return function () { return true; };
	case 'placeholder-shown': return function (e) { return e.hasAttribute('placeholder') && !e.value; };
	case 'indeterminate': return function () { return false; };
	case 'hover': case 'active': case 'visited': case 'fullscreen': case 'modal':
	case 'popover-open': case 'autofill':
		return function () { return false; };
	default:
		/* vendor-prefixed and unknown pseudo-classes never match */
		return function () { return false; };
	}
}

function attrMatch(e, a) {
	var v = e.getAttribute(a.name);
	if (v === null) return false;
	if (!a.op) return true;
	var want = a.val === null ? '' : a.val;
	if (a.i) { v = v.toLowerCase(); want = want.toLowerCase(); }
	switch (a.op) {
	case '=': return v === want;
	case '~=': return want !== '' && v.split(/\s+/).indexOf(want) >= 0;
	case '|=': return v === want || v.indexOf(want + '-') === 0;
	case '^=': return want !== '' && v.indexOf(want) === 0;
	case '$=': return want !== '' && v.slice(-want.length) === want;
	case '*=': return want !== '' && v.indexOf(want) >= 0;
	}
	return false;
}

function compoundMatch(e, c, scope) {
	if (c.tag && c.tag !== '*' && e.tagName.toLowerCase() !== c.tag) return false;
	if (c.id !== null && e.id !== c.id) return false;
	if (c.classes.length) {
		var cls = ' ' + (e.getAttribute('class') || '').replace(/\s+/g, ' ') + ' ';
		for (var i = 0; i < c.classes.length; i++)
			if (cls.indexOf(' ' + c.classes[i] + ' ') < 0) return false;
	}
	for (var j = 0; j < c.attrs.length; j++)
		if (!attrMatch(e, c.attrs[j])) return false;
	for (var k = 0; k < c.pseudos.length; k++)
		if (!c.pseudos[k](e, scope)) return false;
	return true;
}

function complexMatch(e, parts, idx, scope) {
	var part = parts[idx];
	if (!compoundMatch(e, part.c, scope)) return false;
	if (idx === 0) {
		if (parts.relative && scope) {
			/* relative selector anchored at the scope (for :has) */
			var p0 = e.parentNode;
			if (parts.relative === '>') return p0 === scope;
			if (parts.relative === '+') return prevEl(e) === scope;
			if (parts.relative === '~') {
				for (var s0 = prevEl(e); s0; s0 = prevEl(s0)) if (s0 === scope) return true;
				return false;
			}
		}
		return true;
	}
	var comb = part.comb;
	var n;
	if (comb === '>') {
		n = e.parentNode;
		return isElement(n) && complexMatch(n, parts, idx - 1, scope);
	}
	if (comb === ' ') {
		for (n = e.parentNode; isElement(n); n = n.parentNode)
			if (complexMatch(n, parts, idx - 1, scope)) return true;
		return false;
	}
	if (comb === '+') {
		n = prevEl(e);
		return !!n && complexMatch(n, parts, idx - 1, scope);
	}
	if (comb === '~') {
		for (n = prevEl(e); n; n = prevEl(n))
			if (complexMatch(n, parts, idx - 1, scope)) return true;
		return false;
	}
	return false;
}
function prevEl(e) {
	for (var n = e.previousSibling; n; n = n.previousSibling) if (n.nodeType === 1) return n;
	return null;
}
function matchesList(e, list, scope) {
	for (var i = 0; i < list.length; i++)
		if (complexMatch(e, list[i], list[i].length - 1, scope)) return true;
	return false;
}
function hasMatch(e, list) {
	var all = descendants(e);
	for (var i = 0; i < list.length; i++) {
		var sel = list[i];
		var cand = all;
		if (sel.relative === '+' || sel.relative === '~') {
			cand = [];
			for (var s = e.nextSibling; s; s = s.nextSibling)
				if (s.nodeType === 1) { cand.push(s); if (sel.relative === '+') break; }
		}
		for (var j = 0; j < cand.length; j++)
			if (complexMatch(cand[j], sel, sel.length - 1, e)) return true;
	}
	return false;
}

function qsa(root, selector, first) {
	var list = parseSelectorList(String(selector));
	var scope = root.nodeType === 1 ? root : null;
	/* fast path: #id */
	if (list.length === 1 && list[0].length === 1) {
		var c = list[0][0].c;
		if (c.id && !c.tag && !c.classes.length && !c.attrs.length &&
		    !c.pseudos.length && root.nodeType === 9) {
			var byId = root.getElementById(c.id);
			return first ? byId : (byId ? [byId] : []);
		}
	}
	var all = descendants(root);
	var out = [];
	for (var i = 0; i < all.length; i++) {
		if (matchesList(all[i], list, scope)) {
			if (first) return all[i];
			out.push(all[i]);
		}
	}
	return first ? null : out;
}

function NodeList(arr) {
	var nl = Object.create(NodeList.prototype);
	for (var i = 0; i < arr.length; i++) nl[i] = arr[i];
	def(nl, 'length', { value: arr.length, writable: false, enumerable: false });
	return nl;
}
NodeList.prototype.item = function (i) { return this[i] || null; };
NodeList.prototype.forEach = Array.prototype.forEach;
NodeList.prototype.entries = Array.prototype.entries;
NodeList.prototype.keys = Array.prototype.keys;
NodeList.prototype.values = Array.prototype.values;
NodeList.prototype[Symbol.iterator] = Array.prototype[Symbol.iterator];
global.NodeList = NodeList;
global.HTMLCollection = NodeList;

[ElementP, DocP].forEach(function (P) {
	method(P, 'querySelector', function (s) { return qsa(this, s, true); });
	method(P, 'querySelectorAll', function (s) { return NodeList(qsa(this, s, false)); });
});
method(NodeP, 'querySelector', function (s) { return qsa(this, s, true); });
method(NodeP, 'querySelectorAll', function (s) { return NodeList(qsa(this, s, false)); });
method(ElementP, 'matches', function (s) { return matchesList(this, parseSelectorList(String(s)), this); });
method(ElementP, 'webkitMatchesSelector', ElementP.matches);
method(ElementP, 'msMatchesSelector', ElementP.matches);
method(ElementP, 'closest', function (s) {
	var list = parseSelectorList(String(s));
	for (var n = this; isElement(n); n = n.parentNode)
		if (matchesList(n, list)) return n;
	return null;
});
method(DocP, 'getElementsByName', function (name) {
	return NodeList(qsa(this, '[name="' + String(name).replace(/"/g, '\\"') + '"]'));
});

/* ------------------------------------------------------------------ */
/* ParentNode / ChildNode / Node mixins */

accessor(NodeP, 'isConnected', function () {
	for (var n = this; n; n = n.parentNode) if (n.nodeType === 9) return true;
	return false;
});
method(NodeP, 'getRootNode', function () {
	var n = this;
	while (n.parentNode) n = n.parentNode;
	return n;
});
method(NodeP, 'isSameNode', function (o) { return this === o; });
method(NodeP, 'isEqualNode', function (o) {
	if (!o || o.nodeType !== this.nodeType) return false;
	if (this.nodeType === 1) return this.outerHTML === o.outerHTML;
	return this.nodeValue === o.nodeValue;
});
method(NodeP, 'normalize', function () {});
method(NodeP, 'compareDocumentPosition', function (other) {
	if (other === this) return 0;
	function chain(n) { var a = []; for (; n; n = n.parentNode) a.unshift(n); return a; }
	var a = chain(this), b = chain(other);
	if (a[0] !== b[0]) return 1 | 32;
	var i = 0;
	while (i < a.length && i < b.length && a[i] === b[i]) i++;
	if (i === a.length) return 16 | 4;   /* other is descendant */
	if (i === b.length) return 8 | 2;    /* other is ancestor */
	var pa = a[i], pb = b[i];
	for (var s = pa.nextSibling; s; s = s.nextSibling) if (s === pb) return 4;
	return 2;
});
method(NodeP, 'lookupNamespaceURI', function () { return null; });
method(NodeP, 'isDefaultNamespace', function () { return true; });

function parentMixin(P) {
	accessor(P, 'children', function () { return NodeList(elementChildren(this)); });
	accessor(P, 'childElementCount', function () { return elementChildren(this).length; });
	accessor(P, 'firstElementChild', function () {
		for (var c = this.firstChild; c; c = c.nextSibling) if (c.nodeType === 1) return c;
		return null;
	});
	accessor(P, 'lastElementChild', function () {
		for (var c = this.lastChild; c; c = c.previousSibling) if (c.nodeType === 1) return c;
		return null;
	});
	method(P, 'append', function () {
		if (arguments.length) this.appendChild(nodesToFragment(arguments, this.ownerDocument));
	});
	method(P, 'prepend', function () {
		if (arguments.length) this.insertBefore(nodesToFragment(arguments, this.ownerDocument), this.firstChild);
	});
	method(P, 'replaceChildren', function () {
		while (this.firstChild) this.removeChild(this.firstChild);
		if (arguments.length) this.appendChild(nodesToFragment(arguments, this.ownerDocument));
	});
}
parentMixin(ElementP);
parentMixin(DocP);
parentMixin(NodeP);

function childMixin(P) {
	accessor(P, 'nextElementSibling', function () {
		for (var n = this.nextSibling; n; n = n.nextSibling) if (n.nodeType === 1) return n;
		return null;
	});
	accessor(P, 'previousElementSibling', function () { return prevEl(this); });
	method(P, 'remove', function () { if (this.parentNode) this.parentNode.removeChild(this); });
	method(P, 'before', function () {
		if (this.parentNode) this.parentNode.insertBefore(nodesToFragment(arguments, this.ownerDocument), this);
	});
	method(P, 'after', function () {
		if (this.parentNode) this.parentNode.insertBefore(nodesToFragment(arguments, this.ownerDocument), this.nextSibling);
	});
	method(P, 'replaceWith', function () {
		var p = this.parentNode;
		if (!p) return;
		var frag = nodesToFragment(arguments, this.ownerDocument);
		p.replaceChild(frag, this);
	});
}
childMixin(ElementP);
childMixin(TextP);
childMixin(NodeP);

/* ------------------------------------------------------------------ */
/* Element attributes and reflection */

method(ElementP, 'toggleAttribute', function (name, force) {
	var has = this.hasAttribute(name);
	if (force === undefined) force = !has;
	if (force && !has) this.setAttribute(name, '');
	else if (!force && has) this.removeAttribute(name);
	return !!force;
});
method(ElementP, 'getAttributeNames', function () {
	return ns.attributes ? ns.attributes(this).map(function (a) { return a[0]; }) : [];
});
method(ElementP, 'hasAttributes', function () { return this.getAttributeNames().length > 0; });
accessor(ElementP, 'attributes', function () {
	var el = this;
	var list = ns.attributes ? ns.attributes(el) : [];
	var arr = list.map(function (a) {
		return { name: a[0], localName: a[0], value: a[1], nodeName: a[0], nodeValue: a[1],
			specified: true, ownerElement: el, namespaceURI: null, prefix: null };
	});
	arr.getNamedItem = function (n) {
		n = String(n).toLowerCase();
		for (var i = 0; i < arr.length; i++) if (arr[i].name === n) return arr[i];
		return null;
	};
	arr.item = function (i) { return arr[i] || null; };
	arr.getNamedItemNS = function (x, n) { return arr.getNamedItem(n); };
	return arr;
});
method(ElementP, 'getAttributeNS', function (n, a) { return this.getAttribute(a); });
method(ElementP, 'setAttributeNS', function (n, a, v) { return this.setAttribute(String(a).replace(/^.*:/, ''), v); });
method(ElementP, 'removeAttributeNS', function (n, a) { return this.removeAttribute(a); });
method(ElementP, 'hasAttributeNS', function (n, a) { return this.hasAttribute(a); });
accessor(ElementP, 'localName', function () { return this.tagName.toLowerCase(); });
accessor(ElementP, 'namespaceURI', function () { return 'http://www.w3.org/1999/xhtml'; });
accessor(ElementP, 'prefix', function () { return null; });

function reflect(name, attr, kind) {
	attr = attr || name.toLowerCase();
	accessor(ElementP, name, function () {
		var v = this.getAttribute(attr);
		if (kind === 'bool') return v !== null;
		if (kind === 'int') { var n = parseInt(v, 10); return isNaN(n) ? (name === 'tabIndex' ? (/^(A|BUTTON|INPUT|SELECT|TEXTAREA)$/.test(this.tagName) ? 0 : -1) : 0) : n; }
		if (kind === 'url') return v === null ? '' : (ns.resolveURL(v) || v);
		return v === null ? '' : v;
	}, function (v) {
		if (kind === 'bool') { if (v) this.setAttribute(attr, ''); else this.removeAttribute(attr); }
		else this.setAttribute(attr, String(v));
	});
}
[['title'], ['lang'], ['dir'], ['name'], ['alt'], ['rel'], ['target'],
 ['placeholder'], ['accessKey', 'accesskey'], ['htmlFor', 'for'], ['method'],
 ['enctype'], ['autocomplete'], ['role'], ['slot'], ['label'], ['content'],
 ['charset'], ['media'], ['sizes'], ['srcset'], ['coords'], ['shape'],
 ['download'], ['hreflang'], ['crossOrigin', 'crossorigin'], ['loading'],
 ['decoding'], ['referrerPolicy', 'referrerpolicy'], ['inputMode', 'inputmode'],
 ['pattern'], ['min'], ['max'], ['step'], ['accept'], ['wrap'], ['scope'],
 ['headers'], ['abbr'], ['colSpan', 'colspan', 'int'], ['rowSpan', 'rowspan', 'int'],
 ['maxLength', 'maxlength', 'int'], ['minLength', 'minlength', 'int'],
 ['size', 'size', 'int'], ['cols', 'cols', 'int'], ['rows', 'rows', 'int'],
 ['tabIndex', 'tabindex', 'int'], ['width', 'width', 'int'], ['height', 'height', 'int'],
 ['hidden', 'hidden', 'bool'], ['disabled', 'disabled', 'bool'],
 ['required', 'required', 'bool'], ['readOnly', 'readonly', 'bool'],
 ['multiple', 'multiple', 'bool'], ['autofocus', 'autofocus', 'bool'],
 ['noValidate', 'novalidate', 'bool'], ['open', 'open', 'bool'],
 ['defer', 'defer', 'bool'], ['async', 'async', 'bool'],
 ['noModule', 'nomodule', 'bool'], ['controls', 'controls', 'bool'],
 ['draggable', 'draggable', 'bool'], ['inert', 'inert', 'bool'],
 ['href', 'href', 'url'], ['src', 'src', 'url'], ['action', 'action', 'url'],
 ['poster', 'poster', 'url'], ['cite', 'cite', 'url'], ['data', 'data', 'url'],
 ['formAction', 'formaction', 'url']
].forEach(function (r) { reflect(r[0], r[1], r[2]); });

/* type attribute (input defaults to "text") */
accessor(ElementP, 'type', function () {
	var v = this.getAttribute('type');
	if (this.tagName === 'INPUT') return v ? v.toLowerCase() : 'text';
	if (this.tagName === 'BUTTON') return v ? v.toLowerCase() : 'submit';
	if (this.tagName === 'SELECT') return this.multiple ? 'select-multiple' : 'select-one';
	return v || '';
}, function (v) { this.setAttribute('type', v); });

/* href/URL decomposition on links */
['protocol', 'host', 'hostname', 'port', 'pathname', 'search', 'hash', 'origin'].forEach(function (k) {
	accessor(ElementP, k, function () {
		if (this.tagName !== 'A' && this.tagName !== 'AREA') return undefined;
		try { return new URL(this.href)[k]; } catch (e) { return ''; }
	}, function (v) {
		if (this.tagName !== 'A' && this.tagName !== 'AREA') return;
		try { var u = new URL(this.href); u[k] = v; this.href = u.href; } catch (e) {}
	});
});

/* form control value/checked, backed by the DOM form state */
accessor(ElementP, 'value', function () {
	var t = this.tagName;
	if (t === 'INPUT' || t === 'TEXTAREA') return ns.inputValue ? ns.inputValue(this) : toStr(this.getAttribute('value'));
	if (t === 'SELECT') {
		var o = this.selectedOptions[0];
		return o ? o.value : '';
	}
	if (t === 'OPTION') { var v = this.getAttribute('value'); return v !== null ? v : this.textContent.trim(); }
	if (t === 'BUTTON' || t === 'DATA' || t === 'PARAM' || t === 'LI' || t === 'METER' || t === 'PROGRESS' || t === 'OUTPUT')
		return toStr(this.getAttribute('value'));
	return undefined;
}, function (v) {
	var t = this.tagName;
	v = toStr(v);
	if (t === 'INPUT' || t === 'TEXTAREA') {
		if (ns.setInputValue) ns.setInputValue(this, v);
		else this.setAttribute('value', v);
	} else if (t === 'SELECT') {
		var opts = this.options;
		for (var i = 0; i < opts.length; i++) opts[i].selected = opts[i].value === v;
	} else this.setAttribute('value', v);
});
accessor(ElementP, 'defaultValue', function () {
	return this.tagName === 'TEXTAREA' ? this.textContent : toStr(this.getAttribute('value'));
}, function (v) { if (this.tagName === 'TEXTAREA') this.textContent = v; else this.setAttribute('value', v); });
accessor(ElementP, 'checked', function () {
	if (this.tagName !== 'INPUT') return undefined;
	return ns.inputChecked ? ns.inputChecked(this) : this.hasAttribute('checked');
}, function (v) {
	if (this.tagName !== 'INPUT') return;
	if (ns.setInputChecked) ns.setInputChecked(this, !!v);
	else if (v) this.setAttribute('checked', ''); else this.removeAttribute('checked');
});
accessor(ElementP, 'defaultChecked', function () { return this.hasAttribute('checked'); },
	function (v) { if (v) this.setAttribute('checked', ''); else this.removeAttribute('checked'); });
accessor(ElementP, 'selected', function () {
	if (this.tagName !== 'OPTION') return undefined;
	if (this.hasAttribute('selected')) return true;
	var sel = this.closest('select');
	if (!sel || sel.multiple) return false;
	var opts = sel.options;
	for (var i = 0; i < opts.length; i++) if (opts[i].hasAttribute('selected')) return false;
	return opts[0] === this;
}, function (v) {
	if (this.tagName !== 'OPTION') return;
	var sel = this.closest('select');
	if (v && sel && !sel.multiple) {
		var opts = sel.options;
		for (var i = 0; i < opts.length; i++) if (opts[i] !== this) opts[i].removeAttribute('selected');
	}
	if (v) this.setAttribute('selected', ''); else this.removeAttribute('selected');
});
accessor(ElementP, 'options', function () {
	return this.tagName === 'SELECT' || this.tagName === 'DATALIST' ? NodeList(qsa(this, 'option')) : undefined;
});
accessor(ElementP, 'selectedOptions', function () {
	return NodeList(qsa(this, 'option').filter(function (o) { return o.selected; }));
});
accessor(ElementP, 'selectedIndex', function () {
	if (this.tagName !== 'SELECT') return undefined;
	var opts = this.options;
	for (var i = 0; i < opts.length; i++) if (opts[i].selected) return i;
	return -1;
}, function (idx) {
	var opts = this.options;
	for (var i = 0; i < opts.length; i++) opts[i].selected = i === idx;
});
accessor(ElementP, 'form', function () {
	var f = this.getAttribute('form');
	if (f) return document.getElementById(f);
	return this.closest('form');
});
accessor(ElementP, 'elements', function () {
	if (this.tagName !== 'FORM') return undefined;
	var list = qsa(this, 'input,select,textarea,button,fieldset,output,object');
	var coll = NodeList(list);
	list.forEach(function (el) { var n = el.name || el.id; if (n && !(n in coll)) coll[n] = el; });
	coll.namedItem = function (n) { return coll[n] || null; };
	return coll;
});
accessor(ElementP, 'labels', function () {
	var id = this.id, out = [];
	if (id) out = qsa(document, 'label[for="' + id + '"]');
	var l = this.closest('label');
	if (l && out.indexOf(l) < 0) out.push(l);
	return NodeList(out);
});
accessor(ElementP, 'text', function () { return this.textContent; }, function (v) { this.textContent = v; });
accessor(ElementP, 'validity', function () {
	var el = this;
	var missing = el.required && !el.value;
	return { valid: !missing, valueMissing: missing, typeMismatch: false, patternMismatch: false,
		tooLong: false, tooShort: false, rangeUnderflow: false, rangeOverflow: false,
		stepMismatch: false, badInput: false, customError: !!el.__ns_custom_validity };
});
accessor(ElementP, 'validationMessage', function () { return this.__ns_custom_validity || ''; });
accessor(ElementP, 'willValidate', function () { return true; });
method(ElementP, 'checkValidity', function () { return this.validity.valid; });
method(ElementP, 'reportValidity', function () { return this.validity.valid; });
method(ElementP, 'setCustomValidity', function (m) { this.__ns_custom_validity = toStr(m); });
method(ElementP, 'select', function () {});
method(ElementP, 'setSelectionRange', function () {});
accessor(ElementP, 'selectionStart', function () { return toStr(this.value).length; });
accessor(ElementP, 'selectionEnd', function () { return toStr(this.value).length; });

/* ------------------------------------------------------------------ */
/* classList, dataset, style */

function DOMTokenList(el, attr) { this._el = el; this._attr = attr; }
DOMTokenList.prototype = {
	_get: function () {
		var v = this._el.getAttribute(this._attr);
		return v ? v.split(/\s+/).filter(Boolean) : [];
	},
	_set: function (a) { this._el.setAttribute(this._attr, a.join(' ')); },
	get length() { return this._get().length; },
	get value() { return toStr(this._el.getAttribute(this._attr)); },
	set value(v) { this._el.setAttribute(this._attr, v); },
	item: function (i) { return this._get()[i] || null; },
	contains: function (t) { return this._get().indexOf(String(t)) >= 0; },
	add: function () {
		var a = this._get(), changed = false;
		for (var i = 0; i < arguments.length; i++) {
			var t = String(arguments[i]);
			if (a.indexOf(t) < 0) { a.push(t); changed = true; }
		}
		if (changed || !this._el.hasAttribute(this._attr)) this._set(a);
	},
	remove: function () {
		var rm = Array.prototype.map.call(arguments, String);
		var a = this._get();
		var b = a.filter(function (x) { return rm.indexOf(x) < 0; });
		if (b.length !== a.length) this._set(b);
	},
	toggle: function (t, force) {
		t = String(t);
		var has = this.contains(t);
		if (force === undefined) force = !has;
		if (force && !has) this.add(t);
		else if (!force && has) this.remove(t);
		return !!force;
	},
	replace: function (a, b) {
		var l = this._get(), i = l.indexOf(String(a));
		if (i < 0) return false;
		l[i] = String(b);
		this._set(l.filter(function (x, j) { return l.indexOf(x) === j; }));
		return true;
	},
	supports: function () { return true; },
	forEach: function (f, t) { this._get().forEach(f, t); },
	entries: function () { return this._get().entries(); },
	keys: function () { return this._get().keys(); },
	values: function () { return this._get().values(); },
	toString: function () { return this.value; }
};
DOMTokenList.prototype[Symbol.iterator] = function () { return this._get()[Symbol.iterator](); };
global.DOMTokenList = DOMTokenList;
accessor(ElementP, 'classList', function () { return new DOMTokenList(this, 'class'); },
	function (v) { this.setAttribute('class', v); });
accessor(ElementP, 'relList', function () { return new DOMTokenList(this, 'rel'); });

function camelToKebab(s) {
	if (s === 'cssFloat') return 'float';
	return s.replace(/[A-Z]/g, function (c) { return '-' + c.toLowerCase(); })
		.replace(/^(webkit|moz|ms)-/, '-$1-');
}
function kebabToCamel(s) {
	if (s === 'float') return 'cssFloat';
	return s.replace(/^-(webkit|moz|ms)-/, '$1-').replace(/-([a-z])/g, function (m, c) { return c.toUpperCase(); });
}
accessor(ElementP, 'dataset', function () {
	var el = this;
	return new Proxy({}, {
		get: function (t, k) {
			if (typeof k !== 'string') return undefined;
			var v = el.getAttribute('data-' + camelToKebab(k));
			return v === null ? undefined : v;
		},
		set: function (t, k, v) { el.setAttribute('data-' + camelToKebab(String(k)), String(v)); return true; },
		deleteProperty: function (t, k) { el.removeAttribute('data-' + camelToKebab(String(k))); return true; },
		has: function (t, k) { return el.hasAttribute('data-' + camelToKebab(String(k))); },
		ownKeys: function () {
			return el.getAttributeNames().filter(function (n) { return n.indexOf('data-') === 0; })
				.map(function (n) { return kebabToCamel(n.slice(5)); });
		},
		getOwnPropertyDescriptor: function (t, k) {
			var v = el.getAttribute('data-' + camelToKebab(String(k)));
			return v === null ? undefined : { value: v, enumerable: true, configurable: true, writable: true };
		}
	});
});

function parseStyleText(text) {
	var out = [];
	toStr(text).split(';').forEach(function (decl) {
		var i = decl.indexOf(':');
		if (i < 0) return;
		var name = decl.slice(0, i).trim().toLowerCase();
		var value = decl.slice(i + 1).trim();
		if (!name) return;
		var imp = false;
		var m = /\s*!\s*important\s*$/i.exec(value);
		if (m) { imp = true; value = value.slice(0, m.index); }
		for (var j = 0; j < out.length; j++) if (out[j][0] === name) { out.splice(j, 1); break; }
		out.push([name, value, imp]);
	});
	return out;
}
function serializeStyle(decls) {
	return decls.map(function (d) { return d[0] + ': ' + d[1] + (d[2] ? ' !important' : ''); }).join('; ') + (decls.length ? ';' : '');
}

var styleCache = new WeakMap();
function CSSStyleDeclaration() {}
global.CSSStyleDeclaration = CSSStyleDeclaration;
function makeStyle(el) {
	var s = styleCache.get(el);
	if (s) return s;
	function decls() { return parseStyleText(el.getAttribute('style')); }
	function write(d) {
		var text = serializeStyle(d);
		if (text) el.setAttribute('style', text);
		else if (el.hasAttribute('style')) el.removeAttribute('style');
	}
	var api = {
		getPropertyValue: function (n) {
			n = String(n).toLowerCase();
			var d = decls();
			for (var i = 0; i < d.length; i++) if (d[i][0] === n) return d[i][1];
			return '';
		},
		getPropertyPriority: function (n) {
			var d = decls();
			for (var i = 0; i < d.length; i++) if (d[i][0] === n) return d[i][2] ? 'important' : '';
			return '';
		},
		setProperty: function (n, v, prio) {
			n = String(n);
			if (n.slice(0, 2) !== '--') n = n.toLowerCase();
			v = toStr(v).trim();
			var d = decls();
			for (var i = 0; i < d.length; i++) if (d[i][0] === n) { d.splice(i, 1); break; }
			if (v !== '') d.push([n, v, prio === 'important']);
			write(d);
		},
		removeProperty: function (n) {
			var old = api.getPropertyValue(n);
			var d = decls().filter(function (x) { return x[0] !== String(n).toLowerCase(); });
			write(d);
			return old;
		},
		item: function (i) { var d = decls(); return d[i] ? d[i][0] : ''; }
	};
	s = new Proxy(Object.create(CSSStyleDeclaration.prototype), {
		get: function (t, k) {
			if (hasOwn(api, k)) return api[k];
			if (k === 'cssText') return serializeStyle(decls());
			if (k === 'length') return decls().length;
			if (typeof k !== 'string') return undefined;
			if (/^\d+$/.test(k)) return api.item(+k);
			return api.getPropertyValue(k.slice(0, 2) === '--' ? k : camelToKebab(k));
		},
		set: function (t, k, v) {
			if (k === 'cssText') { write(parseStyleText(v)); return true; }
			if (typeof k !== 'string') return true;
			api.setProperty(k.slice(0, 2) === '--' ? k : camelToKebab(k), v);
			return true;
		},
		has: function (t, k) { return typeof k === 'string'; }
	});
	styleCache.set(el, s);
	return s;
}
accessor(ElementP, 'style', function () { return makeStyle(this); },
	function (v) { this.setAttribute('style', toStr(v)); });

/* ------------------------------------------------------------------ */
/* HTML serialisation and insertion */

var VOID_TAGS = /^(AREA|BASE|BR|COL|EMBED|HR|IMG|INPUT|LINK|META|PARAM|SOURCE|TRACK|WBR)$/;
function escapeText(s) { return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/\u00a0/g, '&nbsp;'); }
function escapeAttr(s) { return s.replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/\u00a0/g, '&nbsp;'); }
function serialize(n) {
	if (n.nodeType === 3) {
		var p = n.parentNode;
		if (p && /^(SCRIPT|STYLE|XMP|IFRAME|NOEMBED|NOFRAMES|PLAINTEXT|NOSCRIPT)$/.test(p.tagName)) return n.data;
		return escapeText(n.data);
	}
	if (n.nodeType === 8) return '<!--' + n.data + '-->';
	if (n.nodeType === 1) {
		var tag = n.tagName.toLowerCase();
		var s = '<' + tag;
		var attrs = ns.attributes ? ns.attributes(n) : [];
		for (var i = 0; i < attrs.length; i++) s += ' ' + attrs[i][0] + '="' + escapeAttr(attrs[i][1]) + '"';
		s += '>';
		if (VOID_TAGS.test(n.tagName)) return s;
		for (var c = n.firstChild; c; c = c.nextSibling) s += serialize(c);
		return s + '</' + tag + '>';
	}
	if (n.nodeType === 11 || n.nodeType === 9) {
		var out = '';
		for (var d = n.firstChild; d; d = d.nextSibling) out += serialize(d);
		return out;
	}
	return '';
}
accessor(ElementP, 'outerHTML', function () { return serialize(this); }, function (html) {
	var p = this.parentNode;
	if (!p) return;
	var tmp = this.ownerDocument.createElement(p.nodeType === 1 ? p.tagName : 'div');
	tmp.innerHTML = html;
	var frag = this.ownerDocument.createDocumentFragment();
	while (tmp.firstChild) frag.appendChild(tmp.firstChild);
	p.replaceChild(frag, this);
});
method(ElementP, 'getHTML', function () { return this.innerHTML; });
method(ElementP, 'setHTMLUnsafe', function (h) { this.innerHTML = h; });
function fragmentFromHTML(ctxEl, html) {
	var tmp = ctxEl.ownerDocument.createElement(ctxEl.nodeType === 1 && ctxEl.tagName ? ctxEl.tagName : 'div');
	tmp.innerHTML = html;
	var frag = ctxEl.ownerDocument.createDocumentFragment();
	while (tmp.firstChild) frag.appendChild(tmp.firstChild);
	return frag;
}
method(ElementP, 'insertAdjacentElement', function (where, node) {
	switch (String(where).toLowerCase()) {
	case 'beforebegin': if (this.parentNode) this.parentNode.insertBefore(node, this); break;
	case 'afterbegin': this.insertBefore(node, this.firstChild); break;
	case 'beforeend': this.appendChild(node); break;
	case 'afterend': if (this.parentNode) this.parentNode.insertBefore(node, this.nextSibling); break;
	default: throw new Error('SyntaxError');
	}
	return node;
});
method(ElementP, 'insertAdjacentHTML', function (where, html) {
	var w = String(where).toLowerCase();
	var ctx = (w === 'beforebegin' || w === 'afterend') ? (this.parentNode || this) : this;
	this.insertAdjacentElement(w, fragmentFromHTML(ctx, html));
});
method(ElementP, 'insertAdjacentText', function (where, text) {
	this.insertAdjacentElement(where, this.ownerDocument.createTextNode(text));
});
accessor(ElementP, 'innerText', function () { return this.textContent; }, function (v) { this.textContent = v; });
accessor(ElementP, 'outerText', function () { return this.textContent; });
accessor(ElementP, 'isContentEditable', function () {
	for (var n = this; isElement(n); n = n.parentNode) {
		var v = n.getAttribute('contenteditable');
		if (v !== null) return v !== 'false';
	}
	return false;
});
accessor(ElementP, 'contentEditable', function () { return toStr(this.getAttribute('contenteditable')) || 'inherit'; },
	function (v) { this.setAttribute('contenteditable', v); });

/* ------------------------------------------------------------------ */
/* Geometry */

function DOMRect(x, y, w, h) {
	this.x = x || 0; this.y = y || 0; this.width = w || 0; this.height = h || 0;
	this.top = this.y; this.left = this.x;
	this.right = this.x + this.width; this.bottom = this.y + this.height;
}
DOMRect.prototype.toJSON = function () {
	return { x: this.x, y: this.y, width: this.width, height: this.height,
		top: this.top, left: this.left, right: this.right, bottom: this.bottom };
};
DOMRect.fromRect = function (r) { r = r || {}; return new DOMRect(r.x, r.y, r.width, r.height); };
global.DOMRect = DOMRect;
global.DOMRectReadOnly = DOMRect;

function docRect(el) { return ns.rect(el); }
method(ElementP, 'getBoundingClientRect', function () {
	var r = docRect(this);
	if (!r) return new DOMRect(0, 0, 0, 0);
	var vp = ns.viewport();
	return new DOMRect(r[0] - vp[0], r[1] - vp[1], r[2], r[3]);
});
method(ElementP, 'getClientRects', function () {
	var r = docRect(this);
	if (!r) return [];
	return [this.getBoundingClientRect()];
});
function isRootish(el) { return el.tagName === 'HTML' || el.tagName === 'BODY'; }
accessor(ElementP, 'offsetWidth', function () { var r = docRect(this); return r ? r[2] : 0; });
accessor(ElementP, 'offsetHeight', function () { var r = docRect(this); return r ? r[3] : 0; });
accessor(ElementP, 'clientWidth', function () {
	if (this.tagName === 'HTML') return ns.viewport()[2];
	var r = docRect(this); return r ? r[6] : 0;
});
accessor(ElementP, 'clientHeight', function () {
	if (this.tagName === 'HTML') return ns.viewport()[3];
	var r = docRect(this); return r ? r[7] : 0;
});
accessor(ElementP, 'clientTop', function () { var r = docRect(this); return r ? r[9] : 0; });
accessor(ElementP, 'clientLeft', function () { var r = docRect(this); return r ? r[8] : 0; });
accessor(ElementP, 'scrollWidth', function () {
	if (isRootish(this)) return Math.max(ns.viewport()[4], ns.viewport()[2]);
	var r = docRect(this); return r ? r[4] : 0;
});
accessor(ElementP, 'scrollHeight', function () {
	if (isRootish(this)) return Math.max(ns.viewport()[5], ns.viewport()[3]);
	var r = docRect(this); return r ? r[5] : 0;
});
accessor(ElementP, 'scrollTop', function () {
	if (isRootish(this)) return ns.viewport()[1];
	var s = ns.elementScroll(this); return s ? s[1] : 0;
}, function (v) {
	if (isRootish(this)) { ns.scrollTo(ns.viewport()[0], +v || 0); return; }
	var s = ns.elementScroll(this); if (s) ns.elementScroll(this, s[0], +v || 0);
});
accessor(ElementP, 'scrollLeft', function () {
	if (isRootish(this)) return ns.viewport()[0];
	var s = ns.elementScroll(this); return s ? s[0] : 0;
}, function (v) {
	if (isRootish(this)) { ns.scrollTo(+v || 0, ns.viewport()[1]); return; }
	var s = ns.elementScroll(this); if (s) ns.elementScroll(this, +v || 0, s[1]);
});
accessor(ElementP, 'offsetParent', function () {
	for (var n = this.parentNode; isElement(n); n = n.parentNode) {
		if (n.tagName === 'BODY') return n;
		var cs = ns.computed(n);
		if (cs && cs.position !== 'static') return n;
		if (/^(TD|TH|TABLE)$/.test(n.tagName)) return n;
	}
	return null;
});
accessor(ElementP, 'offsetTop', function () {
	var r = docRect(this); if (!r) return 0;
	var p = this.offsetParent, pr = p && p.tagName !== 'BODY' ? docRect(p) : null;
	return r[1] - (pr ? pr[1] + pr[9] : 0);
});
accessor(ElementP, 'offsetLeft', function () {
	var r = docRect(this); if (!r) return 0;
	var p = this.offsetParent, pr = p && p.tagName !== 'BODY' ? docRect(p) : null;
	return r[0] - (pr ? pr[0] + pr[8] : 0);
});
function scrollOpts(a, b) {
	if (a && typeof a === 'object') return [a.left, a.top];
	return [a, b];
}
method(ElementP, 'scrollIntoView', function (opt) {
	var r = docRect(this);
	if (!r) return;
	var vp = ns.viewport();
	var block = opt && typeof opt === 'object' ? opt.block : (opt === false ? 'end' : 'start');
	var y = r[1];
	if (block === 'center') y = r[1] - (vp[3] - r[3]) / 2;
	else if (block === 'end') y = r[1] + r[3] - vp[3];
	else if (block === 'nearest') {
		if (r[1] >= vp[1] && r[1] + r[3] <= vp[1] + vp[3]) return;
		y = r[1] < vp[1] ? r[1] : r[1] + r[3] - vp[3];
	}
	ns.scrollTo(vp[0], Math.max(0, Math.round(y)));
});
method(ElementP, 'scrollIntoViewIfNeeded', function () { this.scrollIntoView({ block: 'nearest' }); });
method(ElementP, 'scrollTo', function (a, b) {
	var o = scrollOpts(a, b);
	if (o[0] !== undefined) this.scrollLeft = o[0];
	if (o[1] !== undefined) this.scrollTop = o[1];
});
method(ElementP, 'scroll', ElementP.scrollTo);
method(ElementP, 'scrollBy', function (a, b) {
	var o = scrollOpts(a, b);
	this.scrollLeft += o[0] || 0; this.scrollTop += o[1] || 0;
});

/* ------------------------------------------------------------------ */
/* Focus, click, misc element behaviour */

var activeElement = null;
method(ElementP, 'focus', function () {
	if (activeElement === this) return;
	var prev = activeElement;
	activeElement = this;
	if (prev) {
		prev.dispatchEvent(new FocusEvent('blur', { relatedTarget: this }));
		prev.dispatchEvent(new FocusEvent('focusout', { bubbles: true, relatedTarget: this }));
	}
	this.dispatchEvent(new FocusEvent('focus', { relatedTarget: prev }));
	this.dispatchEvent(new FocusEvent('focusin', { bubbles: true, relatedTarget: prev }));
});
method(ElementP, 'blur', function () {
	if (activeElement !== this) return;
	activeElement = null;
	this.dispatchEvent(new FocusEvent('blur'));
	this.dispatchEvent(new FocusEvent('focusout', { bubbles: true }));
});
method(ElementP, 'click', function () {
	if (this.disabled) return;
	var ev = new MouseEvent('click', { bubbles: true, cancelable: true, view: global });
	var ok = this.dispatchEvent(ev);
	if (!ok) return;
	var a = this.closest('a[href]');
	if (a) {
		var href = a.getAttribute('href');
		if (href && href.charAt(0) === '#') { location.hash = href; return; }
		if (href && !/^javascript:/i.test(href)) location.href = a.href;
		return;
	}
	if (this.tagName === 'INPUT' && (this.type === 'checkbox' || this.type === 'radio')) {
		this.checked = this.type === 'radio' ? true : !this.checked;
		this.dispatchEvent(new Event('input', { bubbles: true }));
		this.dispatchEvent(new Event('change', { bubbles: true }));
	}
	if ((this.tagName === 'BUTTON' && this.type === 'submit') ||
	    (this.tagName === 'INPUT' && (this.type === 'submit' || this.type === 'image'))) {
		var f = this.form;
		if (f) f.requestSubmit(this);
	}
});
method(ElementP, 'submit', function () { submitForm(this, null); });
method(ElementP, 'requestSubmit', function (submitter) {
	if (this.tagName !== 'FORM') return;
	var ev = new Event('submit', { bubbles: true, cancelable: true });
	ev.submitter = submitter || null;
	if (this.dispatchEvent(ev)) submitForm(this, submitter);
});
method(ElementP, 'reset', function () {
	if (this.tagName !== 'FORM') return;
	if (!this.dispatchEvent(new Event('reset', { bubbles: true, cancelable: true }))) return;
	qsa(this, 'input,textarea').forEach(function (el) {
		if (el.type === 'checkbox' || el.type === 'radio') el.checked = el.defaultChecked;
		else el.value = el.defaultValue;
	});
});
function formEntries(form, submitter) {
	var out = [];
	qsa(form, 'input,select,textarea,button').forEach(function (el) {
		var name = el.getAttribute('name');
		if (!name || el.disabled) return;
		var t = el.type;
		if (el.tagName === 'BUTTON' || t === 'submit' || t === 'image' || t === 'button' || t === 'reset') {
			if (el === submitter) out.push([name, el.value]);
			return;
		}
		if ((t === 'checkbox' || t === 'radio') && !el.checked) return;
		if (t === 'file') return;
		if (el.tagName === 'SELECT') {
			el.selectedOptions.forEach(function (o) { out.push([name, o.value]); });
			return;
		}
		out.push([name, (t === 'checkbox' || t === 'radio') && !el.hasAttribute('value') ? 'on' : el.value]);
	});
	return out;
}
function submitForm(form, submitter) {
	var method = (submitter && submitter.getAttribute('formmethod')) || form.getAttribute('method') || 'get';
	var action = (submitter && submitter.getAttribute('formaction')) || form.getAttribute('action') || location.href;
	action = ns.resolveURL(action) || action;
	var q = new URLSearchParams(formEntries(form, submitter)).toString();
	if (method.toLowerCase() === 'post') {
		/* POST submission from script: fall back to GET-style
		 * navigation carrying the fields */
		ns.log('form POST from script submitted as GET');
	}
	var u = action.split('#')[0].split('?')[0];
	location.href = u + (q ? '?' + q : '');
}

accessor(ElementP, 'shadowRoot', function () { return null; });
method(ElementP, 'attachShadow', function () {
	/* no shadow DOM: content renders in the light tree */
	return this;
});
method(ElementP, 'animate', function () {
	var anim = new EventTarget();
	anim.finished = Promise.resolve(anim);
	anim.ready = Promise.resolve(anim);
	anim.playState = 'finished';
	anim.cancel = anim.finish = anim.play = anim.pause = anim.reverse = function () {};
	anim.onfinish = null;
	setTimeout(function () { if (typeof anim.onfinish === 'function') anim.onfinish({}); }, 0);
	return anim;
});
method(ElementP, 'getAnimations', function () { return []; });
method(ElementP, 'requestFullscreen', function () { return Promise.reject(new Error('not supported')); });
method(ElementP, 'setPointerCapture', function () {});
method(ElementP, 'releasePointerCapture', function () {});
method(ElementP, 'hasPointerCapture', function () { return false; });
method(ElementP, 'showModal', function () { this.setAttribute('open', ''); });
method(ElementP, 'show', function () { this.setAttribute('open', ''); });
method(ElementP, 'close', function (rv) {
	if (this.tagName !== 'DIALOG') return;
	this.removeAttribute('open');
	if (rv !== undefined) this.returnValue = rv;
	this.dispatchEvent(new Event('close'));
});
method(ElementP, 'showPopover', function () { this.setAttribute('data-ns-popover-open', ''); });
method(ElementP, 'hidePopover', function () { this.removeAttribute('data-ns-popover-open'); });
method(ElementP, 'togglePopover', function () { this.toggleAttribute('data-ns-popover-open'); });

/* <template>.content */
accessor(ElementP, 'content', function () {
	if (this.tagName !== 'TEMPLATE') return toStr(this.getAttribute('content'));
	var frag = this.ownerDocument.createDocumentFragment();
	for (var c = this.firstChild; c; c = c.nextSibling) frag.appendChild(c.cloneNode(true));
	return frag;
}, function (v) { this.setAttribute('content', v); });

/* images */
accessor(ElementP, 'complete', function () { return true; });
accessor(ElementP, 'naturalWidth', function () { var r = docRect(this); return r ? r[6] : 0; });
accessor(ElementP, 'naturalHeight', function () { var r = docRect(this); return r ? r[7] : 0; });
accessor(ElementP, 'currentSrc', function () { return this.src; });
method(ElementP, 'decode', function () { return Promise.resolve(); });

/* <canvas>: CanvasRenderingContext2D is native (qjs_canvas.c) */
function CanvasGradient(type, a) { this._t = type; this._a = a; this._s = []; }
CanvasGradient.prototype.addColorStop = function (o, c) {
	o = +o;
	if (!(o >= 0 && o <= 1)) throw new DOMException('offset out of range', 'IndexSizeError');
	this._s.push([o, String(c)]);
	this._s.sort(function (x, y) { return x[0] - y[0]; });
};
global.CanvasGradient = CanvasGradient;
function CanvasPattern(c) { this._t = 'pattern'; this._c = c || 'rgba(0,0,0,0)'; }
CanvasPattern.prototype.setTransform = function () {};
global.CanvasPattern = CanvasPattern;
function ImageData(a, b, c) {
	if (typeof a === 'number') {
		this.width = a >>> 0; this.height = b >>> 0;
		this.data = new Uint8ClampedArray(this.width * this.height * 4);
	} else {
		this.data = a; this.width = b >>> 0;
		this.height = c !== undefined ? c >>> 0 : (a.length / 4 / this.width) >>> 0;
	}
	this.colorSpace = 'srgb';
}
global.ImageData = ImageData;
function Path2D(p) { this._ops = p && p._ops ? p._ops.slice() : []; }
['moveTo', 'lineTo', 'bezierCurveTo', 'quadraticCurveTo', 'arc', 'arcTo', 'ellipse',
 'rect', 'roundRect', 'closePath'].forEach(function (n) {
	Path2D.prototype[n] = function () { this._ops.push([n, Array.prototype.slice.call(arguments)]); };
});
Path2D.prototype.addPath = function (p) { if (p && p._ops) this._ops = this._ops.concat(p._ops); };
global.Path2D = Path2D;
var Ctx2DP = global.CanvasRenderingContext2D && global.CanvasRenderingContext2D.prototype;
if (Ctx2DP) {
	var nativeFill = Ctx2DP.fill, nativeStroke = Ctx2DP.stroke, nativeClip = Ctx2DP.clip,
		nativeInPath = Ctx2DP.isPointInPath;
	var replay = function (ctx, p) {
		ctx.beginPath();
		p._ops.forEach(function (op) { ctx[op[0]].apply(ctx, op[1]); });
	};
	Ctx2DP.fill = function (a, b) {
		if (a instanceof Path2D) { replay(this, a); return nativeFill.call(this, b); }
		return nativeFill.call(this, a);
	};
	Ctx2DP.stroke = function (a) {
		if (a instanceof Path2D) replay(this, a);
		return nativeStroke.call(this);
	};
	Ctx2DP.clip = function (a, b) {
		if (a instanceof Path2D) replay(this, a);
		return nativeClip.call(this);
	};
	Ctx2DP.isPointInPath = function (a, x, y) {
		if (a instanceof Path2D) { replay(this, a); return nativeInPath.call(this, x, y); }
		return nativeInPath.call(this, a, x);
	};
	Ctx2DP.createLinearGradient = function (x0, y0, x1, y1) {
		return new CanvasGradient('linear', [+x0, +y0, +x1, +y1, 0, 0]);
	};
	Ctx2DP.createRadialGradient = function (x0, y0, r0, x1, y1, r1) {
		return new CanvasGradient('radial', [+x0, +y0, +r0, +x1, +y1, +r1]);
	};
	Ctx2DP.createConicGradient = function (a, x, y) {
		return new CanvasGradient('radial', [+x, +y, 0, +x, +y, 100]);
	};
	Ctx2DP.createPattern = function () { return new CanvasPattern('rgba(128,128,128,0.5)'); };
	Ctx2DP.createImageData = function (w, h) {
		if (w && typeof w === 'object') return new ImageData(w.width, w.height);
		return new ImageData(Math.abs(w), Math.abs(h));
	};
	Ctx2DP.getImageData = function (x, y, w, h) {
		var buf = this.__getImageData(x, y, w, h);
		return new ImageData(new Uint8ClampedArray(buf), w, h);
	};
	Ctx2DP.putImageData = function (d, dx, dy, sx, sy, sw, sh) {
		if (sx === undefined) { sx = 0; sy = 0; sw = d.width; sh = d.height; }
		this.__putImageData(d.data, d.width, d.height, dx | 0, dy | 0, sx | 0, sy | 0, sw | 0, sh | 0);
	};
	Ctx2DP.getLineDash = function () { return []; };
	Ctx2DP.getContextAttributes = function () { return { alpha: true, desynchronized: false }; };
	['shadowBlur', 'shadowOffsetX', 'shadowOffsetY', 'miterLimit', 'lineDashOffset'].forEach(function (k) {
		def(Ctx2DP, k, { get: function () { return this['__' + k] || (k === 'miterLimit' ? 10 : 0); },
			set: function (v) { this['__' + k] = +v; } });
	});
	['shadowColor', 'filter', 'direction', 'fontKerning', 'letterSpacing', 'imageSmoothingQuality'].forEach(function (k) {
		def(Ctx2DP, k, { get: function () { return this['__' + k] || ''; },
			set: function (v) { this['__' + k] = String(v); } });
	});
}
method(ElementP, 'getContext', function (kind) {
	if (this.tagName !== 'CANVAS') return null;
	if (kind !== '2d') return null;
	if (!this.__ns_ctx2d) this.__ns_ctx2d = ns.canvasContext(this);
	return this.__ns_ctx2d;
});
(function () {
	var w = Object.getOwnPropertyDescriptor(ElementP, 'width');
	var h = Object.getOwnPropertyDescriptor(ElementP, 'height');
	def(ElementP, 'width', {
		get: function () { return this.tagName === 'CANVAS' ? ns.canvasSize(this)[0] : w.get.call(this); },
		set: function (v) {
			w.set.call(this, v);
			if (this.tagName === 'CANVAS') ns.canvasResize(this, v >>> 0, ns.canvasSize(this)[1]);
		}, enumerable: true });
	def(ElementP, 'height', {
		get: function () { return this.tagName === 'CANVAS' ? ns.canvasSize(this)[1] : h.get.call(this); },
		set: function (v) {
			h.set.call(this, v);
			if (this.tagName === 'CANVAS') ns.canvasResize(this, ns.canvasSize(this)[0], v >>> 0);
		}, enumerable: true });
})();
method(ElementP, 'toDataURL', function () {
	return 'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==';
});
method(ElementP, 'toBlob', function (cb) { setTimeout(function () { cb(new Blob([])); }, 0); });

/* ------------------------------------------------------------------ */
/* Events */

function EventTarget() {
	def(this, '__ns_listeners', { value: {}, enumerable: false });
}
EventTarget.prototype.addEventListener = function (type, fn, opts) {
	if (!fn) return;
	var m = this.__ns_listeners || def(this, '__ns_listeners', { value: {}, enumerable: false }) || this.__ns_listeners;
	var a = m[type] || (m[type] = []);
	for (var i = 0; i < a.length; i++) if (a[i].fn === fn) return;
	a.push({ fn: fn, once: !!(opts && opts.once) });
	if (opts && opts.signal) {
		var self = this;
		opts.signal.addEventListener('abort', function () { self.removeEventListener(type, fn); });
	}
};
EventTarget.prototype.removeEventListener = function (type, fn) {
	var m = this.__ns_listeners, a = m && m[type];
	if (!a) return;
	for (var i = 0; i < a.length; i++) if (a[i].fn === fn) { a.splice(i, 1); return; }
};
EventTarget.prototype.dispatchEvent = function (ev) {
	var self = this;
	try { def(ev, 'target', { value: self }); def(ev, 'currentTarget', { value: self }); } catch (e) {}
	var h = this['on' + ev.type];
	if (typeof h === 'function') { try { h.call(this, ev); } catch (e) { reportError(e); } }
	var m = this.__ns_listeners, a = m && m[ev.type];
	if (a) {
		a.slice().forEach(function (l) {
			if (l.once) self.removeEventListener(ev.type, l.fn);
			try {
				if (typeof l.fn === 'function') l.fn.call(self, ev);
				else if (l.fn && typeof l.fn.handleEvent === 'function') l.fn.handleEvent(ev);
			} catch (e) { reportError(e); }
		});
	}
	return !ev.defaultPrevented;
};
global.EventTarget = EventTarget;

function reportError(e) {
	try { console.error('Uncaught ' + e + (e && e.stack ? '\n' + e.stack : '')); } catch (x) {}
}
global.reportError = reportError;

var NativeEvent = global.Event;
function mkEventClass(name, props) {
	var C = function (type, init) {
		if (!(this instanceof C) && !new.target) throw new TypeError("Constructor " + name + " requires 'new'");
		init = init || {};
		var ev = new NativeEvent(type, init);
		props.forEach(function (p) {
			var v = p[0] in init ? init[p[0]] : p[1];
			try { def(ev, p[0], { value: v, enumerable: true }); } catch (e) {}
		});
		try { Object.setPrototypeOf(ev, C.prototype); } catch (e) {}
		return ev;
	};
	C.prototype = Object.create(NativeEvent.prototype);
	C.prototype.constructor = C;
	def(C, 'name', { value: name });
	global[name] = C;
	return C;
}
var mouseProps = [['screenX', 0], ['screenY', 0], ['clientX', 0], ['clientY', 0],
	['pageX', 0], ['pageY', 0], ['offsetX', 0], ['offsetY', 0], ['movementX', 0], ['movementY', 0],
	['button', 0], ['buttons', 0], ['ctrlKey', false], ['shiftKey', false], ['altKey', false],
	['metaKey', false], ['relatedTarget', null], ['detail', 0], ['view', null], ['which', 1]];
mkEventClass('CustomEvent', [['detail', null]]);
mkEventClass('UIEvent', [['detail', 0], ['view', null]]);
mkEventClass('MouseEvent', mouseProps);
mkEventClass('PointerEvent', mouseProps.concat([['pointerId', 1], ['pointerType', 'mouse'],
	['width', 1], ['height', 1], ['pressure', 0], ['isPrimary', true]]));
mkEventClass('WheelEvent', mouseProps.concat([['deltaX', 0], ['deltaY', 0], ['deltaZ', 0], ['deltaMode', 0]]));
mkEventClass('KeyboardEvent', [['key', ''], ['code', ''], ['keyCode', 0], ['charCode', 0],
	['which', 0], ['location', 0], ['repeat', false], ['isComposing', false], ['ctrlKey', false],
	['shiftKey', false], ['altKey', false], ['metaKey', false]]);
mkEventClass('FocusEvent', [['relatedTarget', null], ['view', null]]);
mkEventClass('InputEvent', [['data', null], ['inputType', ''], ['isComposing', false]]);
mkEventClass('TouchEvent', [['touches', []], ['targetTouches', []], ['changedTouches', []]]);
mkEventClass('ErrorEvent', [['message', ''], ['filename', ''], ['lineno', 0], ['colno', 0], ['error', null]]);
mkEventClass('MessageEvent', [['data', null], ['origin', ''], ['lastEventId', ''], ['source', null], ['ports', []]]);
mkEventClass('ProgressEvent', [['lengthComputable', false], ['loaded', 0], ['total', 0]]);
mkEventClass('PopStateEvent', [['state', null]]);
mkEventClass('HashChangeEvent', [['oldURL', ''], ['newURL', '']]);
mkEventClass('PageTransitionEvent', [['persisted', false]]);
mkEventClass('StorageEvent', [['key', null], ['oldValue', null], ['newValue', null], ['url', ''], ['storageArea', null]]);
mkEventClass('AnimationEvent', [['animationName', ''], ['elapsedTime', 0], ['pseudoElement', '']]);
mkEventClass('TransitionEvent', [['propertyName', ''], ['elapsedTime', 0], ['pseudoElement', '']]);
mkEventClass('SubmitEvent', [['submitter', null]]);
mkEventClass('CompositionEvent', [['data', '']]);
mkEventClass('DragEvent', mouseProps.concat([['dataTransfer', null]]));
mkEventClass('ClipboardEvent', [['clipboardData', null]]);

/* Event instance conveniences missing from the native wrapper */
['bubbles', 'cancelable', 'composed', 'isTrusted', 'defaultPrevented', 'eventPhase', 'timeStamp'].forEach(function (k) {
	if (!(k in EventP)) {
		var dflt = { bubbles: false, cancelable: false, composed: false, isTrusted: false, defaultPrevented: false, eventPhase: 2, timeStamp: 0 }[k];
		accessor(EventP, k, function () { return hasOwn(this, '__' + k) ? this['__' + k] : dflt; });
	}
});
if (!('composedPath' in EventP)) method(EventP, 'composedPath', function () {
	var path = [];
	for (var n = this.target; n; n = n.parentNode) path.push(n);
	path.push(global);
	return path;
});
if (!('srcElement' in EventP)) accessor(EventP, 'srcElement', function () { return this.target; });
if (!('returnValue' in EventP)) accessor(EventP, 'returnValue', function () { return !this.defaultPrevented; },
	function (v) { if (v === false) this.preventDefault(); });
if (!('cancelBubble' in EventP)) accessor(EventP, 'cancelBubble', function () { return false; },
	function (v) { if (v) this.stopPropagation(); });

/* AbortController */
function AbortSignal() { EventTarget.call(this); this.aborted = false; this.reason = undefined; this.onabort = null; }
AbortSignal.prototype = Object.create(EventTarget.prototype);
AbortSignal.prototype.throwIfAborted = function () { if (this.aborted) throw this.reason; };
AbortSignal.abort = function (r) { var c = new AbortController(); c.abort(r); return c.signal; };
AbortSignal.timeout = function (ms) { var c = new AbortController(); setTimeout(function () { c.abort(new DOMException('Timeout', 'TimeoutError')); }, ms); return c.signal; };
AbortSignal.any = function (signals) {
	var c = new AbortController();
	signals.forEach(function (s) { if (s.aborted) c.abort(s.reason); else s.addEventListener('abort', function () { c.abort(s.reason); }); });
	return c.signal;
};
function AbortController() { this.signal = new AbortSignal(); }
AbortController.prototype.abort = function (reason) {
	if (this.signal.aborted) return;
	this.signal.aborted = true;
	this.signal.reason = reason === undefined ? new DOMException('signal is aborted without reason', 'AbortError') : reason;
	this.signal.dispatchEvent(new Event('abort'));
};
global.AbortController = AbortController;
global.AbortSignal = AbortSignal;

function DOMException(message, name) {
	var e = new Error(message);
	Object.setPrototypeOf(e, DOMException.prototype);
	e.name = name || 'Error';
	e.code = { IndexSizeError: 1, NotFoundError: 8, NotSupportedError: 9, InvalidStateError: 11,
		SyntaxError: 12, InvalidAccessError: 15, TimeoutError: 23, AbortError: 20, DataCloneError: 25 }[e.name] || 0;
	return e;
}
DOMException.prototype = Object.create(Error.prototype);
DOMException.prototype.constructor = DOMException;
global.DOMException = DOMException;

/* ------------------------------------------------------------------ */
/* MutationObserver (for script-driven changes) */

var observers = [];
var pendingMO = false;
function MutationObserver(cb) {
	this._cb = cb; this._targets = []; this._records = [];
}
MutationObserver.prototype.observe = function (target, opts) {
	opts = opts || {};
	if (opts.attributeFilter || opts.attributeOldValue) opts.attributes = true;
	if (opts.characterDataOldValue) opts.characterData = true;
	for (var i = 0; i < this._targets.length; i++)
		if (this._targets[i].node === target) { this._targets[i].opts = opts; return; }
	this._targets.push({ node: target, opts: opts });
	if (observers.indexOf(this) < 0) observers.push(this);
};
MutationObserver.prototype.disconnect = function () {
	this._targets = []; this._records = [];
	var i = observers.indexOf(this);
	if (i >= 0) observers.splice(i, 1);
};
MutationObserver.prototype.takeRecords = function () { var r = this._records; this._records = []; return r; };
global.MutationObserver = MutationObserver;
global.WebKitMutationObserver = MutationObserver;

function queueMutation(type, target, extra) {
	if (!observers.length) return;
	observers.forEach(function (mo) {
		for (var i = 0; i < mo._targets.length; i++) {
			var t = mo._targets[i], o = t.opts;
			var within = t.node === target || (o.subtree && t.node.contains && t.node.contains(target));
			if (!within) continue;
			if (type === 'childList' && !o.childList) continue;
			if (type === 'attributes' && (!o.attributes || (o.attributeFilter && o.attributeFilter.indexOf(extra.attributeName) < 0))) continue;
			if (type === 'characterData' && !o.characterData) continue;
			var rec = { type: type, target: target, addedNodes: NodeList(extra.added || []),
				removedNodes: NodeList(extra.removed || []), previousSibling: extra.prev || null,
				nextSibling: extra.next || null, attributeName: extra.attributeName || null,
				attributeNamespace: null,
				oldValue: (o.attributeOldValue || o.characterDataOldValue) ? (extra.oldValue === undefined ? null : extra.oldValue) : null };
			mo._records.push(rec);
			break;
		}
	});
	if (!pendingMO) {
		pendingMO = true;
		Promise.resolve().then(function () {
			pendingMO = false;
			observers.slice().forEach(function (mo) {
				if (!mo._records.length) return;
				var recs = mo.takeRecords();
				try { mo._cb.call(mo, recs, mo); } catch (e) { reportError(e); }
			});
		});
	}
}
function wrapMutator(P, name, fn) {
	if (!hasOwn(P, name)) return;
	var orig = P[name];
	if (typeof orig !== 'function') return;
	method(P, name, function () { return fn.call(this, orig, arguments); });
}
function fragKids(n) {
	if (n && n.nodeType === 11) { var a = []; for (var c = n.firstChild; c; c = c.nextSibling) a.push(c); return a; }
	return n ? [n] : [];
}
[NodeP, ElementP, DocP].forEach(function (P) {
	wrapMutator(P, 'appendChild', function (orig, args) {
		var added = fragKids(args[0]);
		var r = orig.apply(this, args);
		queueMutation('childList', this, { added: added });
		return r;
	});
	wrapMutator(P, 'insertBefore', function (orig, args) {
		var added = fragKids(args[0]);
		var r = orig.apply(this, args);
		queueMutation('childList', this, { added: added, next: args[1] || null });
		return r;
	});
	wrapMutator(P, 'removeChild', function (orig, args) {
		var r = orig.apply(this, args);
		queueMutation('childList', this, { removed: [args[0]] });
		return r;
	});
	wrapMutator(P, 'replaceChild', function (orig, args) {
		var added = fragKids(args[0]);
		var r = orig.apply(this, args);
		queueMutation('childList', this, { added: added, removed: [args[1]] });
		return r;
	});
});
wrapMutator(ElementP, 'setAttribute', function (orig, args) {
	var name = String(args[0]).toLowerCase();
	var old = observers.length ? this.getAttribute(name) : null;
	var r = orig.apply(this, args);
	queueMutation('attributes', this, { attributeName: name, oldValue: old });
	return r;
});
wrapMutator(ElementP, 'removeAttribute', function (orig, args) {
	var name = String(args[0]).toLowerCase();
	var old = observers.length ? this.getAttribute(name) : null;
	var r = orig.apply(this, args);
	if (old !== null) queueMutation('attributes', this, { attributeName: name, oldValue: old });
	return r;
});
(function () {
	var d = Object.getOwnPropertyDescriptor(ElementP, 'innerHTML');
	if (d && d.set) {
		accessor(ElementP, 'innerHTML', function () {
			var out = '';
			for (var c = this.firstChild; c; c = c.nextSibling) out += serialize(c);
			return out;
		}, function (v) {
			var removed = observers.length ? elementChildren(this) : [];
			d.set.call(this, v);
			if (observers.length) queueMutation('childList', this, { removed: removed, added: elementChildren(this) });
		});
	}
	var t = Object.getOwnPropertyDescriptor(NodeP, 'textContent');
	if (t && t.set) {
		accessor(NodeP, 'textContent', t.get, function (v) {
			t.set.call(this, v);
			queueMutation(this.nodeType === 3 ? 'characterData' : 'childList', this, {});
		});
	}
	var c = Object.getOwnPropertyDescriptor(ElementP, 'className');
	if (c && c.set) {
		accessor(ElementP, 'className', c.get, function (v) {
			var old = observers.length ? this.getAttribute('class') : null;
			c.set.call(this, v);
			queueMutation('attributes', this, { attributeName: 'class', oldValue: old });
		});
	}
	var idd = Object.getOwnPropertyDescriptor(ElementP, 'id');
	if (idd && idd.set) {
		accessor(ElementP, 'id', idd.get, function (v) {
			idd.set.call(this, v);
			queueMutation('attributes', this, { attributeName: 'id' });
		});
	}
})();

/* Observers of layout: report everything visible once */
function IntersectionObserver(cb, opts) { this._cb = cb; this._els = []; this.root = (opts && opts.root) || null; this.rootMargin = (opts && opts.rootMargin) || '0px'; this.thresholds = [0]; }
IntersectionObserver.prototype.observe = function (el) {
	var self = this;
	if (this._els.indexOf(el) >= 0) return;
	this._els.push(el);
	setTimeout(function () {
		if (self._els.indexOf(el) < 0) return;
		var r = el.getBoundingClientRect();
		var entry = { target: el, isIntersecting: true, intersectionRatio: 1, boundingClientRect: r,
			intersectionRect: r, rootBounds: new DOMRect(0, 0, innerWidth, innerHeight), time: performance.now() };
		try { self._cb([entry], self); } catch (e) { reportError(e); }
	}, 0);
};
IntersectionObserver.prototype.unobserve = function (el) { var i = this._els.indexOf(el); if (i >= 0) this._els.splice(i, 1); };
IntersectionObserver.prototype.disconnect = function () { this._els = []; };
IntersectionObserver.prototype.takeRecords = function () { return []; };
global.IntersectionObserver = IntersectionObserver;

function ResizeObserver(cb) { this._cb = cb; this._els = []; }
ResizeObserver.prototype.observe = function (el) {
	var self = this;
	if (this._els.indexOf(el) >= 0) return;
	this._els.push(el);
	setTimeout(function () {
		if (self._els.indexOf(el) < 0) return;
		var r = el.getBoundingClientRect();
		var size = [{ inlineSize: r.width, blockSize: r.height }];
		try { self._cb([{ target: el, contentRect: r, borderBoxSize: size, contentBoxSize: size, devicePixelContentBoxSize: size }], self); }
		catch (e) { reportError(e); }
	}, 0);
};
ResizeObserver.prototype.unobserve = function (el) { var i = this._els.indexOf(el); if (i >= 0) this._els.splice(i, 1); };
ResizeObserver.prototype.disconnect = function () { this._els = []; };
global.ResizeObserver = ResizeObserver;
global.PerformanceObserver = function () { this.observe = this.disconnect = function () {}; this.takeRecords = function () { return []; }; };
global.PerformanceObserver.supportedEntryTypes = [];
global.ReportingObserver = global.PerformanceObserver;

/* ------------------------------------------------------------------ */
/* Encoding, base64, URL */

function utf8Encode(str) {
	var out = [];
	for (var i = 0; i < str.length; i++) {
		var c = str.charCodeAt(i);
		if (c >= 0xd800 && c < 0xdc00 && i + 1 < str.length) {
			var d = str.charCodeAt(i + 1);
			if (d >= 0xdc00 && d < 0xe000) { c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00); i++; }
		}
		if (c < 0x80) out.push(c);
		else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 63));
		else if (c < 0x10000) out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
		else out.push(0xf0 | (c >> 18), 0x80 | ((c >> 12) & 63), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
	}
	return new Uint8Array(out);
}
function utf8Decode(bytes) {
	var out = '', i = 0, n = bytes.length;
	var chunk = [];
	while (i < n) {
		var b = bytes[i++], c;
		if (b < 0x80) c = b;
		else if (b >= 0xc0 && b < 0xe0 && i < n) c = ((b & 31) << 6) | (bytes[i++] & 63);
		else if (b >= 0xe0 && b < 0xf0 && i + 1 < n) { c = ((b & 15) << 12) | ((bytes[i] & 63) << 6) | (bytes[i + 1] & 63); i += 2; }
		else if (b >= 0xf0 && i + 2 < n) {
			c = ((b & 7) << 18) | ((bytes[i] & 63) << 12) | ((bytes[i + 1] & 63) << 6) | (bytes[i + 2] & 63); i += 3;
			c -= 0x10000;
			chunk.push(0xd800 + (c >> 10), 0xdc00 + (c & 1023));
			c = -1;
		} else c = 0xfffd;
		if (c >= 0) chunk.push(c);
		if (chunk.length > 8192) { out += String.fromCharCode.apply(null, chunk); chunk = []; }
	}
	return out + String.fromCharCode.apply(null, chunk);
}
function TextEncoder() {}
TextEncoder.prototype.encoding = 'utf-8';
TextEncoder.prototype.encode = function (s) { return utf8Encode(toStr(s)); };
TextEncoder.prototype.encodeInto = function (s, dest) {
	var b = utf8Encode(toStr(s)), n = Math.min(b.length, dest.length);
	dest.set(b.subarray(0, n));
	return { read: s.length, written: n };
};
function TextDecoder(label) { this.encoding = (label || 'utf-8').toLowerCase(); }
TextDecoder.prototype.decode = function (buf) {
	if (!buf) return '';
	var bytes = buf instanceof ArrayBuffer ? new Uint8Array(buf) :
		new Uint8Array(buf.buffer || buf, buf.byteOffset || 0, buf.byteLength !== undefined ? buf.byteLength : buf.length);
	if (this.encoding === 'utf-8' || this.encoding === 'utf8') {
		var start = (bytes[0] === 0xef && bytes[1] === 0xbb && bytes[2] === 0xbf) ? 3 : 0;
		return utf8Decode(start ? bytes.subarray(3) : bytes);
	}
	var s = '';
	for (var i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
	return s;
};
global.TextEncoder = TextEncoder;
global.TextDecoder = TextDecoder;

var B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
global.btoa = function (s) {
	s = toStr(s);
	var out = '';
	for (var i = 0; i < s.length; i += 3) {
		var a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2);
		if (a > 255 || b > 255 || c > 255) throw DOMException('Invalid character', 'InvalidCharacterError');
		var n = (a << 16) | ((b || 0) << 8) | (c || 0);
		out += B64[n >> 18] + B64[(n >> 12) & 63] + (i + 1 < s.length ? B64[(n >> 6) & 63] : '=') + (i + 2 < s.length ? B64[n & 63] : '=');
	}
	return out;
};
global.atob = function (s) {
	s = toStr(s).replace(/[\s=]/g, '').replace(/-/g, '+').replace(/_/g, '/');
	var out = '';
	for (var i = 0; i < s.length; i += 4) {
		var n = (B64.indexOf(s[i]) << 18) | (B64.indexOf(s[i + 1]) << 12) |
			((B64.indexOf(s[i + 2]) & 63) << 6) | (B64.indexOf(s[i + 3]) & 63);
		out += String.fromCharCode((n >> 16) & 255);
		if (i + 2 < s.length) out += String.fromCharCode((n >> 8) & 255);
		if (i + 3 < s.length) out += String.fromCharCode(n & 255);
	}
	return out;
};

function URLSearchParams(init) {
	this._list = [];
	if (init === undefined || init === null) return;
	if (typeof init === 'object') {
		if (init instanceof URLSearchParams) { this._list = init._list.slice(); return; }
		if (typeof init[Symbol.iterator] === 'function') {
			for (var p of init) this._list.push([toStr(p[0]), toStr(p[1])]);
			return;
		}
		for (var k in init) if (hasOwn(init, k)) this._list.push([k, toStr(init[k])]);
		return;
	}
	var s = toStr(init);
	if (s[0] === '?') s = s.slice(1);
	var self = this;
	s.split('&').forEach(function (part) {
		if (!part) return;
		var i = part.indexOf('=');
		var k = i < 0 ? part : part.slice(0, i), v = i < 0 ? '' : part.slice(i + 1);
		function dec(x) { try { return decodeURIComponent(x.replace(/\+/g, ' ')); } catch (e) { return x; } }
		self._list.push([dec(k), dec(v)]);
	});
}
function formEncode(s) {
	return encodeURIComponent(s).replace(/%20/g, '+').replace(/[!'()~]/g, function (c) {
		return '%' + c.charCodeAt(0).toString(16).toUpperCase();
	});
}
URLSearchParams.prototype = {
	append: function (k, v) { this._list.push([toStr(k), toStr(v)]); this._update(); },
	delete: function (k) { k = toStr(k); this._list = this._list.filter(function (p) { return p[0] !== k; }); this._update(); },
	get: function (k) { k = toStr(k); for (var i = 0; i < this._list.length; i++) if (this._list[i][0] === k) return this._list[i][1]; return null; },
	getAll: function (k) { k = toStr(k); return this._list.filter(function (p) { return p[0] === k; }).map(function (p) { return p[1]; }); },
	has: function (k) { return this.get(k) !== null; },
	set: function (k, v) {
		k = toStr(k); v = toStr(v);
		var i = this._list.findIndex(function (p) { return p[0] === k; });
		if (i < 0) this._list.push([k, v]);
		else { this._list[i][1] = v; this._list = this._list.filter(function (p, j) { return j <= i || p[0] !== k; }); }
		this._update();
	},
	sort: function () { this._list.sort(function (a, b) { return a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0; }); this._update(); },
	forEach: function (f, t) { var self = this; this._list.forEach(function (p) { f.call(t, p[1], p[0], self); }); },
	keys: function () { return this._list.map(function (p) { return p[0]; })[Symbol.iterator](); },
	values: function () { return this._list.map(function (p) { return p[1]; })[Symbol.iterator](); },
	entries: function () { return this._list.map(function (p) { return [p[0], p[1]]; })[Symbol.iterator](); },
	toString: function () { return this._list.map(function (p) { return formEncode(p[0]) + '=' + formEncode(p[1]); }).join('&'); },
	get size() { return this._list.length; },
	_update: function () { if (this._url) { var q = this.toString(); this._url._search = q ? '?' + q : ''; } }
};
URLSearchParams.prototype[Symbol.iterator] = URLSearchParams.prototype.entries;
global.URLSearchParams = URLSearchParams;

var URL_RE = /^([a-zA-Z][a-zA-Z0-9+.-]*:)(?:\/\/(?:([^:@\/?#]*)(?::([^@\/?#]*))?@)?(\[[^\]]*\]|[^:\/?#]*)(?::(\d*))?)?([^?#]*)(\?[^#]*)?(#.*)?$/;
var DEFAULT_PORTS = { 'http:': '80', 'https:': '443', 'ws:': '80', 'wss:': '443', 'ftp:': '21' };
function URL(url, base) {
	if (!(this instanceof URL)) throw new TypeError("Constructor URL requires 'new'");
	var abs;
	url = toStr(url);
	if (base !== undefined && base !== null) {
		base = base instanceof URL ? base.href : toStr(base);
		if (!URL_RE.test(base)) throw new TypeError('Invalid base URL: ' + base);
		abs = ns.resolveURL(url, base);
	} else {
		if (!/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(url)) throw new TypeError('Invalid URL: ' + url);
		abs = /^(https?|ftp|wss?|file):/i.test(url) ? ns.resolveURL(url, url) : url;
	}
	if (!abs) throw new TypeError('Invalid URL: ' + url);
	this._parse(abs);
}
URL.prototype._parse = function (s) {
	var m = URL_RE.exec(s);
	if (!m) throw new TypeError('Invalid URL: ' + s);
	this._protocol = m[1].toLowerCase();
	this._username = m[2] || '';
	this._password = m[3] || '';
	this._hostname = (m[4] || '').toLowerCase();
	this._port = m[5] && m[5] !== DEFAULT_PORTS[this._protocol] ? m[5] : '';
	this._hasAuthority = s.slice(this._protocol.length, this._protocol.length + 2) === '//';
	this._pathname = m[6] || (this._hasAuthority ? '/' : '');
	this._search = m[7] && m[7] !== '?' ? m[7] : '';
	this._hash = m[8] && m[8] !== '#' ? m[8] : '';
	var sp = new URLSearchParams(this._search);
	sp._url = this;
	this._sp = sp;
};
Object.defineProperties(URL.prototype, {
	protocol: { get: function () { return this._protocol; }, set: function (v) { v = toStr(v); this._protocol = (v.slice(-1) === ':' ? v : v + ':').toLowerCase(); } },
	username: { get: function () { return this._username; }, set: function (v) { this._username = toStr(v); } },
	password: { get: function () { return this._password; }, set: function (v) { this._password = toStr(v); } },
	hostname: { get: function () { return this._hostname; }, set: function (v) { this._hostname = toStr(v).toLowerCase(); } },
	port: { get: function () { return this._port; }, set: function (v) { v = toStr(v); this._port = v === DEFAULT_PORTS[this._protocol] ? '' : v; } },
	host: { get: function () { return this._hostname + (this._port ? ':' + this._port : ''); },
		set: function (v) { var p = toStr(v).split(':'); this.hostname = p[0]; this.port = p[1] || ''; } },
	origin: { get: function () { return this._hasAuthority ? this._protocol + '//' + this.host : 'null'; } },
	pathname: { get: function () { return this._pathname; }, set: function (v) { v = toStr(v); this._pathname = v[0] === '/' || !this._hasAuthority ? v : '/' + v; } },
	search: { get: function () { return this._search; }, set: function (v) { v = toStr(v); this._search = v && v[0] !== '?' ? '?' + v : (v === '?' ? '' : v); var sp = new URLSearchParams(this._search); sp._url = this; this._sp = sp; } },
	searchParams: { get: function () { return this._sp; } },
	hash: { get: function () { return this._hash; }, set: function (v) { v = toStr(v); this._hash = v && v[0] !== '#' ? '#' + v : (v === '#' ? '' : v); } },
	href: { get: function () {
		var auth = this._username ? this._username + (this._password ? ':' + this._password : '') + '@' : '';
		return this._protocol + (this._hasAuthority ? '//' + auth + this.host : '') + this._pathname + this._search + this._hash;
	}, set: function (v) { this._parse(new URL(v).href); } }
});
URL.prototype.toString = function () { return this.href; };
URL.prototype.toJSON = function () { return this.href; };
URL.canParse = function (u, b) { try { new URL(u, b); return true; } catch (e) { return false; } };
URL.parse = function (u, b) { try { return new URL(u, b); } catch (e) { return null; } };
var blobUrls = {};
var blobCounter = 0;
URL.createObjectURL = function (blob) {
	var id = 'blob:' + (location.origin || 'null') + '/' + (++blobCounter);
	blobUrls[id] = blob;
	return id;
};
URL.revokeObjectURL = function (u) { delete blobUrls[u]; };
global.URL = URL;
global.webkitURL = URL;

/* ------------------------------------------------------------------ */
/* Blob, File, FormData */

function Blob(parts, opts) {
	var chunks = [], size = 0;
	(parts || []).forEach(function (p) {
		var b;
		if (p instanceof Blob) b = p._bytes;
		else if (p instanceof ArrayBuffer) b = new Uint8Array(p);
		else if (ArrayBuffer.isView(p)) b = new Uint8Array(p.buffer, p.byteOffset, p.byteLength);
		else b = utf8Encode(toStr(p));
		chunks.push(b); size += b.length;
	});
	var all = new Uint8Array(size), off = 0;
	chunks.forEach(function (c) { all.set(c, off); off += c.length; });
	this._bytes = all;
	this.size = size;
	this.type = (opts && opts.type) ? String(opts.type).toLowerCase() : '';
}
Blob.prototype.text = function () { return Promise.resolve(utf8Decode(this._bytes)); };
Blob.prototype.arrayBuffer = function () { return Promise.resolve(this._bytes.slice().buffer); };
Blob.prototype.bytes = function () { return Promise.resolve(this._bytes.slice()); };
Blob.prototype.slice = function (a, b, type) { return new Blob([this._bytes.slice(a, b)], { type: type }); };
Blob.prototype.stream = function () { throw new Error('streams not supported'); };
function File(parts, name, opts) {
	Blob.call(this, parts, opts);
	this.name = toStr(name);
	this.lastModified = (opts && opts.lastModified) || Date.now();
}
File.prototype = Object.create(Blob.prototype);
global.Blob = Blob;
global.File = File;
function FileReader() { EventTarget.call(this); this.readyState = 0; this.result = null; this.error = null; }
FileReader.prototype = Object.create(EventTarget.prototype);
['readAsText', 'readAsArrayBuffer', 'readAsDataURL', 'readAsBinaryString'].forEach(function (m) {
	FileReader.prototype[m] = function (blob) {
		var self = this;
		self.readyState = 1;
		setTimeout(function () {
			var b = blob._bytes;
			if (m === 'readAsText') self.result = utf8Decode(b);
			else if (m === 'readAsArrayBuffer') self.result = b.slice().buffer;
			else {
				var s = '';
				for (var i = 0; i < b.length; i++) s += String.fromCharCode(b[i]);
				self.result = m === 'readAsBinaryString' ? s : 'data:' + (blob.type || 'application/octet-stream') + ';base64,' + btoa(s);
			}
			self.readyState = 2;
			self.dispatchEvent(new ProgressEvent('load'));
			self.dispatchEvent(new ProgressEvent('loadend'));
		}, 0);
	};
});
FileReader.prototype.abort = function () {};
global.FileReader = FileReader;

function FormData(form) {
	this._list = form && form.tagName === 'FORM' ? formEntries(form, null) : [];
}
FormData.prototype = {
	append: function (k, v) { this._list.push([toStr(k), v instanceof Blob ? v : toStr(v)]); },
	delete: function (k) { this._list = this._list.filter(function (p) { return p[0] !== k; }); },
	get: function (k) { for (var i = 0; i < this._list.length; i++) if (this._list[i][0] === k) return this._list[i][1]; return null; },
	getAll: function (k) { return this._list.filter(function (p) { return p[0] === k; }).map(function (p) { return p[1]; }); },
	has: function (k) { return this.get(k) !== null; },
	set: function (k, v) { this.delete(k); this.append(k, v); },
	forEach: function (f, t) { var self = this; this._list.forEach(function (p) { f.call(t, p[1], p[0], self); }); },
	keys: function () { return this._list.map(function (p) { return p[0]; })[Symbol.iterator](); },
	values: function () { return this._list.map(function (p) { return p[1]; })[Symbol.iterator](); },
	entries: function () { return this._list.map(function (p) { return [p[0], p[1]]; })[Symbol.iterator](); }
};
FormData.prototype[Symbol.iterator] = FormData.prototype.entries;
global.FormData = FormData;

/* ------------------------------------------------------------------ */
/* fetch and XMLHttpRequest */

function Headers(init) {
	this._map = [];
	if (!init) return;
	var self = this;
	if (init instanceof Headers) init.forEach(function (v, k) { self.append(k, v); });
	else if (Array.isArray(init)) init.forEach(function (p) { self.append(p[0], p[1]); });
	else for (var k in init) if (hasOwn(init, k)) this.append(k, init[k]);
}
Headers.prototype = {
	append: function (k, v) {
		k = toStr(k).toLowerCase(); v = toStr(v).trim();
		for (var i = 0; i < this._map.length; i++)
			if (this._map[i][0] === k) { this._map[i][1] += ', ' + v; return; }
		this._map.push([k, v]);
	},
	set: function (k, v) { this.delete(k); this.append(k, v); },
	get: function (k) { k = toStr(k).toLowerCase(); for (var i = 0; i < this._map.length; i++) if (this._map[i][0] === k) return this._map[i][1]; return null; },
	has: function (k) { return this.get(k) !== null; },
	delete: function (k) { k = toStr(k).toLowerCase(); this._map = this._map.filter(function (p) { return p[0] !== k; }); },
	forEach: function (f, t) { var self = this; this._map.forEach(function (p) { f.call(t, p[1], p[0], self); }); },
	keys: function () { return this._map.map(function (p) { return p[0]; })[Symbol.iterator](); },
	values: function () { return this._map.map(function (p) { return p[1]; })[Symbol.iterator](); },
	entries: function () { return this._map.map(function (p) { return [p[0], p[1]]; })[Symbol.iterator](); },
	getSetCookie: function () { return []; }
};
Headers.prototype[Symbol.iterator] = Headers.prototype.entries;
global.Headers = Headers;

function parseRawHeaders(raw) {
	var h = new Headers();
	var status = '';
	raw.split(/\r?\n/).forEach(function (line) {
		if (/^HTTP\//i.test(line)) { status = line.replace(/^\S+\s+\d+\s*/, ''); return; }
		var i = line.indexOf(':');
		if (i > 0) h.append(line.slice(0, i).trim(), line.slice(i + 1).trim());
	});
	return { headers: h, statusText: status };
}

function bodyToText(body) {
	if (body === undefined || body === null) return null;
	if (typeof body === 'string') return body;
	if (body instanceof URLSearchParams) return body.toString();
	if (body instanceof FormData) {
		return new URLSearchParams(body._list.filter(function (p) { return typeof p[1] === 'string'; })).toString();
	}
	if (body instanceof Blob) return utf8Decode(body._bytes);
	if (body instanceof ArrayBuffer) return utf8Decode(new Uint8Array(body));
	if (ArrayBuffer.isView(body)) return utf8Decode(new Uint8Array(body.buffer, body.byteOffset, body.byteLength));
	return toStr(body);
}
function bodyContentType(body) {
	if (typeof body === 'string') return 'text/plain;charset=UTF-8';
	if (body instanceof URLSearchParams || body instanceof FormData) return 'application/x-www-form-urlencoded;charset=UTF-8';
	if (body instanceof Blob && body.type) return body.type;
	return null;
}

var fetchId = 0;
var fetchPending = {};
global.__ns_fetch_done = function (id, status, url, rawHeaders, buffer, error) {
	var cb = fetchPending[id];
	if (!cb) return;
	delete fetchPending[id];
	cb(status, url, rawHeaders, buffer, error);
};
function nativeRequest(method, url, headers, body, cb) {
	var id = ++fetchId;
	var hdrs = [];
	headers.forEach(function (v, k) { hdrs.push(k + ': ' + v); });
	fetchPending[id] = cb;
	var m = method.toUpperCase();
	var b = (m === 'GET' || m === 'HEAD') ? null : (body === null ? '' : body);
	if (m !== 'GET' && m !== 'POST' && m !== 'HEAD')
		hdrs.push('X-HTTP-Method-Override: ' + m);
	if (!ns.fetch(id, m, url, hdrs, b)) {
		delete fetchPending[id];
		setTimeout(function () { cb(0, url, '', new ArrayBuffer(0), 'Failed to fetch'); }, 0);
	}
	return id;
}

function Body() {}
Body.prototype = {
	_consume: function () {
		if (this.bodyUsed) return Promise.reject(new TypeError('Body has already been consumed'));
		this.bodyUsed = true;
		return Promise.resolve(this._buffer || new ArrayBuffer(0));
	},
	arrayBuffer: function () { return this._consume(); },
	bytes: function () { return this._consume().then(function (b) { return new Uint8Array(b); }); },
	text: function () { return this._consume().then(function (b) { return utf8Decode(new Uint8Array(b)); }); },
	json: function () { return this.text().then(JSON.parse); },
	blob: function () { var t = this.headers.get('content-type') || ''; return this._consume().then(function (b) { return new Blob([b], { type: t }); }); },
	formData: function () { return this.text().then(function (t) { var f = new FormData(); new URLSearchParams(t).forEach(function (v, k) { f.append(k, v); }); return f; }); }
};

function Request(input, init) {
	init = init || {};
	if (input instanceof Request) {
		this.url = input.url; this.method = input.method; this.headers = new Headers(input.headers);
		this._bodyText = input._bodyText; this.signal = input.signal;
	} else {
		this.url = new URL(toStr(input), ns.baseURL()).href;
		this.method = 'GET'; this.headers = new Headers();
		this._bodyText = null;
	}
	if (init.method) this.method = String(init.method).toUpperCase();
	if (init.headers) this.headers = new Headers(init.headers);
	if (init.body !== undefined && init.body !== null) {
		this._bodyText = bodyToText(init.body);
		var ct = bodyContentType(init.body);
		if (ct && !this.headers.has('content-type')) this.headers.set('content-type', ct);
	}
	if (init.signal) this.signal = init.signal;
	this.credentials = init.credentials || 'same-origin';
	this.mode = init.mode || 'cors';
	this.cache = init.cache || 'default';
	this.redirect = init.redirect || 'follow';
	this.referrer = 'about:client';
	this.bodyUsed = false;
	this._buffer = this._bodyText !== null ? utf8Encode(this._bodyText).buffer : null;
}
Request.prototype = Object.create(Body.prototype);
Request.prototype.clone = function () { return new Request(this); };
global.Request = Request;

var STATUS_TEXT = { 200: 'OK', 201: 'Created', 204: 'No Content', 301: 'Moved Permanently', 302: 'Found',
	304: 'Not Modified', 400: 'Bad Request', 401: 'Unauthorized', 403: 'Forbidden', 404: 'Not Found',
	500: 'Internal Server Error', 502: 'Bad Gateway', 503: 'Service Unavailable' };
function Response(body, init) {
	init = init || {};
	this.status = init.status === undefined ? 200 : init.status;
	this.statusText = init.statusText !== undefined ? init.statusText : (STATUS_TEXT[this.status] || '');
	this.ok = this.status >= 200 && this.status < 300;
	this.headers = init.headers instanceof Headers ? init.headers : new Headers(init.headers);
	this.url = init.url || '';
	this.type = init.type || 'default';
	this.redirected = false;
	this.bodyUsed = false;
	if (body instanceof ArrayBuffer) this._buffer = body;
	else if (body !== undefined && body !== null) {
		this._buffer = body instanceof Blob ? body._bytes.slice().buffer : utf8Encode(bodyToText(body)).buffer;
		var ct = bodyContentType(body);
		if (ct && !this.headers.has('content-type')) this.headers.set('content-type', ct);
	} else this._buffer = null;
	this.body = this._buffer ? {} : null;
}
Response.prototype = Object.create(Body.prototype);
Response.prototype.clone = function () {
	return new Response(this._buffer ? this._buffer.slice(0) : null,
		{ status: this.status, statusText: this.statusText, headers: new Headers(this.headers), url: this.url });
};
Response.error = function () { var r = new Response(null, { status: 0 }); r.type = 'error'; return r; };
Response.json = function (data, init) {
	init = init || {};
	var h = new Headers(init.headers);
	if (!h.has('content-type')) h.set('content-type', 'application/json');
	return new Response(JSON.stringify(data), { status: init.status, headers: h });
};
Response.redirect = function (url, status) { return new Response(null, { status: status || 302, headers: { location: url } }); };
global.Response = Response;

global.fetch = function (input, init) {
	return new Promise(function (resolve, reject) {
		var req;
		try { req = new Request(input, init); } catch (e) { reject(new TypeError(e.message)); return; }
		if (req.signal && req.signal.aborted) { reject(req.signal.reason); return; }
		if (/^blob:/.test(req.url) && blobUrls[req.url]) {
			var b = blobUrls[req.url];
			resolve(new Response(b, { status: 200, url: req.url, headers: { 'content-type': b.type } }));
			return;
		}
		if (/^data:/.test(req.url)) {
			var m = /^data:([^,]*?)(;base64)?,(.*)$/.exec(req.url);
			if (!m) { reject(new TypeError('Failed to fetch')); return; }
			var data = m[2] ? atob(m[3]) : decodeURIComponent(m[3]);
			var bytes = new Uint8Array(data.length);
			for (var i = 0; i < data.length; i++) bytes[i] = data.charCodeAt(i) & 255;
			resolve(new Response(bytes.buffer, { status: 200, url: req.url, headers: { 'content-type': m[1] || 'text/plain' } }));
			return;
		}
		var id = nativeRequest(req.method, req.url, req.headers, req._bodyText, function (status, url, raw, buf, error) {
			if (error || !status) { reject(new TypeError('Failed to fetch')); return; }
			var ph = parseRawHeaders(raw);
			var resp = new Response(buf, { status: status, statusText: ph.statusText || STATUS_TEXT[status] || '', headers: ph.headers, url: url });
			resp.redirected = url !== req.url;
			resp.type = 'basic';
			resolve(resp);
		});
		if (req.signal) req.signal.addEventListener('abort', function () {
			if (ns.fetchAbort) ns.fetchAbort(id);
			delete fetchPending[id];
			reject(req.signal.reason);
		});
	});
};

function XMLHttpRequest() {
	EventTarget.call(this);
	this.readyState = 0;
	this.status = 0;
	this.statusText = '';
	this.responseType = '';
	this.response = null;
	this.responseText = '';
	this.responseXML = null;
	this.responseURL = '';
	this.timeout = 0;
	this.withCredentials = false;
	this.upload = new EventTarget();
	this._headers = new Headers();
	this._respHeaders = new Headers();
	this.onreadystatechange = null;
}
XMLHttpRequest.prototype = Object.create(EventTarget.prototype);
XMLHttpRequest.UNSENT = 0; XMLHttpRequest.OPENED = 1; XMLHttpRequest.HEADERS_RECEIVED = 2;
XMLHttpRequest.LOADING = 3; XMLHttpRequest.DONE = 4;
XMLHttpRequest.prototype.UNSENT = 0; XMLHttpRequest.prototype.OPENED = 1;
XMLHttpRequest.prototype.HEADERS_RECEIVED = 2; XMLHttpRequest.prototype.LOADING = 3;
XMLHttpRequest.prototype.DONE = 4;
XMLHttpRequest.prototype._state = function (s) {
	this.readyState = s;
	this.dispatchEvent(new Event('readystatechange'));
};
XMLHttpRequest.prototype.open = function (method, url) {
	this._method = toStr(method).toUpperCase();
	this._url = new URL(toStr(url), ns.baseURL()).href;
	this._headers = new Headers();
	this._aborted = false;
	this._state(1);
};
XMLHttpRequest.prototype.setRequestHeader = function (k, v) { this._headers.append(k, v); };
XMLHttpRequest.prototype.overrideMimeType = function (m) { this._mime = m; };
XMLHttpRequest.prototype.getResponseHeader = function (k) { return this._respHeaders.get(k); };
XMLHttpRequest.prototype.getAllResponseHeaders = function () {
	var s = '';
	this._respHeaders.forEach(function (v, k) { s += k + ': ' + v + '\r\n'; });
	return s;
};
XMLHttpRequest.prototype.abort = function () {
	if (this._id && ns.fetchAbort) ns.fetchAbort(this._id);
	delete fetchPending[this._id];
	this._aborted = true;
	if (this.readyState > 0 && this.readyState < 4) {
		this._state(4);
		this.dispatchEvent(new ProgressEvent('abort'));
		this.dispatchEvent(new ProgressEvent('loadend'));
	}
	this.readyState = 0;
};
XMLHttpRequest.prototype.send = function (body) {
	var self = this;
	var text = (this._method === 'GET' || this._method === 'HEAD') ? null : bodyToText(body);
	var ct = body !== undefined && body !== null ? bodyContentType(body) : null;
	if (ct && !this._headers.has('content-type')) this._headers.set('content-type', ct);
	if (!this._headers.has('x-requested-with') && false) this._headers.set('x-requested-with', 'XMLHttpRequest');
	this.dispatchEvent(new ProgressEvent('loadstart'));
	var finish = function (status, url, raw, buf, error) {
		if (self._aborted) return;
		if (error || !status) {
			self.status = 0;
			self._state(4);
			self.dispatchEvent(new ProgressEvent('error'));
			self.dispatchEvent(new ProgressEvent('loadend'));
			return;
		}
		var ph = parseRawHeaders(raw);
		self._respHeaders = ph.headers;
		self.status = status;
		self.statusText = ph.statusText || STATUS_TEXT[status] || '';
		self.responseURL = url;
		self._state(2);
		self._state(3);
		var t = self.responseType;
		if (t === 'arraybuffer') self.response = buf;
		else if (t === 'blob') self.response = new Blob([buf], { type: ph.headers.get('content-type') || '' });
		else {
			var str = utf8Decode(new Uint8Array(buf));
			if (t === '' || t === 'text') { self.responseText = str; self.response = str; }
			else if (t === 'json') { try { self.response = JSON.parse(str); } catch (e) { self.response = null; } }
			else if (t === 'document') { self.response = self.responseXML = ns.parseDocument(str); }
		}
		self._state(4);
		self.dispatchEvent(new ProgressEvent('load', { lengthComputable: true, loaded: buf.byteLength, total: buf.byteLength }));
		self.dispatchEvent(new ProgressEvent('loadend'));
	};
	this._id = nativeRequest(this._method, this._url, this._headers, text, finish);
	if (this.timeout > 0) {
		setTimeout(function () {
			if (self.readyState !== 4 && !self._aborted) {
				self.abort();
				self.dispatchEvent(new ProgressEvent('timeout'));
			}
		}, this.timeout);
	}
};
global.XMLHttpRequest = XMLHttpRequest;
global.WebSocket = function (url) {
	var ws = new EventTarget();
	ws.url = url; ws.readyState = 3; ws.bufferedAmount = 0; ws.protocol = ''; ws.extensions = '';
	ws.send = ws.close = function () {};
	setTimeout(function () {
		ws.dispatchEvent(new Event('error'));
		ws.dispatchEvent(new CloseEvent('close', { code: 1006, wasClean: false }));
	}, 0);
	return ws;
};
global.WebSocket.CONNECTING = 0; global.WebSocket.OPEN = 1; global.WebSocket.CLOSING = 2; global.WebSocket.CLOSED = 3;
mkEventClass('CloseEvent', [['code', 0], ['reason', ''], ['wasClean', false]]);
global.EventSource = function (url) {
	var es = new EventTarget();
	es.url = url; es.readyState = 2; es.close = function () {};
	setTimeout(function () { es.dispatchEvent(new Event('error')); }, 0);
	return es;
};
global.navigator.sendBeacon = function (url, data) {
	try { fetch(url, { method: 'POST', body: data }).catch(function () {}); } catch (e) {}
	return true;
};

/* ------------------------------------------------------------------ */
/* Storage */

function makeStorage(kind, persist) {
	var data = {};
	if (persist) {
		try { var t = ns.storageLoad(kind); if (t) data = JSON.parse(t) || {}; } catch (e) { data = {}; }
	}
	var saveTimer = null;
	function save() {
		if (!persist || saveTimer) return;
		saveTimer = setTimeout(function () { saveTimer = null; ns.storageSave(kind, JSON.stringify(data)); }, 50);
	}
	var api = {
		getItem: function (k) { k = toStr(k); return hasOwn(data, k) ? data[k] : null; },
		setItem: function (k, v) { data[toStr(k)] = toStr(v); save(); },
		removeItem: function (k) { k = toStr(k); if (hasOwn(data, k)) { delete data[k]; save(); } },
		clear: function () { data = {}; save(); },
		key: function (i) { return Object.keys(data)[i] || null; }
	};
	return new Proxy({}, {
		get: function (t, k) {
			if (hasOwn(api, k)) return api[k];
			if (k === 'length') return Object.keys(data).length;
			if (typeof k !== 'string') return undefined;
			return hasOwn(data, k) ? data[k] : undefined;
		},
		set: function (t, k, v) { api.setItem(k, v); return true; },
		deleteProperty: function (t, k) { api.removeItem(k); return true; },
		has: function (t, k) { return hasOwn(data, k) || hasOwn(api, k); },
		ownKeys: function () { return Object.keys(data); },
		getOwnPropertyDescriptor: function (t, k) {
			return hasOwn(data, k) ? { value: data[k], enumerable: true, configurable: true, writable: true } : undefined;
		}
	});
}
var localStore = null, sessionStore = null;
def(global, 'localStorage', { get: function () { return localStore || (localStore = makeStorage('local', true)); }, enumerable: true });
def(global, 'sessionStorage', { get: function () { return sessionStore || (sessionStore = makeStorage('session', false)); }, enumerable: true });
global.Storage = function () {};
global.indexedDB = {
	open: function () {
		var req = new EventTarget();
		setTimeout(function () { req.error = new DOMException('IndexedDB is not supported', 'NotSupportedError'); req.dispatchEvent(new Event('error')); }, 0);
		return req;
	},
	deleteDatabase: function () { return this.open(); },
	databases: function () { return Promise.resolve([]); }
};
global.caches = { open: function () { return Promise.reject(new Error('not supported')); }, match: function () { return Promise.resolve(undefined); },
	has: function () { return Promise.resolve(false); }, keys: function () { return Promise.resolve([]); }, delete: function () { return Promise.resolve(false); } };

/* ------------------------------------------------------------------ */
/* Window */

var vpAccessors = {
	innerWidth: function () { return ns.viewport()[2]; },
	innerHeight: function () { return ns.viewport()[3]; },
	outerWidth: function () { return ns.viewport()[2]; },
	outerHeight: function () { return ns.viewport()[3]; },
	scrollX: function () { return ns.viewport()[0]; },
	scrollY: function () { return ns.viewport()[1]; },
	pageXOffset: function () { return ns.viewport()[0]; },
	pageYOffset: function () { return ns.viewport()[1]; },
	screenX: function () { return 0; }, screenY: function () { return 0; },
	screenLeft: function () { return 0; }, screenTop: function () { return 0; }
};
Object.keys(vpAccessors).forEach(function (k) { def(global, k, { get: vpAccessors[k], set: function () {}, enumerable: true }); });
global.devicePixelRatio = 1;
global.screen = {
	get width() { return ns.viewport()[2]; }, get height() { return ns.viewport()[3]; },
	get availWidth() { return ns.viewport()[2]; }, get availHeight() { return ns.viewport()[3]; },
	colorDepth: 24, pixelDepth: 24,
	orientation: { type: 'landscape-primary', angle: 0, addEventListener: function () {}, removeEventListener: function () {} }
};
global.scrollTo = function (a, b) {
	var o = scrollOpts(a, b);
	ns.scrollTo(o[0] === undefined ? scrollX : o[0], o[1] === undefined ? scrollY : o[1]);
};
global.scroll = global.scrollTo;
global.scrollBy = function (a, b) {
	var o = scrollOpts(a, b);
	ns.scrollTo(scrollX + (o[0] || 0), scrollY + (o[1] || 0));
};
global.focus = global.blur = global.print = global.stop = function () {};
global.close = function () {};
global.open = function (url) { if (url) location.href = url; return null; };
global.confirm = function () { return false; };
global.prompt = function () { return null; };
global.frames = global;
global.length = 0;
global.opener = null;
global.frameElement = null;
global.closed = false;
global.name = '';
global.status = '';
global.isSecureContext = true;
global.crossOriginIsolated = false;
global.origin = (function () { try { return new URL(ns.baseURL()).origin; } catch (e) { return 'null'; } })();

global.queueMicrotask = function (f) { Promise.resolve().then(function () { try { f(); } catch (e) { reportError(e); } }); };
global.requestIdleCallback = function (f) {
	var start = Date.now();
	return setTimeout(function () { f({ didTimeout: false, timeRemaining: function () { return Math.max(0, 50 - (Date.now() - start)); } }); }, 1);
};
global.cancelIdleCallback = function (id) { clearTimeout(id); };
var rafStart = ns.now();
global.requestAnimationFrame = function (f) {
	return setTimeout(function () { f(ns.now() - rafStart); }, 16);
};
global.cancelAnimationFrame = function (id) { clearTimeout(id); };

global.performance = {
	timeOrigin: Date.now(),
	now: function () { return ns.now() - rafStart; },
	mark: function (n) { return { name: n, startTime: this.now(), duration: 0, entryType: 'mark' }; },
	measure: function (n) { return { name: n, startTime: 0, duration: 0, entryType: 'measure' }; },
	clearMarks: function () {}, clearMeasures: function () {}, clearResourceTimings: function () {},
	getEntries: function () { return []; }, getEntriesByName: function () { return []; },
	getEntriesByType: function () { return []; }, setResourceTimingBufferSize: function () {},
	timing: { navigationStart: Date.now(), domContentLoadedEventEnd: 0, loadEventEnd: 0 },
	navigation: { type: 0, redirectCount: 0 },
	memory: { jsHeapSizeLimit: 0, totalJSHeapSize: 0, usedJSHeapSize: 0 },
	toJSON: function () { return {}; }
};

function MediaQueryList(q) {
	EventTarget.call(this);
	this.media = q;
	this.onchange = null;
}
MediaQueryList.prototype = Object.create(EventTarget.prototype);
Object.defineProperty(MediaQueryList.prototype, 'matches', { get: function () { return ns.mediaMatches(this.media); } });
MediaQueryList.prototype.addListener = function () {};
MediaQueryList.prototype.removeListener = function () {};
global.MediaQueryList = MediaQueryList;
global.matchMedia = function (q) { return new MediaQueryList(toStr(q)); };

global.getComputedStyle = function (el, pseudo) {
	var c = (el && ns.computed(el)) || { display: 'none' };
	var props = {};
	Object.keys(c).forEach(function (k) {
		var kebab = k.indexOf('-') >= 0 ? k : camelToKebab(k);
		props[kebab] = c[k];
	});
	var inline = el && el.style;
	var api = {
		getPropertyValue: function (n) {
			n = String(n);
			if (hasOwn(props, n)) return props[n];
			var v = inline ? inline.getPropertyValue(n) : '';
			return v || '';
		},
		getPropertyPriority: function () { return ''; },
		setProperty: function () { throw DOMException('read-only', 'NoModificationAllowedError'); },
		removeProperty: function () { throw DOMException('read-only', 'NoModificationAllowedError'); },
		item: function (i) { return Object.keys(props)[i] || ''; }
	};
	return new Proxy(Object.create(CSSStyleDeclaration.prototype), {
		get: function (t, k) {
			if (hasOwn(api, k)) return api[k];
			if (k === 'length') return Object.keys(props).length;
			if (k === 'cssText') return '';
			if (typeof k !== 'string') return undefined;
			var kebab = k.slice(0, 2) === '--' ? k : camelToKebab(k);
			if (kebab === 'margin' || kebab === 'padding') {
				return ['top', 'right', 'bottom', 'left'].map(function (s) { return props[kebab + '-' + s] || '0px'; }).join(' ');
			}
			return api.getPropertyValue(kebab);
		}
	});
};

/* location: settable components navigate */
(function () {
	var loc = global.__ns_location;
	if (!loc) return;
	function part(k) { return Object.getOwnPropertyDescriptor(loc, k); }
	function setter(k, fix) {
		var d = part(k) || Object.getOwnPropertyDescriptor(Object.getPrototypeOf(loc), k);
		var get = d && d.get ? d.get : function () { try { return new URL(loc.href)[k]; } catch (e) { return ''; } };
		def(loc, k, { get: get, set: function (v) {
			var u = new URL(loc.href);
			u[k] = fix ? fix(v) : v;
			if (k === 'hash') {
				var oldURL = loc.href;
				loc.href = u.href;
				setTimeout(function () { global.dispatchEvent(new HashChangeEvent('hashchange', { oldURL: oldURL, newURL: u.href })); }, 0);
			} else loc.href = u.href;
		}, enumerable: true });
	}
	['hash', 'search', 'pathname', 'host', 'hostname', 'port', 'protocol'].forEach(function (k) { setter(k); });
	def(loc, 'origin', { get: function () { try { return new URL(loc.href).origin; } catch (e) { return 'null'; } }, enumerable: true });
	def(loc, 'ancestorOrigins', { value: [] });
	method(loc, 'toString', function () { return loc.href; });
	method(loc, 'valueOf', function () { return loc; });
})();

/* history: state kept in-page; the URL bar is not updated */
var historyState = null;
global.history = {
	get length() { return 1; },
	get state() { return historyState; },
	scrollRestoration: 'auto',
	pushState: function (state, title, url) { historyState = state === undefined ? null : state; if (url) this._url(url); },
	replaceState: function (state, title, url) { historyState = state === undefined ? null : state; if (url) this._url(url); },
	back: function () {}, forward: function () {},
	go: function (n) { if (!n) location.reload(); },
	_url: function (url) {
		try {
			var u = new URL(toStr(url), location.href);
			pageURL = u.href;
		} catch (e) {}
	}
};
var pageURL = null;

/* navigator extras */
var nav = global.navigator;
nav.languages = ['en-US', 'en'];
nav.language = 'en-US';
nav.cookieEnabled = true;
nav.maxTouchPoints = 1;
nav.hardwareConcurrency = 4;
nav.deviceMemory = 4;
nav.vendor = '';
nav.product = 'Gecko';
nav.webdriver = false;
nav.doNotTrack = null;
nav.pdfViewerEnabled = false;
nav.plugins = [];
nav.mimeTypes = [];
nav.javaEnabled = function () { return false; };
nav.connection = { effectiveType: '4g', downlink: 10, rtt: 50, saveData: false, addEventListener: function () {} };
nav.clipboard = { writeText: function () { return Promise.resolve(); }, readText: function () { return Promise.resolve(''); } };
nav.permissions = { query: function () { return Promise.resolve({ state: 'denied', addEventListener: function () {} }); } };
nav.serviceWorker = { register: function () { return Promise.reject(new Error('not supported')); },
	getRegistrations: function () { return Promise.resolve([]); }, ready: new Promise(function () {}),
	addEventListener: function () {}, controller: null };
nav.mediaDevices = { getUserMedia: function () { return Promise.reject(new DOMException('not supported', 'NotSupportedError')); },
	enumerateDevices: function () { return Promise.resolve([]); } };
nav.geolocation = { getCurrentPosition: function (s, e) { if (e) setTimeout(function () { e({ code: 1, message: 'denied' }); }, 0); },
	watchPosition: function () { return 0; }, clearWatch: function () {} };
nav.userAgentData = { brands: [{ brand: 'NetSurf', version: '3' }], mobile: false, platform: 'Nintendo Switch',
	getHighEntropyValues: function () { return Promise.resolve({}); } };
nav.share = function () { return Promise.reject(new DOMException('not supported', 'NotAllowedError')); };
nav.canShare = function () { return false; };
nav.vibrate = function () { return false; };
nav.getGamepads = function () { return []; };
nav.storage = { estimate: function () { return Promise.resolve({ quota: 0, usage: 0 }); }, persist: function () { return Promise.resolve(false); } };
nav.locks = { request: function (n, o, f) { f = typeof o === 'function' ? o : f; return Promise.resolve().then(function () { return f({ name: n }); }); } };

global.crypto = {
	getRandomValues: function (arr) {
		for (var i = 0; i < arr.length; i++) arr[i] = Math.floor(Math.random() * 4294967296);
		return arr;
	},
	randomUUID: function () {
		var b = crypto.getRandomValues(new Uint8Array(16));
		b[6] = (b[6] & 15) | 64; b[8] = (b[8] & 63) | 128;
		var h = Array.prototype.map.call(b, function (x) { return (x + 256).toString(16).slice(1); }).join('');
		return h.slice(0, 8) + '-' + h.slice(8, 12) + '-' + h.slice(12, 16) + '-' + h.slice(16, 20) + '-' + h.slice(20);
	},
	subtle: {}
};
global.structuredClone = function clone(v, seen) {
	seen = seen || new Map();
	if (v === null || typeof v !== 'object') return v;
	if (seen.has(v)) return seen.get(v);
	var out;
	if (v instanceof Date) return new Date(v.getTime());
	if (v instanceof RegExp) return new RegExp(v.source, v.flags);
	if (v instanceof ArrayBuffer) return v.slice(0);
	if (ArrayBuffer.isView(v)) return new v.constructor(v);
	if (v instanceof Map) { out = new Map(); seen.set(v, out); v.forEach(function (x, k) { out.set(clone(k, seen), clone(x, seen)); }); return out; }
	if (v instanceof Set) { out = new Set(); seen.set(v, out); v.forEach(function (x) { out.add(clone(x, seen)); }); return out; }
	if (typeof v.nodeType === 'number') throw DOMException('node cannot be cloned', 'DataCloneError');
	out = Array.isArray(v) ? [] : {};
	seen.set(v, out);
	Object.keys(v).forEach(function (k) { out[k] = clone(v[k], seen); });
	return out;
};

/* postMessage to self */
global.postMessage = function (data, origin) {
	setTimeout(function () { global.dispatchEvent(new MessageEvent('message', { data: data, origin: global.origin, source: global })); }, 0);
};
function MessageChannel() {
	var a = new EventTarget(), b = new EventTarget();
	[[a, b], [b, a]].forEach(function (p) {
		p[0].postMessage = function (d) { setTimeout(function () {
			var ev = new MessageEvent('message', { data: d });
			if (typeof p[1].onmessage === 'function') p[1].onmessage(ev);
			p[1].dispatchEvent(ev);
		}, 0); };
		p[0].start = p[0].close = function () {};
	});
	this.port1 = a; this.port2 = b;
}
global.MessageChannel = MessageChannel;
global.BroadcastChannel = function (name) { var c = new EventTarget(); c.name = name; c.postMessage = c.close = function () {}; return c; };
global.Worker = function () { throw new Error('Web Workers are not supported'); };
global.SharedWorker = global.Worker;

/* Custom elements: definitions are recorded; elements are not upgraded */
var ceRegistry = {};
global.customElements = {
	define: function (name, cls) { ceRegistry[name] = cls; },
	get: function (name) { return ceRegistry[name]; },
	getName: function (cls) { for (var k in ceRegistry) if (ceRegistry[k] === cls) return k; return null; },
	whenDefined: function (name) { return ceRegistry[name] ? Promise.resolve(ceRegistry[name]) : new Promise(function () {}); },
	upgrade: function () {}
};

/* HTMLxxxElement interface objects for instanceof checks */
var TAG_IFACES = {
	HTMLAnchorElement: 'A', HTMLAreaElement: 'AREA', HTMLBodyElement: 'BODY', HTMLBRElement: 'BR',
	HTMLButtonElement: 'BUTTON', HTMLCanvasElement: 'CANVAS', HTMLDataElement: 'DATA',
	HTMLDataListElement: 'DATALIST', HTMLDetailsElement: 'DETAILS', HTMLDialogElement: 'DIALOG',
	HTMLDivElement: 'DIV', HTMLDListElement: 'DL', HTMLEmbedElement: 'EMBED',
	HTMLFieldSetElement: 'FIELDSET', HTMLFormElement: 'FORM', HTMLHeadElement: 'HEAD',
	HTMLHeadingElement: 'H1|H2|H3|H4|H5|H6', HTMLHRElement: 'HR', HTMLHtmlElement: 'HTML',
	HTMLIFrameElement: 'IFRAME', HTMLImageElement: 'IMG', HTMLInputElement: 'INPUT',
	HTMLLabelElement: 'LABEL', HTMLLegendElement: 'LEGEND', HTMLLIElement: 'LI', HTMLLinkElement: 'LINK',
	HTMLMapElement: 'MAP', HTMLMetaElement: 'META', HTMLMeterElement: 'METER', HTMLModElement: 'INS|DEL',
	HTMLObjectElement: 'OBJECT', HTMLOListElement: 'OL', HTMLOptGroupElement: 'OPTGROUP',
	HTMLOptionElement: 'OPTION', HTMLOutputElement: 'OUTPUT', HTMLParagraphElement: 'P',
	HTMLPictureElement: 'PICTURE', HTMLPreElement: 'PRE', HTMLProgressElement: 'PROGRESS',
	HTMLQuoteElement: 'Q|BLOCKQUOTE', HTMLScriptElement: 'SCRIPT', HTMLSelectElement: 'SELECT',
	HTMLSlotElement: 'SLOT', HTMLSourceElement: 'SOURCE', HTMLSpanElement: 'SPAN', HTMLStyleElement: 'STYLE',
	HTMLTableElement: 'TABLE', HTMLTableCaptionElement: 'CAPTION', HTMLTableCellElement: 'TD|TH',
	HTMLTableColElement: 'COL|COLGROUP', HTMLTableRowElement: 'TR', HTMLTableSectionElement: 'THEAD|TBODY|TFOOT',
	HTMLTemplateElement: 'TEMPLATE', HTMLTextAreaElement: 'TEXTAREA', HTMLTimeElement: 'TIME',
	HTMLTitleElement: 'TITLE', HTMLTrackElement: 'TRACK', HTMLUListElement: 'UL', HTMLUnknownElement: '',
	SVGElement: 'SVG|PATH|G|CIRCLE|RECT|LINE|POLYGON|POLYLINE|TEXT|USE|DEFS|SYMBOL',
	SVGSVGElement: 'SVG'
};
Object.keys(TAG_IFACES).forEach(function (name) {
	var re = new RegExp('^(' + TAG_IFACES[name] + ')$', 'i');
	var C = function () { throw new TypeError('Illegal constructor'); };
	C.prototype = ElementP;
	def(C, Symbol.hasInstance, { value: function (o) { return isElement(o) && (!TAG_IFACES[name] || re.test(o.tagName)); } });
	def(C, 'name', { value: name });
	global[name] = C;
});
[['Comment', 8], ['DocumentFragment', 11], ['DocumentType', 10], ['ProcessingInstruction', 7], ['Attr', 2], ['CDATASection', 4]].forEach(function (p) {
	var C = function () { throw new TypeError('Illegal constructor'); };
	def(C, Symbol.hasInstance, { value: function (o) { return !!o && o.nodeType === p[1]; } });
	global[p[0]] = C;
});
global.ShadowRoot = function () {};
global.Range = function () {};
global.Selection = function () {};
global.NamedNodeMap = function () {};
global.Image = function (w, h) {
	var img = document.createElement('img');
	if (w !== undefined) img.width = w;
	if (h !== undefined) img.height = h;
	return img;
};
global.Option = function (text, value, dflt, sel) {
	var o = document.createElement('option');
	if (text !== undefined) o.textContent = text;
	if (value !== undefined) o.setAttribute('value', value);
	if (dflt || sel) o.setAttribute('selected', '');
	return o;
};

/* Intl (QuickJS-ng has none): enough for formatting not to throw */
if (typeof global.Intl === 'undefined') {
	var Intl = {};
	Intl.NumberFormat = function (loc, opts) { this._o = opts || {}; };
	Intl.NumberFormat.prototype.format = function (n) {
		var o = this._o;
		var d = o.maximumFractionDigits !== undefined ? o.maximumFractionDigits : (o.style === 'percent' ? 0 : 3);
		var v = o.style === 'percent' ? n * 100 : n;
		var s = (+v).toFixed(Math.min(20, Math.max(0, o.minimumFractionDigits !== undefined ? Math.max(o.minimumFractionDigits, 0) : 0)));
		if (o.minimumFractionDigits === undefined) s = String(Math.round(v * Math.pow(10, d)) / Math.pow(10, d));
		var parts = s.split('.');
		if (o.useGrouping !== false) parts[0] = parts[0].replace(/\B(?=(\d{3})+(?!\d))/g, ',');
		s = parts.join('.');
		if (o.style === 'currency') s = ({ USD: '$', EUR: '\u20ac', GBP: '\u00a3', JPY: '\u00a5' }[o.currency] || (o.currency + ' ')) + s;
		if (o.style === 'percent') s += '%';
		return s;
	};
	Intl.NumberFormat.prototype.formatToParts = function (n) { return [{ type: 'integer', value: this.format(n) }]; };
	Intl.NumberFormat.prototype.resolvedOptions = function () { return { locale: 'en-US', numberingSystem: 'latn' }; };
	Intl.NumberFormat.supportedLocalesOf = function () { return ['en-US']; };
	Intl.DateTimeFormat = function (loc, opts) { this._o = opts || {}; };
	Intl.DateTimeFormat.prototype.format = function (d) {
		d = d === undefined ? new Date() : new Date(d);
		var o = this._o;
		if (o.timeStyle || o.hour) return d.toLocaleTimeString ? d.toLocaleTimeString() : d.toTimeString().slice(0, 8);
		var months = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];
		if (o.month === 'long' || o.month === 'short' || o.dateStyle === 'long' || o.dateStyle === 'medium') {
			var m = months[d.getMonth()];
			if (o.month === 'short' || o.dateStyle === 'medium') m = m.slice(0, 3);
			return m + ' ' + d.getDate() + ', ' + d.getFullYear();
		}
		return (d.getMonth() + 1) + '/' + d.getDate() + '/' + d.getFullYear();
	};
	Intl.DateTimeFormat.prototype.formatToParts = function (d) { return [{ type: 'literal', value: this.format(d) }]; };
	Intl.DateTimeFormat.prototype.resolvedOptions = function () { return { locale: 'en-US', timeZone: 'UTC', calendar: 'gregory', numberingSystem: 'latn' }; };
	Intl.DateTimeFormat.supportedLocalesOf = function () { return ['en-US']; };
	Intl.Collator = function () {};
	Intl.Collator.prototype.compare = function (a, b) { return a < b ? -1 : a > b ? 1 : 0; };
	Intl.Collator.prototype.resolvedOptions = function () { return { locale: 'en-US' }; };
	Intl.PluralRules = function () {};
	Intl.PluralRules.prototype.select = function (n) { return n === 1 ? 'one' : 'other'; };
	Intl.PluralRules.prototype.resolvedOptions = function () { return { locale: 'en-US', pluralCategories: ['one', 'other'] }; };
	Intl.RelativeTimeFormat = function () {};
	Intl.RelativeTimeFormat.prototype.format = function (v, u) {
		var a = Math.abs(v), unit = a === 1 ? u.replace(/s$/, '') : u.replace(/s?$/, 's');
		return v < 0 ? a + ' ' + unit + ' ago' : 'in ' + a + ' ' + unit;
	};
	Intl.ListFormat = function () {};
	Intl.ListFormat.prototype.format = function (l) { l = Array.from(l); return l.length < 2 ? l.join('') : l.slice(0, -1).join(', ') + ' and ' + l[l.length - 1]; };
	Intl.Segmenter = function () {};
	Intl.Segmenter.prototype.segment = function (s) { return Array.from(s).map(function (c, i) { return { segment: c, index: i, input: s }; }); };
	Intl.DisplayNames = function () {};
	Intl.DisplayNames.prototype.of = function (c) { return c; };
	Intl.getCanonicalLocales = function (l) { return l ? [].concat(l) : []; };
	Intl.supportedValuesOf = function () { return []; };
	global.Intl = Intl;
	if (!Number.prototype.toLocaleString || (1234).toLocaleString() === '1234')
		Number.prototype.toLocaleString = function (loc, opts) { return new Intl.NumberFormat(loc, opts).format(this); };
}

/* ------------------------------------------------------------------ */
/* Document */

accessor(DocP, 'activeElement', function () { return activeElement && activeElement.isConnected ? activeElement : this.body; });
method(DocP, 'hasFocus', function () { return true; });
accessor(DocP, 'visibilityState', function () { return 'visible'; });
accessor(DocP, 'hidden', function () { return false; });
accessor(DocP, 'characterSet', function () { return 'UTF-8'; });
accessor(DocP, 'charset', function () { return 'UTF-8'; });
accessor(DocP, 'inputEncoding', function () { return 'UTF-8'; });
accessor(DocP, 'contentType', function () { return 'text/html'; });
accessor(DocP, 'compatMode', function () { return 'CSS1Compat'; });
accessor(DocP, 'referrer', function () { return ''; });
accessor(DocP, 'lastModified', function () { return new Date().toLocaleString(); });
accessor(DocP, 'domain', function () { try { return new URL(ns.baseURL()).hostname; } catch (e) { return ''; } });
accessor(DocP, 'baseURI', function () { return ns.baseURL(); });
accessor(NodeP, 'baseURI', function () { return ns.baseURL(); });
accessor(DocP, 'scrollingElement', function () { return this.documentElement; });
accessor(DocP, 'fullscreenElement', function () { return null; });
accessor(DocP, 'fullscreenEnabled', function () { return false; });
accessor(DocP, 'pointerLockElement', function () { return null; });
accessor(DocP, 'currentScript', function () { return currentScript; });
accessor(DocP, 'forms', function () { return NodeList(qsa(this, 'form')); });
accessor(DocP, 'images', function () { return NodeList(qsa(this, 'img')); });
accessor(DocP, 'links', function () { return NodeList(qsa(this, 'a[href],area[href]')); });
accessor(DocP, 'scripts', function () { return NodeList(qsa(this, 'script')); });
accessor(DocP, 'embeds', function () { return NodeList(qsa(this, 'embed')); });
accessor(DocP, 'plugins', function () { return NodeList([]); });
accessor(DocP, 'anchors', function () { return NodeList(qsa(this, 'a[name]')); });
accessor(DocP, 'styleSheets', function () { return NodeList(qsa(this, 'style,link[rel~=stylesheet]').map(function (n) {
	return { ownerNode: n, href: n.href || null, cssRules: [], rules: [], disabled: false, media: [],
		insertRule: function () { return 0; }, deleteRule: function () {}, addRule: function () {} };
})); });
accessor(DocP, 'fonts', function () {
	return { ready: Promise.resolve(), status: 'loaded', check: function () { return true; },
		load: function () { return Promise.resolve([]); }, add: function () {}, delete: function () {},
		forEach: function () {}, addEventListener: function () {}, removeEventListener: function () {}, size: 0 };
});
accessor(DocP, 'dir', function () { return this.documentElement ? this.documentElement.dir : ''; });
accessor(DocP, 'designMode', function () { return 'off'; });
accessor(DocP, 'implementation', function () {
	return { hasFeature: function () { return true; },
		createHTMLDocument: function (t) { var d = ns.parseDocument('<!DOCTYPE html><html><head><title>' + escapeText(toStr(t)) + '</title></head><body></body></html>'); return d; },
		createDocument: function () { return ns.parseDocument(''); },
		createDocumentType: function (n) { return { nodeType: 10, name: n }; } };
});
method(DocP, 'createElementNS', function (nsURI, qname) { return this.createElement(String(qname).replace(/^.*:/, '')); });
method(DocP, 'importNode', function (n, deep) { return n.cloneNode(!!deep); });
method(DocP, 'adoptNode', function (n) { if (n.parentNode) n.parentNode.removeChild(n); return n; });
method(DocP, 'elementFromPoint', function () { return null; });
method(DocP, 'elementsFromPoint', function () { return []; });
method(DocP, 'caretRangeFromPoint', function () { return null; });
method(DocP, 'getSelection', function () { return global.getSelection(); });
method(DocP, 'execCommand', function () { return false; });
method(DocP, 'queryCommandSupported', function () { return false; });
method(DocP, 'exitFullscreen', function () { return Promise.resolve(); });
method(DocP, 'exitPointerLock', function () {});
method(DocP, 'open', function () { return this; });
method(DocP, 'close', function () {});
method(DocP, 'createAttribute', function (n) { return { name: n, value: '', nodeType: 2 }; });
method(DocP, 'createCDATASection', function (t) { return this.createTextNode(t); });
method(DocP, 'createProcessingInstruction', function () { return this.createComment(''); });
method(DocP, 'startViewTransition', function (cb) {
	var p = Promise.resolve().then(function () { if (cb) return cb(); });
	return { finished: p, ready: p, updateCallbackDone: p, skipTransition: function () {} };
});
function makeRange(doc) {
	var r = {
		startContainer: doc, startOffset: 0, endContainer: doc, endOffset: 0, collapsed: true,
		commonAncestorContainer: doc,
		setStart: function (n, o) { this.startContainer = n; this.startOffset = o; },
		setEnd: function (n, o) { this.endContainer = n; this.endOffset = o; },
		setStartBefore: function () {}, setStartAfter: function () {}, setEndBefore: function () {}, setEndAfter: function () {},
		selectNode: function (n) { this.startContainer = this.endContainer = n; },
		selectNodeContents: function (n) { this.startContainer = this.endContainer = n; },
		collapse: function () {}, detach: function () {}, cloneRange: function () { return makeRange(doc); },
		deleteContents: function () {}, extractContents: function () { return doc.createDocumentFragment(); },
		cloneContents: function () { return doc.createDocumentFragment(); },
		insertNode: function (n) { if (this.startContainer && this.startContainer.nodeType === 1) this.startContainer.insertBefore(n, this.startContainer.firstChild); },
		surroundContents: function () {},
		createContextualFragment: function (html) { return fragmentFromHTML(doc.body || doc.documentElement, html); },
		getBoundingClientRect: function () { return new DOMRect(); },
		getClientRects: function () { return []; },
		toString: function () { return ''; }
	};
	return r;
}
method(DocP, 'createRange', function () { return makeRange(this); });
global.getSelection = function () {
	return { rangeCount: 0, isCollapsed: true, type: 'None', anchorNode: null, focusNode: null,
		addRange: function () {}, removeAllRanges: function () {}, removeRange: function () {},
		getRangeAt: function () { return makeRange(document); }, collapse: function () {},
		selectAllChildren: function () {}, empty: function () {}, toString: function () { return ''; } };
};
function makeWalker(root, whatToShow, filter, elementsOnly) {
	var nodes = [];
	(function walk(n) {
		for (var c = n.firstChild; c; c = c.nextSibling) {
			var t = c.nodeType;
			var show = whatToShow === undefined || whatToShow === 0xFFFFFFFF ||
				(whatToShow & (1 << (t - 1)));
			var ok = show;
			if (ok && filter) {
				var f = typeof filter === 'function' ? filter : filter.acceptNode;
				var res = f ? f.call(filter, c) : 1;
				ok = res === 1;
				if (res === 2) continue;
			}
			if (ok) nodes.push(c);
			walk(c);
		}
	})(root);
	var i = -1;
	return {
		root: root, whatToShow: whatToShow, filter: filter, currentNode: root,
		nextNode: function () { if (i + 1 >= nodes.length) return null; this.currentNode = nodes[++i]; return this.currentNode; },
		previousNode: function () { if (i <= 0) return null; this.currentNode = nodes[--i]; return this.currentNode; },
		firstChild: function () { return this.nextNode(); },
		parentNode: function () { var p = this.currentNode.parentNode; if (p && p !== root) { this.currentNode = p; i = nodes.indexOf(p); return p; } return null; },
		nextSibling: function () { var s = this.currentNode.nextSibling; while (s && nodes.indexOf(s) < 0) s = s.nextSibling; if (s) { this.currentNode = s; i = nodes.indexOf(s); } return s; },
		detach: function () {}
	};
}
method(DocP, 'createTreeWalker', function (root, what, filter) { return makeWalker(root, what, filter); });
method(DocP, 'createNodeIterator', function (root, what, filter) { return makeWalker(root, what, filter); });
global.NodeFilter = { FILTER_ACCEPT: 1, FILTER_REJECT: 2, FILTER_SKIP: 3, SHOW_ALL: 0xFFFFFFFF,
	SHOW_ELEMENT: 1, SHOW_ATTRIBUTE: 2, SHOW_TEXT: 4, SHOW_CDATA_SECTION: 8, SHOW_COMMENT: 128,
	SHOW_DOCUMENT: 256, SHOW_DOCUMENT_TYPE: 512, SHOW_DOCUMENT_FRAGMENT: 1024 };

function DOMParser() {}
DOMParser.prototype.parseFromString = function (s, type) { return ns.parseDocument(toStr(s)); };
global.DOMParser = DOMParser;
global.XMLSerializer = function () {};
global.XMLSerializer.prototype.serializeToString = function (n) { return serialize(n); };

var currentScript = null;

/* ------------------------------------------------------------------ */
/* Misc globals */

if (typeof global.console === 'object') {
	var c = global.console;
	['trace', 'dir', 'dirxml', 'table', 'group', 'groupCollapsed', 'groupEnd', 'count',
	 'countReset', 'assert', 'time', 'timeLog', 'timeEnd', 'timeStamp', 'profile', 'profileEnd', 'clear']
		.forEach(function (k) { if (!c[k]) c[k] = function () {}; });
	var origAssert = c.assert;
	c.assert = function (cond) { if (!cond) c.error('Assertion failed: ' + Array.prototype.slice.call(arguments, 1).join(' ')); };
}
global.globalThis = global;
global.onerror = null;
global.onunhandledrejection = null;
global.trustedTypes = { createPolicy: function (n, rules) { return {
	createHTML: function (s) { return rules && rules.createHTML ? rules.createHTML(s) : s; },
	createScript: function (s) { return rules && rules.createScript ? rules.createScript(s) : s; },
	createScriptURL: function (s) { return rules && rules.createScriptURL ? rules.createScriptURL(s) : s; } }; },
	isHTML: function () { return false; }, isScript: function () { return false; }, isScriptURL: function () { return false; },
	emptyHTML: '', emptyScript: '', defaultPolicy: null };
global.CSS = {
	supports: function (a, b) {
		var text = b === undefined ? a : '(' + a + ':' + b + ')';
		return /grid|flex|var|calc|color|radius|shadow|gap|transform|opacity|aspect-ratio|position:\s*sticky/i.test(text);
	},
	escape: function (s) { return toStr(s).replace(/([^\w-])/g, '\\$1').replace(/^(\d)/, '\\3$1 '); },
	registerProperty: function () {},
	px: function (n) { return n + 'px'; }
};
global.CSSStyleSheet = function () { this.cssRules = []; this.replaceSync = this.replace = function () { return Promise.resolve(this); }; this.insertRule = function () { return 0; }; };
global.visualViewport = new EventTarget();
Object.defineProperties(global.visualViewport, {
	width: { get: function () { return innerWidth; } }, height: { get: function () { return innerHeight; } },
	offsetLeft: { value: 0 }, offsetTop: { value: 0 }, pageLeft: { get: function () { return scrollX; } },
	pageTop: { get: function () { return scrollY; } }, scale: { value: 1 }
});
global.speechSynthesis = { speak: function () {}, cancel: function () {}, getVoices: function () { return []; }, addEventListener: function () {} };
global.Notification = function () {};
global.Notification.permission = 'denied';
global.Notification.requestPermission = function () { return Promise.resolve('denied'); };
global.AudioContext = global.webkitAudioContext = undefined;

/* unhandled promise rejections are reported, not fatal */
global.addEventListener('unhandledrejection', function () {});

})(globalThis);
