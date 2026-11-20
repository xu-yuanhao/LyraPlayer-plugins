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
  const console = Object.freeze({ log() {}, warn() {}, error() {}, info() {}, debug() {}, group() {}, groupCollapsed() {}, groupEnd() {} });
  const operation = budget => ({ id: "lyra", deadlineAt: Date.now()+budget,
    signal: Object.freeze({ aborted: false, addEventListener() {}, removeEventListener() {}, throwIfAborted() {} }) });
  const ceru = async (source) => {
    const manifest = source.manifest;
    const pack = source.package;
    if ((pack != null && (pack.formatVersion !== 2 || pack.syntax !== "js" || pack.signature !== null ||
        pack.template != null || pack.delivery != null)) || manifest.signature || manifest.delivery || manifest.template)
      return { format: "ceru-package" };
    if (manifest.engines?.logicRuntime !== "ceru-js@1" || typeof source.activate !== "function")
      throw new Error("Unsupported Ceru activation");
    const providers = new Map();
    const effects = [];
    const declared = (manifest.contributes?.providers ?? []).filter(entry => entry.connectionMode == null || entry.connectionMode === "none");
    const allowedKeys = new Set((manifest.permissions ?? []).filter(p => p.name === "network.request").map(p => p.key));
    const permissions = Object.freeze({
      async query({key}) { return {status:allowedKeys.has(key) ? "granted" : "denied"}; },
      async request({key}) { return this.query({key}); },
    });
    const http = Object.freeze({
      async request(input) {
        if (!allowedKeys.has(input.permissionKey) || input.credential != null)
          throw Error("Network permission unavailable");
        const url = new URL(input.url);
        if(url.protocol==="http:" && !url.port)url.protocol="https:";
        if(url.protocol!=="https:" || url.username || url.password)throw Error("Expected HTTPS URL");
        input.operation.signal.throwIfAborted();
        const result=request(url.href,{method:input.method,headers:input.headers,body:input.body});
        if (!result.status) throw Error("HTTP connection failed");
        return {status:result.status,headers:result.headers ?? {},body:parse(result.body)};
      },
      create(options) { return CeruRuntime.createHttpClient({ http, permissions },options); },
    });
    const discard = () => ({ dispose() {} });
    const ctx = Object.freeze({
      plugin: Object.freeze({ id:manifest.id, version:manifest.version, manifest }),
      config: Object.freeze({ async get() { return Object.freeze({...manifest.config}); } }),
      log: console,
      permissions, http,
      effects:Object.freeze({add(effect){if(effect && typeof effect.dispose==="function")effects.push(effect);}}),
      actions:Object.freeze({register:discard}),
      lyricConverters:Object.freeze({register:discard}),
      playlistImporters:Object.freeze({register:discard}),
      ui:Object.freeze({toast() {},pluginUpdate:Object.freeze({async request(){return false;}})}),
      playback:Object.freeze({failure:Object.freeze({register:discard})}),
      providers: Object.freeze({
        register(id, implementation) {
          if (!declared.some(entry => entry.id === id) || providers.has(id)) throw new Error("Invalid provider registration");
          providers.set(id,implementation);
          return { dispose() { providers.delete(id); } };
        },
      }),
    });
    Object.assign(globalThis,{ceru:ctx});
    const legacy=CeruRuntime.createLegacyHttpBridge(ctx);
    legacyBridges.set(ctx,legacy);
    await source.activate(ctx);
    if (![...providers.values()].some(p => typeof (p.tracks ?? p).search === "function" || typeof (p.tracks ?? p).resolve === "function" || p.playlists || p.charts))
      throw new Error("No compatible Ceru track provider");
    return { format:"ceru", manifest, providers, declared, effects };
  };
  const legacyBridges = new WeakMap();
  const ceruModules=Object.freeze({
    "@ceru/crypto":CeruRuntime.crypto,
    "@ceru/compression":CeruRuntime.compression,
    "@ceru/encoding":Object.freeze({decode:(bytes,encoding)=>new TextDecoder(encoding).decode(bytes)}),
    "@ceru/legacy-http":Object.freeze({
      createLegacyHttpBridge(ctx) {
        const bridge=legacyBridges.get(ctx);
        if(!bridge)throw Error("Legacy HTTP context unavailable");
        return bridge;
      }
    }),
  });
  globalThis.URL=CeruRuntime.URL;
  globalThis.URLSearchParams=CeruRuntime.URLSearchParams;
  globalThis.Buffer=CeruRuntime.Buffer;
  globalThis.__ceruRequire=(name)=>{
    if(!Object.hasOwn(ceruModules,name))throw Error("Unsupported Ceru module: "+name);
    return ceruModules[name];
  };
  globalThis.__load = async code => {
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
    if (source?.manifest?.manifestVersion === 2) return ceru(source);
    if (typeof listener === "function" && sources && typeof sources === "object")
      return { format: "lx", sources, listener };
    if (!source || typeof source.platform !== "string" ||
        !["search", "getMediaSource", "getTopLists", "getMusicSheetInfo"].some(key => typeof source[key] === "function"))
      throw new Error("Unsupported source script");
    return { format: "musicfree", source };
  };
  globalThis.__metadata = plugin => {
    if (plugin.format === "ceru-package") return { format:"ceru-package" };
    if (plugin.format === "ceru") {
      const trackProviders=[...plugin.providers.values()].map(p=>p.tracks ?? p);
      const capabilities = [["search","搜索"],["resolve","播放"],["lyrics","歌词"]]
        .filter(([method]) => trackProviders.some(provider => typeof provider[method] === "function"))
        .map(([, label]) => label);
      if([...plugin.providers.values()].some(p=>p.charts?.list || p.playlists?.categories))capabilities.push("歌单");
      return { format:"ceru", name:plugin.manifest.name, version:plugin.manifest.version,
        description:plugin.manifest.description, capabilities,
        searchProviders:[...plugin.providers].filter(([,p])=>typeof (p.tracks ?? p).search==="function").map(([id])=>id),
        chartProviders:[...plugin.providers].filter(([,p])=>typeof p.charts?.list==="function").map(([id])=>id),
        playlistProviders:[...plugin.providers].filter(([,p])=>typeof p.playlists?.categories==="function").map(([id])=>id) };
    }
    if (plugin.format === "lx") return { format: "lx", capabilities: ["播放解析", "需要搭配可搜索的音源"], platforms: Object.keys(plugin.sources) };
    const source = plugin.source;
    const capabilities = [["search", "搜索"], ["getMediaSource", "播放"], ["getTopLists", "歌单"], ["getLyric", "歌词"]]
      .filter(([method]) => typeof source[method] === "function").map(([, label]) => label);
    return { format: "musicfree", name: source.platform, version: source.version,
      description: source.description, update: source.srcUrl, icon: source.icon, capabilities };
  };
  globalThis.__invoke = async (plugin, method, args) => {
    if (plugin.format === "ceru") {
      const acceptable=(ref,providerId,kind)=>ref?.providerId===providerId && ref.kind===kind &&
        typeof ref.id==="string" && ref.connectionId==null &&
        (ref.pluginId==null || ref.pluginId===plugin.manifest.id);
      const tracks=(rows,providerId)=>{
        const hits=[];
        if(!Array.isArray(rows))return hits;
        for(const row of rows){
          if(hits.length>=100)break;
          if(!row || !acceptable(row.ref,providerId,"track"))continue;
          const metadata=row.metadata ?? {};
          hits.push({id:row.ref.id,title:row.title,artist:metadata.artists?.join(" / ") ?? row.subtitle,
            album:metadata.album?.title,artwork:metadata.artworkUrl,
            duration:(row.durationMs ?? metadata.durationMs ?? 0)/1000,ceruRef:row.ref});
        }
        return hits;
      };
      const op=operation(method==="collections"?14500:9500);
      if (method === "search") {
        const hits = [];
        for (const [id, group] of plugin.providers) {
          if (args[1] != null && args[1] !== id) continue;
          const provider=group.tracks ?? group;
          if (typeof provider.search !== "function") continue;
          try {
            const result=await provider.search({query:args[0],kinds:["track"],filters:{},limit:50},op);
            hits.push(...tracks(result?.items,id));
          } catch {}
          if(hits.length>=100)break;
        }
        return {data:hits.slice(0,100)};
      }
      if(method==="collections"){
        const rows=[];
        const append=(items,id,kind)=>{
          for(const row of items ?? []){
            if(rows.length>=100)break;
            if(acceptable(row?.ref,id,kind) && typeof row.title==="string")
              rows.push({id:row.ref.id,title:row.title,ceruRef:row.ref});
          }
        };
        for(const [id, group] of plugin.providers){
          if(args[0]!=null && args[0]!==id)continue;
          if(args[1]!=="playlists"){
            try { append((await group.charts?.list?.(op))?.items,id,"chart"); }catch {}
          }
          if(args[1]==="charts")continue;
          try {
            const categories=(await group.playlists?.categories?.(op))?.items ?? [];
            append(categories,id,"playlist");
            const category=categories.find(row=>acceptable(row?.ref,id,"playlist-category"));
            if(category)append((await group.playlists.list?.(category.ref,undefined,op))?.items,id,"playlist");
          }catch {}
        }
        return rows;
      }
      const item=method==="resolve"?args[1]:args[0];
      const ref=item?.ceruRef;
      const group=plugin.providers.get(ref?.providerId);
      if(!group || !acceptable(ref,ref.providerId,ref.kind))return null;
      if(method==="collection"){
        const result=ref.kind==="chart"?await group.charts?.getTracks?.(ref,undefined,op)
          :ref.kind==="playlist"?await group.playlists?.get?.(ref,undefined,op):null;
        const rows=result?.items ?? result?.tracks;
        return {data:Array.isArray(rows)?rows.slice(0,100).flatMap(row=>{
          const providerId=row?.ref?.providerId;
          return plugin.providers.has(providerId)?tracks([row],providerId):[];
        }):[]};
      }
      if(ref.kind!=="track")return null;
      const provider=group.tracks ?? group;
      if(method==="resolve"){
        const quality=plugin.declared.find(entry=>entry.id===ref.providerId)?.qualities?.[0];
        const resolved=await provider.resolve?.(ref,quality,op);
        return resolved?.ok===true?{url:resolved.url,headers:resolved.requestHeaders}:null;
      }
      if(method==="lyrics"){
        const result=await provider.lyrics?.(ref,op);
        if(result?.format!=="crlyric")return null;
        if(!Array.isArray(result.lines) || !result.lines.length)return result.plainText ?? null;
        const lrc=result.lines.flatMap(line=>{
          if(!Number.isFinite(line.startTimeMs) || line.startTimeMs<0 || typeof line.text!=="string")return [];
          const time=Math.max(0,Math.trunc(line.startTimeMs+(result.offsetMs ?? 0)));
          const pad=(value,width)=>String(value).padStart(width,"0");
          return  "["+pad(Math.floor(time/60000),2)+":"+pad(Math.floor(time/1000)%60,2)+"."+pad(time%1000,3)+"]"+line.text;
        }).join("\n");
        return lrc;
      }
      return null;
    }
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
