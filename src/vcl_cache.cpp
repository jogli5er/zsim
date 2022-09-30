#include "vcl_cache.h"

VCLCache::VCLCache(uint32_t _numLines, std::vector<uint8_t> way_sizes, CC* _cc,
                   CacheArray* _array, ReplPolicy* _rp, uint32_t _accLat,
                   uint32_t _invLat, const g_string& _name)
    : Cache(_numLines, _cc, _array, _rp, _accLat, _invLat, _name) {}

uint64_t VCLCache::invalidate(const InvReq& req) {
  return Cache::invalidate(req);
}

void VCLCache::initStats(AggregateStat* parentStat) {
  Cache::initStats(parentStat);
}

bool VCLCache::isPresent(Address lineAddr) {
  return Cache::isPresent(lineAddr);
}

uint64_t VCLCache::access(MemReq& req) { return 0; }