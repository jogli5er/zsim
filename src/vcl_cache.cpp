#include "vcl_cache.h"

uint64_t VCLCache::invalidate(const InvReq& req) {
  return Cache::invalidate(req);
}

bool VCLCache::isPresent(Address lineAddr) {
  return Cache::isPresent(lineAddr);
}

uint64_t VCLCache::access(MemReq& req) {
  uint64_t respCycle = req.cycle;
  bool skipAccess = cc->startAccess(req);
  if (likely(!skipAccess)) {
    bool updateReplacement = (req.type == GETS) || (req.type == GETX);
    uint64_t availCycle;
    int32_t lineId =
        array->lookup(req.lineAddr, &req, updateReplacement, &availCycle);
    if (lineId != -1 && cc->isValid(lineId)) {
      respCycle = (availCycle > respCycle) ? availCycle : respCycle + accLat;
    }
  }
}