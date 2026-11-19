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
      on(EVENT_NAMES.request,async ({source,info}) => "https://fixture.test/"+source+"/"+info.musicInfo.id);
      send(EVENT_NAMES.inited,{sources:{wy:{actions:["musicUrl"],qualitys:["128k"]}}});
    )";
    assert(Json::parse(sources::evaluate(lx,"metadata","[]"))["format"]=="lx");
    assert(Json::parse(sources::evaluate(lx,"resolve",R"(["wy",{"id":3}])"))["url"]=="https://fixture.test/wy/3");
    assert(sources::evaluate("require('fs')","metadata","[]").empty());
    assert(sources::evaluate("for(;;){}","metadata","[]").empty());
}
