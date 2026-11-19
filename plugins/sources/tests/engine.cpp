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
          async resolve(ref, quality){
            if(quality !== "low" || ref.data.ticket !== "fixture") return {ok:false};
            return {ok:true,url:"https://fixture.test/track",requestHeaders:{"X-Fixture":"yes"}};
          }
        }});
      };
    )";
    assert(Json::parse(sources::evaluate(ceru,"metadata","[]"))["format"]=="ceru");
    auto ceru_search=Json::parse(sources::evaluate(ceru,"search",R"(["Track"])"));
    auto track=ceru_search["data"][0];
    assert(track["title"]=="Track" && track["artist"]=="Artist" && track["duration"]==42);
    auto ceru_play=Json::parse(sources::evaluate(ceru,"resolve",Json::array({"",track}).dump()));
    assert(ceru_play["url"]=="https://fixture.test/track" && ceru_play["headers"]["X-Fixture"]=="yes");
    auto signed_ceru=std::string(ceru)+"\nexports.package={signature:{}};";
    assert(Json::parse(sources::evaluate(signed_ceru,"metadata","[]"))["format"]=="ceru-package");
    assert(sources::evaluate("require('fs')","metadata","[]").empty());
    assert(sources::evaluate("for(;;){}","metadata","[]").empty());
}
