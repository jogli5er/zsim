#include "vcl_cache.h"

#ifndef EXTERNAL_CACHE_MODEL
#include "timing_event.h"
#endif

uint64_t VCLCache::invalidate(const InvReq& req) {
  return Cache::invalidate(req);
}

bool VCLCache::isPresent(Address lineAddr) {
  return Cache::isPresent(lineAddr);
}

inline uint64_t VCLCache::store(Address vAddr, uint64_t curCycle,
                                uint64_t dispatchCycle, Address pc,
                                OOOCoreRecorder* cRec, uint8_t size) {
  Address vLineAddr = vAddr >> lineBits;
  uint32_t idx = vLineAddr & setMask;
  uint64_t respCycle = replace(vAddr, idx, false, curCycle, pc, size);

  cRec->record(curCycle, dispatchCycle, respCycle);
  executePrefetch(curCycle, dispatchCycle, 0, cRec);

  if (zeroLatencyCache) {
    return dispatchCycle;
  }

  return respCycle;
}

uint64_t VCLCache::issuePrefetch(Address lineAddr, uint32_t skip,
                                 uint64_t curCycle, uint64_t dispatchCycle,
                                 OOOCoreRecorder* cRec, uint64_t pc,
                                 bool isSW) {
  // TODO: What is a prefetch in the context of VCL caches? This is currently
  // just a copy of ooo_filter_cache, but we should think about that

  uint64_t respCycle;
  futex_lock(&filterLock);
  MESIState dummyState = MESIState::I;
  uint32_t flags = MemReq::PREFETCH | MemReq::SPECULATIVE;
  if (isSW) {
    flags |= MemReq::SW_SPECULATIVE;
  }
  MemReq req = {pc,          lineAddr,      GETS,        0,
                &dummyState, dispatchCycle, &filterLock, dummyState,
                srcId,       flags,         skip};
  respCycle = access(req);
  cRec->record(curCycle, dispatchCycle, respCycle);
  futex_unlock(&filterLock);
  return respCycle;
}

inline uint64_t VCLCache::load(Address vAddr, uint64_t curCycle,
                               uint64_t dispatchCycle, Address pc,
                               OOOCoreRecorder* cRec, uint8_t size) {
  Address vLineAddr = vAddr >> lineBits;
  uint32_t idx = vLineAddr & setMask;
  uint64_t respCycle = replace(vAddr, idx, true, curCycle, pc, size);
  cRec->record(curCycle, dispatchCycle, respCycle);
  executePrefetch(curCycle, dispatchCycle, 0, cRec);

  for (uint32_t numLines = 1; numLines <= numLinesNLP; numLines++) {
    Address pLineAddr = procMask | vLineAddr;
    Address nextPLineAddr = pLineAddr + numLines;
    issuePrefetch(nextPLineAddr, 0 /*prefetch into L1*/, curCycle,
                  dispatchCycle, cRec, 0 /*No PC*/, false);
  }
#ifdef TRACE_BASED
  // Access Dataflow Prefetcher
  if (pref_degree) {
    MESIState dummyState = MESIState::I;
    MemReq req = {
        pc,   vLineAddr,  GETS,          1, &dummyState, dispatchCycle,
        NULL, dummyState, getSourceId(), 0};
    dataflow_prefetcher->prefetch(req);
  }
#endif

  if (zeroLatencyCache) {
    return dispatchCycle;
  }

  return respCycle;
}

