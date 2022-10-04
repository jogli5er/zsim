#include "vcl_cache.h"

uint64_t VCLCache::invalidate(const InvReq& req) {
  return Cache::invalidate(req);
}

bool VCLCache::isPresent(Address lineAddr) {
  return Cache::isPresent(lineAddr);
}

uint64_t VCLCache::access(MemReq& req) { return 0; }