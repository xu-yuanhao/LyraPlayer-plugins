(() => {
  const parse = (text) => { try { return JSON.parse(text); } catch { return text; } };
  const query = (url, values) => {
    const tail = Object.entries(values || {}).flatMap(([key, value]) =>
      (Array.isArray(value) ? value : [value]).filter(v => v != null)
        .map(v => encodeURIComponent(key) + "=" + encodeURIComponent(v))).join("&");
    return tail ? url + (url.includes("?") ? "&" : "?") + tail : url;
  };
  const request = (url, options = {}) => {
    const headers = { ...options.headers };
    let body = options.data ?? options.body;
    if (body != null && typeof body !== "string") {
      body = JSON.stringify(body);
      if (!Object.keys(headers).some(key => key.toLowerCase() === "content-type"))
        headers["Content-Type"] = "application/json";
    }
    return parse(__transport(query(url, options.params), options.method || "GET",
      JSON.stringify(headers), body || ""));
  };
  const axios = async (options, config = {}) => {
    if (typeof options === "string") options = { ...config, url: options };
    const response = request(options.url, options);
    if (response.status < 200 || response.status >= 300) throw new Error("HTTP " + response.status);
    return { status: response.status, data: options.responseType === "text" ? response.body : parse(response.body) };
  };
  axios.get = (url, config) => axios(url, config);
  axios.post = (url, data, config) => axios(url, { ...config, method: "POST", data });
  axios.request = axios;
  axios.create = defaults => {
    const call = (options, config) => typeof options === "string"
      ? axios(options, { ...defaults, ...config, headers: { ...defaults?.headers, ...config?.headers } })
      : axios({ ...defaults, ...options, headers: { ...defaults?.headers, ...options?.headers } });
    call.get = (url, config) => call(url, config);
    call.post = (url, data, config) => call(url, { ...config, method: "POST", data });
    call.request = call;
    return call;
  };
  const qs = { stringify: data => query("", data).slice(1) };
  const allowed = Object.freeze({ axios, qs });
  const require = name => {
    if (!Object.hasOwn(allowed, name)) throw new Error("Unsupported module: " + name);
    return allowed[name];
  };
  const console = Object.freeze({ log() {}, warn() {}, error() {}, info() {}, debug() {} });
  globalThis.__load = code => {
    let listener, sources;
    const events = { request: "request", inited: "inited", updateAlert: "updateAlert" };
    const lx = {
      EVENT_NAMES: events, env: "mobile", version: "2.0.0",
      on(name, fn) { if (name === events.request) listener = fn; },
      send(name, value) { if (name === events.inited) sources = value?.sources; },
      request(url, options, callback) {
        if (typeof options === "function") { callback = options; options = {}; }
        let response;
        try { response = request(url, options); }
        catch (error) { callback(error); return; }
        callback(null, { statusCode: response.status, body: parse(response.body) }, parse(response.body));
      },
    };
    globalThis.lx = lx;
    const module = { exports: {} };
    new Function("module", "exports", "require", "console", code)(module, module.exports, require, console);
    const source = module.exports?.default ?? module.exports;
    if (typeof listener === "function" && sources && typeof sources === "object")
      return { format: "lx", sources, listener };
    if (!source || typeof source.platform !== "string" ||
        !["search", "getMediaSource", "getTopLists", "getMusicSheetInfo"].some(key => typeof source[key] === "function"))
      throw new Error("Unsupported source script");
    return { format: "musicfree", source };
  };
  globalThis.__metadata = plugin => {
    if (plugin.format === "lx") return { format: "lx", capabilities: ["播放"], platforms: Object.keys(plugin.sources) };
    const source = plugin.source;
    const capabilities = [["search", "搜索"], ["getMediaSource", "播放"], ["getTopLists", "歌单"], ["getLyric", "歌词"]]
      .filter(([method]) => typeof source[method] === "function").map(([, label]) => label);
    return { format: "musicfree", name: source.platform, version: source.version,
      description: source.description, update: source.srcUrl, icon: source.icon, capabilities };
  };
  globalThis.__invoke = async (plugin, method, args) => {
    if (plugin.format === "lx") {
      if (method !== "resolve") return null;
      const [platform, song] = args;
      const source = plugin.sources[platform];
      if (!source?.actions?.includes("musicUrl")) return null;
      return { url: await plugin.listener({ source: platform, action: "musicUrl",
        info: { type: platform === "local" ? null : source.qualitys?.[0] ?? "128k", musicInfo: song } }) };
    }
    const source = plugin.source;
    if (method === "search") return typeof source.search === "function" ? source.search(args[0], 1, "music") : null;
    if (method === "resolve") return typeof source.getMediaSource === "function" ? source.getMediaSource(args[1], "standard") : null;
    if (method === "collections") return typeof source.getTopLists === "function" ? source.getTopLists() : null;
    if (method === "collection") return typeof source.getTopListDetail === "function" ? source.getTopListDetail(args[0])
      : typeof source.getMusicSheetInfo === "function" ? source.getMusicSheetInfo(args[0], 1) : null;
    if (method === "lyrics") return typeof source.getLyric === "function" ? source.getLyric(args[0]) : null;
    return null;
  };
})();