uint64_t VCLCache::access(MemReq& req) {
  uint64_t respCycle = req.cycle;
  bool skipAccess = cc->startAccess(req);
  if (likely(!skipAccess)) {
    bool updateReplacement = (req.type == GETS) || (req.type == GETX);
    uint64_t availCycle;
    int32_t prevId;
    int32_t lineId = ((VCLCacheArray*)array)
                         ->lookup(req.lineAddr, &req, updateReplacement,
                                  &availCycle, &prevId);
    // VCL Array can return multiple error codes
    if (lineId > FULLMISS && cc->isValid(lineId)) {
      respCycle = (availCycle > respCycle) ? availCycle : respCycle + accLat;
    } else {
      respCycle += accLat;
    }

    bool needPostInsert = false;

    std::vector<ReplacementCandidate> cacheEvictionCandidates;
    ReplacementCandidate bufferEvictionCandidate;

    if (lineId == FULLMISS && cc->shouldAllocate(req)) {
      bufferEvictionCandidate =
          ((VCLCacheArray*)array)->preinsert(req.lineAddr, &req);
      ZSIM_TRACE(VCLCache, "[%s] Evicting 0x%lx", name.c_str(),
                 bufferEvictionCandidate.writeBack);

      auto evictionCandidates =
          ((VCLCacheArray*)array)
              ->preinsert(req.lineAddr, &req, bufferEvictionCandidate.arrayIdx);

      // TODO: check if this is the data we need
      for (const auto candidate : evictionCandidates) {
        cc->processEviction(req, candidate.writeBack, candidate.arrayIdx,
                            respCycle);
      }
      cc->processEviction(req, bufferEvictionCandidate.writeBack,
                          bufferEvictionCandidate.arrayIdx, respCycle);

      needPostInsert = true;
    }

    if (lineId == OUTOFRANGEMISS && cc->shouldAllocate(req)) {
      Address wbLineAddr;
      // Find which buffer line we will use
      lineId =
          ((VCLCacheArray*)array)->preinsert(req.lineAddr, &req, &wbLineAddr);
      auto evictionCandidates =
          ((VCLCacheArray*)array)->preinsert(req.lineAddr, &req, lineId);

      // TODO: check if this is the data we need
      for (const auto candidate : evictionCandidates) {
        cc->processEviction(req, candidate.writeBack, candidate.arrayIdx,
                            respCycle);
      }
      needPostInsert = true;
      // Might yield multiple evictions
    }

#ifndef EXTERNAL_CACHE_MODEL
    EventRecorder* evRec = zinfo->eventRecorders[req.srcId];
    TimingRecord wbAcc;
    wbAcc.clear();
    if (unlikely(evRec && evRec->hasRecord() && req.prefetch == 0)) {
      wbAcc = evRec->popRecord();
    }
#endif

    respCycle = cc->processAccess(req, lineId, respCycle);
    if (needPostInsert) {
      ((VCLCacheArray*)array)
          ->postinsert(req.lineAddr, &req, lineId, respCycle);
    }
#ifndef EXTERNAL_CACHE_MODEL
    // Access may have generated another timing record. If *both* access
    // and wb have records, stitch them together
    if (unlikely(wbAcc.isValid())) {
      if (!evRec->hasRecord()) {
        // Downstream should not care about endEvent for PUTs
        wbAcc.endEvent = nullptr;
        evRec->pushRecord(wbAcc);
      } else {
        // Connect both events
        TimingRecord acc = evRec->popRecord();
        assert(wbAcc.reqCycle >= req.cycle);
        assert(acc.reqCycle >= req.cycle);
        DelayEvent* startEv = new (evRec) DelayEvent(0);
        DelayEvent* dWbEv = new (evRec) DelayEvent(wbAcc.reqCycle - req.cycle);
        DelayEvent* dAccEv = new (evRec) DelayEvent(acc.reqCycle - req.cycle);
        startEv->setMinStartCycle(req.cycle);
        dWbEv->setMinStartCycle(req.cycle);
        dAccEv->setMinStartCycle(req.cycle);
        startEv->addChild(dWbEv, evRec)->addChild(wbAcc.startEvent, evRec);
        startEv->addChild(dAccEv, evRec)->addChild(acc.startEvent, evRec);

        acc.reqCycle = req.cycle;
        acc.startEvent = startEv;
        // endEvent / endCycle stay the same; wbAcc's endEvent not connected
        evRec->pushRecord(acc);
      }
    }
#endif
  }
  cc->endAccess(req);
  return respCycle;
}