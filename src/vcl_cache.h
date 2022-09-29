#ifndef VCL_CACHE_H_
#define VCL_CACHE_H_

#include "bithacks.h"
#include "cache.h"
#include "galloc.h"
#include "ooo_core_recorder.h"
#include "zsim.h"

class VCLCacheArray : public CacheArray {
 public:
  virtual int32_t lookup(const Address lineAddr, const MemReq* req,
                         bool updateReplacement, uint64_t* availCycle) override;
  virtual uint32_t preinsert(const Address lineAddr, const MemReq* req,
                             Address* wbLineAddr) override;

  virtual void postinsert(const Address lineAddr, const MemReq* req,
                          uint32_t lineId, uint64_t respCycle) override;

  virtual void initStats(AggregateStat* parent) override;
};

class VCLCache : public Cache {
 public:
  VCLCache(uint32_t _numLines, std::vector<uint8_t> way_sizes, CC* _cc,
           CacheArray* _array, ReplPolicy* _rp, uint32_t _accLat,
           uint32_t _invLat, const g_string& _name);
  void initStats(AggregateStat* parentStat) override;
  bool isPresent(Address lineAddr) override;

  uint64_t access(MemReq& req) override;

  virtual uint64_t invalidate(const InvReq& req) override;
};

#endif  // VCL_CACHE_H_