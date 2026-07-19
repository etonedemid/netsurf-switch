/* Polyfiller for Duktape for NetSurf
 *
 * This JavaScript will be loaded into heaps before the generics
 *
 * We only care for the side-effects of this, be careful.
 */

// Production steps of ECMA-262, Edition 6, 22.1.2.1
if (!Array.from) {
  Array.from = (function () {
    var toStr = Object.prototype.toString;
    var isCallable = function (fn) {
      return typeof fn === 'function' || toStr.call(fn) === '[object Function]';
    };
    var toInteger = function (value) {
      var number = Number(value);
      if (isNaN(number)) { return 0; }
      if (number === 0 || !isFinite(number)) { return number; }
      return (number > 0 ? 1 : -1) * Math.floor(Math.abs(number));
    };
    var maxSafeInteger = Math.pow(2, 53) - 1;
    var toLength = function (value) {
      var len = toInteger(value);
      return Math.min(Math.max(len, 0), maxSafeInteger);
    };

    // The length property of the from method is 1.
    return function from(arrayLike/*, mapFn, thisArg */) {
      // 1. Let C be the this value.
      var C = this;

      // 2. Let items be ToObject(arrayLike).
      var items = Object(arrayLike);

      // 3. ReturnIfAbrupt(items).
      if (arrayLike == null) {
        throw new TypeError('Array.from requires an array-like object - not null or undefined');
      }

      // 4. If mapfn is undefined, then let mapping be false.
      var mapFn = arguments.length > 1 ? arguments[1] : void undefined;
      var T;
      if (typeof mapFn !== 'undefined') {
        // 5. else
        // 5. a If IsCallable(mapfn) is false, throw a TypeError exception.
        if (!isCallable(mapFn)) {
          throw new TypeError('Array.from: when provided, the second argument must be a function');
        }

        // 5. b. If thisArg was supplied, let T be thisArg; else let T be undefined.
        if (arguments.length > 2) {
          T = arguments[2];
        }
      }

      // 10. Let lenValue be Get(items, "length").
      // 11. Let len be ToLength(lenValue).
      var len = toLength(items.length);

      // 13. If IsConstructor(C) is true, then
      // 13. a. Let A be the result of calling the [[Construct]] internal method 
      // of C with an argument list containing the single item len.
      // 14. a. Else, Let A be ArrayCreate(len).
      var A = isCallable(C) ? Object(new C(len)) : new Array(len);

      // 16. Let k be 0.
      var k = 0;
      // 17. Repeat, while k < len... (also steps a - h)
      var kValue;
      while (k < len) {
        kValue = items[k];
        if (mapFn) {
          A[k] = typeof T === 'undefined' ? mapFn(kValue, k) : mapFn.call(T, kValue, k);
        } else {
          A[k] = kValue;
        }
        k += 1;
      }
      // 18. Let putStatus be Put(A, "length", len, true).
      A.length = len;
      // 20. Return A.
      return A;
    };
  }());
}

// DOMTokenList formatter, in theory we can remove this if we do the stringifier IDL support

DOMTokenList.prototype.toString = function () {
  if (this.length == 0) {
    return "";
  }

  var ret = this.item(0);
  for (var index = 1; index < this.length; index++) {
    ret = ret + " " + this.item(index);
  }

  return ret;
}

// Inherit the same toString for settable lists
DOMSettableTokenList.prototype.toString = DOMTokenList.prototype.toString;
// ---- NetSurf Switch port additions ----

// globalThis (ES2020)
if (typeof globalThis === "undefined") {
	(function () {
		if (typeof self !== "undefined") { self.globalThis = self; }
		else if (typeof window !== "undefined") { window.globalThis = window; }
		else { this.globalThis = this; }
	})();
}

// Minimal Promise polyfill (ES5, then/catch/finally, resolve/reject/all/race)
if (typeof Promise === "undefined") {
	(function (global) {
		function isFn(f) { return typeof f === "function"; }
		function Prom(fn) {
			var self = this;
			self._state = 0; self._value = undefined; self._cbs = [];
			function settle(state, value) {
				if (self._state !== 0) { return; }
				if (state === 1 && value && isFn(value.then)) {
					value.then(function (v) { settle(1, v); },
						   function (e) { settle(2, e); });
					return;
				}
				self._state = state; self._value = value;
				for (var i = 0; i < self._cbs.length; i++) {
					self._cbs[i]();
				}
				self._cbs = [];
			}
			try {
				fn(function (v) { settle(1, v); },
				   function (e) { settle(2, e); });
			} catch (e) { settle(2, e); }
		}
		Prom.prototype.then = function (onOk, onErr) {
			var self = this;
			return new Prom(function (resolve, reject) {
				function run() {
					try {
						if (self._state === 1) {
							if (isFn(onOk)) { resolve(onOk(self._value)); }
							else { resolve(self._value); }
						} else {
							if (isFn(onErr)) { resolve(onErr(self._value)); }
							else { reject(self._value); }
						}
					} catch (e) { reject(e); }
				}
				if (self._state === 0) { self._cbs.push(run); }
				else { run(); }
			});
		};
		Prom.prototype["catch"] = function (onErr) {
			return this.then(undefined, onErr);
		};
		Prom.prototype["finally"] = function (onDone) {
			return this.then(
				function (v) { onDone(); return v; },
				function (e) { onDone(); throw e; });
		};
		Prom.resolve = function (v) {
			return new Prom(function (res) { res(v); });
		};
		Prom.reject = function (e) {
			return new Prom(function (res, rej) { rej(e); });
		};
		Prom.all = function (arr) {
			return new Prom(function (resolve, reject) {
				var out = [], left = arr.length;
				if (left === 0) { resolve(out); return; }
				function one(i) {
					Prom.resolve(arr[i]).then(function (v) {
						out[i] = v;
						left -= 1;
						if (left === 0) { resolve(out); }
					}, reject);
				}
				for (var i = 0; i < arr.length; i++) { one(i); }
			});
		};
		Prom.race = function (arr) {
			return new Prom(function (resolve, reject) {
				for (var i = 0; i < arr.length; i++) {
					Prom.resolve(arr[i]).then(resolve, reject);
				}
			});
		};
		global.Promise = Prom;
	})(this);
}

// Image constructor shim
if (typeof Image === "undefined" && typeof document !== "undefined") {
	this.Image = function (w, h) {
		var img = document.createElement("img");
		if (w !== undefined) { img.width = w; }
		if (h !== undefined) { img.height = h; }
		return img;
	};
}

// queueMicrotask
if (typeof queueMicrotask === "undefined" && typeof setTimeout !== "undefined") {
	this.queueMicrotask = function (fn) { setTimeout(fn, 0); };
}
