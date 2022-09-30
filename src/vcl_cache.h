#ifndef VCL_CACHE_H_
#define VCL_CACHE_H_

#include "bithacks.h"
#include "cache.h"
#include "galloc.h"
#include "ooo_core_recorder.h"
#include "zsim.h"

class VCLCache : public Cache {
 public:
  VCLCache(uint32_t _numLines, std::vector<uint8_t> way_sizes, CC* _cc,
           CacheArray* _array, ReplPolicy* _rp, uint32_t _accLat,
           uint32_t _invLat, const g_string& _name);
  void initStats(AggregateStat* parentStat) override;
  bool isPresent(Address lineAddr) override;

  uint64_t access(MemReq& req) override;

  virtual uint64_t invalidate(const InvReq& req) override;
  virtual ~VCLCache() {
    // TODO: ensure resources are all freed.
  }
};

#endif  // VCL_CACHE_H_