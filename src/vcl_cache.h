#ifndef VCL_CACHE_H_
#define VCL_CACHE_H_

#include "bithacks.h"
#include "cache.h"
#include "galloc.h"
#include "ooo_core_recorder.h"
#include "zsim.h"

class VCLCache : public Cache {
 protected:
  struct FileEntry {
    volatile Address rdAddr;
    volatile Address wrAddr;
    volatile uint64_t availCycle;

    void clear() {
      wrAddr = 0;
      rdAddr = 0;
      availCycle = 0;
    }
  };

  Address setMask;
  uint32_t numSets;
  uint32_t srcId;  // should match the core
  uint32_t reqFlags;
  struct PrefetchInfo {
    Address addr;
    uint32_t skip;
    uint64_t pc;
    bool isSW;
    bool serialize;  // Serializes this prefetch after the previous one
                     // (dispatchCycle)

    PrefetchInfo(Address _addr, uint32_t _skip, uint64_t _pc, bool _isSW,
                 bool _serialize)
        : addr(_addr),
          skip(_skip),
          pc(_pc),
          isSW(_isSW),
          serialize(_serialize){};
  };
  g_vector<PrefetchInfo> prefetchQueue;

 public:
  VCLCache(uint32_t _numSets, std::vector<uint8_t> way_sizes, CC* _cc,
           VCLCacheArray* _array, ReplPolicy* _rp, uint32_t _accLat,
           uint32_t _invLat, const g_string& _name)
      : Cache(way_sizes.size(), _cc, _array, _rp, _accLat, _invLat, _name) {
    numSets = _numSets;
    setMask = numSets - 1;
    srcId = -1;
    reqFlags = 0;
    accLat = _accLat;
  }
  void setSourceId(uint32_t id) { srcId = id; }

  uint32_t getSourceId() const { return srcId; }

  void setFlags(uint32_t flags) { reqFlags = flags; }

  enum class Type { D, I };

  void setType(Type _type) { type = _type; }

  Type getType() const { return type; }

  void initStats(AggregateStat* parentStat) {
    AggregateStat* cacheStat = new AggregateStat();
    initCacheStats(cacheStat);
    parentStat->append(cacheStat);
  }
  inline uint64_t load(Address vAddr, uint64_t curCycle, Address pc) override {
    Address vLineAddr = vAddr >> lineBits;
    uint32_t idx = vLineAddr & setMask;
    return replace(vLineAddr, idx, true, curCycle, pc);
  }

  bool isPresent(Address lineAddr) override;
  uint64_t replace(Address vLineAddr, uint32_t idx, bool isLoad,
                   uint64_t curCycle, Address pc) override {
    return 1;
  };

  uint64_t access(MemReq& req) override;

  virtual uint64_t invalidate(const InvReq& req) override;
  virtual ~VCLCache() {
    // TODO: ensure resources are all freed.
  }

 private:
  Type type;
};

#endif  // VCL_CACHE_H_