#include "engine.h"
#include "json.hpp"
#include <cassert>
using Json=nlohmann::json;
int main(){
    const char *musicfree=R"(
      const axios=require("axios");
      module.exports={
        platform:"Fixture",version:"1.0",
        async search(query){return {data:[{id:1,title:query}]}},
        async getMediaSource(song){const response=await axios.get("https://fixture.test/"+song.id);return response.data},
        async getTopLists(){return [{title:"Fixture",data:[{id:"1",title:"Test list"}]}]}
      };
    )";
    auto metadata=Json::parse(sources::evaluate(musicfree,"metadata","[]"));
    assert(metadata["name"]=="Fixture");
    auto search=Json::parse(sources::evaluate(musicfree,"search",R"(["Track"])"));
    assert(search["data"][0]["title"]=="Track");
    auto stream=Json::parse(sources::evaluate(musicfree,"resolve",R"(["",{"id":12}])",
        [](auto url,auto method,auto,auto){
            assert(url=="https://fixture.test/12" && method=="GET");
            return R"({"status":200,"body":"{\"url\":\"https://fixture.test/track\"}"})";
        }));
    assert(stream["url"]=="https://fixture.test/track");
    auto collection=Json::parse(sources::evaluate(musicfree,"collections","[]"));
    assert(collection[0]["data"][0]["title"]=="Test list");
    const char *lx=R"(
      const {on,send,EVENT_NAMES}=globalThis.lx;
      on(EVENT_NAMES.request,async ({source,info}) => {
        console.group("resolve");
        console.groupEnd();
        return "https://fixture.test/"+source+"/"+info.musicInfo.id;
      });
      send(EVENT_NAMES.inited,{sources:{wy:{actions:["musicUrl"],qualitys:["128k"]}}});
    )";
    assert(Json::parse(sources::evaluate(lx,"metadata","[]"))["format"]=="lx");
    assert(Json::parse(sources::evaluate(lx,"resolve",R"(["wy",{"id":3}])"))["url"]=="https://fixture.test/wy/3");
    const char *ceru=R"(
      exports.manifest = {manifestVersion:2, id:"fixture.ceru", name:"Fixture",version:"1.0",
        engines:{logicRuntime:"ceru-js@1"}, contributes:{providers:[{id:"local",qualities:["low"]}]}};
      exports.activate = async (ctx) => {
        await Promise.resolve();
        ctx.providers.register("local",{tracks:{
          async search(request, op){
            if(!request.kinds.includes("track") || op.signal.aborted) throw Error("Invalid request");
            return {items:[{ref:{providerId:"local",kind:"track",id:"42",data:{ticket:"fixture"}},
              title:request.query, durationMs:42000, metadata:{artists:["Artist"],album:{title:"Album"}}}]};
          },
          async lyrics(){return {format:"crlyric",offsetMs:10,lines:[{startTimeMs:990,text:"Fixture lyrics"}]};},
          async resolve(ref, quality){
            if(quality !== "low" || ref.data.ticket !== "fixture") return {ok:false};
            return {ok:true,url:"https://fixture.test/track",requestHeaders:{"X-Fixture":"yes"}};
          }
        },charts:{
          async list(){return {items:[{ref:{providerId:"local",kind:"chart",id:"top"},title:"Fixture chart"}]};},
          async getTracks(){return {items:[{ref:{providerId:"local",kind:"track",id:"42"},title:"Chart track"}]};}
        },playlists:{
          async categories(){return {items:[{ref:{providerId:"local",kind:"playlist-category",id:"cat"},title:"Category"}]};},
          async list(ref){return {items:[{ref:{providerId:"local",kind:"playlist",id:ref.id+"-playlist"},title:"Fixture playlist"}]};},
          async get(){return {tracks:[{ref:{providerId:"local",kind:"track",id:"43"},title:"Playlist track"}]};}
        }});
      };
    )";
    assert(Json::parse(sources::evaluate(ceru,"metadata","[]"))["format"]=="ceru");
    auto ceru_search=Json::parse(sources::evaluate(ceru,"search",R"(["Track"])"));
    auto track=ceru_search["data"][0];
    assert(track["title"]=="Track" && track["artist"]=="Artist" && track["duration"]==42);
    auto ceru_play=Json::parse(sources::evaluate(ceru,"resolve",Json::array({"",track}).dump()));
    assert(ceru_play["url"]=="https://fixture.test/track" && ceru_play["headers"]["X-Fixture"]=="yes");
    auto lyric=Json::parse(sources::evaluate(ceru,"lyrics",Json::array({track}).dump()));
    assert(lyric=="[00:01.000]Fixture lyrics");
    auto directory=Json::parse(sources::evaluate(ceru,"collections","[]"));
    assert(directory[0]["title"]=="Fixture chart" && directory[1]["title"]=="Fixture playlist");
    auto playlists=Json::parse(sources::evaluate(ceru,"collections",R"(["local","playlists"])"));
    assert(playlists.size()==1 && playlists[0]["title"]=="Fixture playlist");
    auto playlist_tracks=Json::parse(sources::evaluate(ceru,"collection",Json::array({playlists[0]}).dump()));
    assert(playlist_tracks["data"][0]["title"]=="Playlist track");
    auto collection_tracks=Json::parse(sources::evaluate(ceru,"collection",Json::array({directory[0]}).dump()));
    assert(collection_tracks["data"][0]["title"]=="Chart track");
    auto signed_ceru=std::string(ceru)+"\nexports.package={signature:{}};";
    assert(Json::parse(sources::evaluate(signed_ceru,"metadata","[]"))["format"]=="ceru-package");
    const auto bundle=std::string(ceru)+R"(
      exports.package={formatVersion:2,syntax:"js",signature:null};
      exports.manifest.permissions=[{key:"network",name:"network.request"}];
      const legacy=globalThis.__ceruRequire("@ceru/legacy-http");
      const crypto=globalThis.__ceruRequire("@ceru/crypto");
      const encoding=globalThis.__ceruRequire("@ceru/encoding");
      const activate=exports.activate;
      exports.activate=async ctx=>{
        if(crypto.createHash("sha256").update("hello").digest("hex").length!==64)throw Error("hash");
        if(crypto.randomBytes(4).length!==4)throw Error("random");
        if(encoding.decode(new Uint8Array([97]),"utf-8")!=="a")throw Error("encoding");
        const bridge=legacy.createLegacyHttpBridge(ctx);
        await activate({providers:{register(id,provider){
          const search=provider.tracks.search;
          provider.tracks.search=(request,op)=>bridge.withOperation(op,async ()=>{
            await new Promise(resolve=>{
              const cancelled=setTimeout(()=>{throw Error("cleared timer fired");},0);
              clearTimeout(cancelled);
              setTimeout(resolve,1);
            });
            const response=await bridge.httpFetch("http://fixture.test/search").promise;
            if(response.body.title!=="Fixture")throw Error("transport");
            return search(request,op);
          });
          return ctx.providers.register(id,provider);
        }}});
      };
    )";
    assert(Json::parse(sources::evaluate(bundle,"metadata","[]"))["format"]=="ceru");
    auto bundle_search=Json::parse(sources::evaluate(bundle,"search",R"(["Track"])",[](auto url,auto call,auto,auto){
        assert(url=="https://fixture.test/search" && call=="GET");
        return R"({"status":200,"body":"{\"title\":\"Fixture\"}"})";
    }));
    assert(bundle_search["data"][0]["title"]=="Track");
    assert(sources::evaluate("require('fs')","metadata","[]").empty());
    assert(sources::evaluate("for(;;){}","metadata","[]").empty());
}
