globalThis.TextEncoder = class {
  encode(value = "") {
    const encoded = encodeURIComponent(String(value));
    const bytes = [];
    for (let i=0; i<encoded.length; i++) {
      if (encoded[i] === "%") {
        bytes.push(Number.parseInt(encoded.slice(i+1,i+3),16));
        i+=2;
      } else bytes.push(encoded.charCodeAt(i));
    }
    return Uint8Array.from(bytes);
  }
};
globalThis.TextDecoder = class {
  constructor(label = "utf-8") { this.label = label; }
  decode(bytes = new Uint8Array()) {
    return __decodeText(new Uint8Array(bytes.buffer ?? bytes, bytes.byteOffset ?? 0, bytes.byteLength), this.label);
  }
};
globalThis.crypto = Object.freeze({
  getRandomValues(array) {
    if (!(array instanceof Uint8Array) || array.length > 65536) throw new TypeError("Expected a byte array");
    array.set(new Uint8Array(__randomBytes(array.length)));
    return array;
  },
  randomUUID() {
    const data=this.getRandomValues(new Uint8Array(16));
    data[6]=(data[6]&15)|64;data[8]=(data[8]&63)|128;
    const hex=Array.from(data,n=>n.toString(16).padStart(2,"0")).join("");
    return [hex.slice(0,8),hex.slice(8,12),hex.slice(12,16),hex.slice(16,20),hex.slice(20)].join("-");
  }
});
(() => {
  const timers = new Map();
  let id = 0;
  globalThis.setTimeout = (callback, delay = 0, ...args) => {
    if (typeof callback !== "function") throw new TypeError("Expected a timer callback");
    if (timers.size >= 128) throw new RangeError("Too many pending timers");
    const key = ++id;
    timers.set(key, { callback, args, due: Date.now() + Math.max(0, Number(delay) || 0) });
    return key;
  };
  globalThis.clearTimeout = key => timers.delete(Number(key));
  const wait = globalThis.__waitTimer;
  delete globalThis.__waitTimer;
  globalThis.__runTimer = () => {
    let next;
    for (const entry of timers)
      if (!next || entry[1].due < next[1].due) next = entry;
    if (!next || !wait(next[1].due - Date.now())) return false;
    const [key, { callback, args }] = next;
    timers.delete(key);
    callback(...args);
    return true;
  };
})();
