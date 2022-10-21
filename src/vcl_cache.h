#ifndef VCL_CACHE_H_
#define VCL_CACHE_H_

#define FULLMISS -1
#define OUTOFRANGEMISS -2

#include "bithacks.h"
#include "galloc.h"
#include "ooo_core_recorder.h"
#include "ooo_filter_cache.h"
#include "zsim.h"

class VCLCache : public FilterCache {
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
           uint32_t _invLat, g_string& _name)
      : FilterCache(_numSets, way_sizes.size(), _cc, _array, _rp, _accLat,
                    _invLat, _name) {
    numSets = _numSets;
    setMask = numSets - 1;
    srcId = -1;
    reqFlags = 0;
    numLinesNLP = 0;
    zeroLatencyCache = false;  // TODO: add setter if needed
#ifdef TRACE_BASED
    pref_degree = 1;
    g_string pref_kernels_file_dummy = "";
    if (pref_degree) {
      dataflow_prefetcher = new DataflowPrefetcher(
          "dataflow_" + _name, pref_degree, _name, true, false, true, true,
          true, true, this, pref_kernels_file_dummy);
    }
#endif
    accLat = _accLat;
  }

  virtual uint64_t issuePrefetch(Address lineAddr, uint32_t skip,
                                 uint64_t curCycle, uint64_t dispatchCycle,
                                 OOOCoreRecorder* cRec, uint64_t pc,
                                 bool isSW) override;

  void setFlags(uint32_t flags) { reqFlags = flags; }

  enum class Type { D, I };

  void setType(Type _type) { type = _type; }

  Type getType() const { return type; }

  void initStats(AggregateStat* parentStat) {
    AggregateStat* cacheStat = new AggregateStat();
    cacheStat->init(name.c_str(), "VCL cache stats");
    initCacheStats(cacheStat);
    parentStat->append(cacheStat);
  }
  virtual inline uint64_t load(Address vAddr, uint64_t curCycle,
                               uint64_t dispatchCycle, Address pc,
                               OOOCoreRecorder* cRec, uint8_t size) override;

  virtual inline uint64_t store(Address vAddr, uint64_t curCycle,
                                uint64_t dispatchCycle, Address pc,
                                OOOCoreRecorder* cRec, uint8_t size) override;
  bool isPresent(Address lineAddr) override;
  uint64_t replace(Address vAddr, uint32_t idx, bool isLoad, uint64_t curCycle,
                   Address pc, uint8_t size) {
    Address vLineAddr = vAddr >> lineBits;
    Address pLineAddr = procMask | vLineAddr;
    MESIState dummy = MESIState::I;

    futex_lock(&filterLock);
    MemReq req = {pc,          pLineAddr, isLoad ? GETS : GETX,
                  0,           &dummy,    curCycle,
                  &filterLock, dummy,     srcId,
                  reqFlags,    0,         vAddr,
                  size};
    uint64_t respCycle = access(req);
    futex_unlock(&filterLock);
    return respCycle;
  };

  uint64_t access(MemReq& req) override;

  virtual uint64_t invalidate(const InvReq& req) override;
  virtual ~VCLCache() {
    // TODO: ensure resources are all freed.
  }

 private:
  Type type;
  bool zeroLatencyCache;
  uint32_t numLinesNLP;
  uint32_t pref_degree;
#ifdef TRACE_BASED
  DataflowPrefetcher* dataflow_prefetcher;
#endif
};

#endif  // VCL_CACHE_H_