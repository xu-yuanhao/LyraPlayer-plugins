#pragma once
#include "message_contracts.h"
#define LYRA_INTERFACE_COLLECTION_DIRECTORY 31u
// A successful INFO reply opts a media provider into collection discovery.
// Its PROVIDER UTF8 is the registered media provider ID, including NUL.
// An empty COLLECTION request then lists collection descriptors. A descriptor's
// uid is subsequently used as the key of a normal COLLECTION request.
enum {LYRA_COLLECTION_DIRECTORY_INFO=1};
enum {LYRA_COLLECTION_DIRECTORY_PROVIDER=1};
typedef struct LyraCollectionDirectoryV3 {
 uint32_t size,version;
 void *user;
 int (*request)(void *,const LyraMessageV3 *,LyraReplyV3 *);
} LyraCollectionDirectoryV3;
