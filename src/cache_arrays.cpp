/** $lic$
 * Copyright (C) 2012-2015 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2013 by The Board of Trustees of Stanford University
 *
 * This file is part of zsim.
 *
 * zsim is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, version 2.
 *
 * If you use this software in your research, we request that you reference
 * the zsim paper ("ZSim: Fast and Accurate Microarchitectural Simulation of
 * Thousand-Core Systems", Sanchez and Kozyrakis, ISCA-40, June 2013) as the
 * source of the simulator in any publications that use this software, and that
 * you send us a citation of your work.
 *
 * zsim is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "cache_arrays.h"

#include "cache_prefetcher.h"
#include "cacheline_access_bitmask_helpers.h"
#include "hash.h"
#include "repl_policies.h"
#include "zsim.h"

/* Set-associative array implementation */

SetAssocArray::SetAssocArray(uint32_t _numLines, uint32_t _assoc,
                             ReplPolicy* _rp, HashFamily* _hf)
    : rp(_rp), hf(_hf), numLines(_numLines), assoc(_assoc) {
  array = gm_calloc<AddrCycle>(numLines);
  numSets = numLines / assoc;
  setMask = numSets - 1;
  assert_msg(isPow2(numSets),
             "must have a power of 2 # sets, but you specified %d", numSets);
}

void SetAssocArray::initStats(AggregateStat* parentStat) {
  AggregateStat* objStats = new AggregateStat();
  objStats->init("array", "Cache array stats");
  profPrefHit.init("prefHits",
                   "Cache line hits that were previously prefetched");
  objStats->append(&profPrefHit);
  profPrefEarlyMiss.init(
      "prefEarlyMiss",
      "Prefetched cache lines that were never used or fetched too early so "
      "they were already evicted from the cache");
  objStats->append(&profPrefEarlyMiss);
  profPrefLateMiss.init("prefLateMiss",
                        "Prefetched cache lines that were fetched too late and "
                        "were still in flight");
  objStats->append(&profPrefLateMiss);
  profPrefLateTotalCycles.init("prefTotalLateCyc",
                               "Total cycles lost waiting on late prefetches");
  objStats->append(&profPrefLateTotalCycles);
  profPrefSavedCycles.init(
      "prefSavedCyc",
      "Total cycles saved by hitting a prefetched line (also if late)");
  objStats->append(&profPrefSavedCycles);

  profPrefInCache.init("prefInCache", "Prefetch that hits cache");
  objStats->append(&profPrefInCache);
  profPrefNotInCache.init("prefNotInCache", "Prefetch that misses cache");
  objStats->append(&profPrefNotInCache);
  profPrefPostInsert.init("prefPostInsert",
                          "Prefetch that leads to replacement");
  objStats->append(&profPrefPostInsert);
  profPrefReplacePref.init("prefReplacePref",
                           "Prefetch replacing an already prefetched line");
  objStats->append(&profPrefReplacePref);

  profPrefHitPref.init("prefHitPref",
                       "Prefetch hitting an already prefetched line");
  objStats->append(&profPrefHitPref);
  profPrefAccesses.init("prefAccesses",
                        "Total number of accesses that are prefetches");
  objStats->append(&profPrefAccesses);
  profPrefInaccurateOOO.init("prefInaccurateOOO",
                             "Number of useless prefetches due to OOO");

#ifdef MONITOR_MISS_PCS
  profMissPc.init("highMissPc", "Load/Store PCs with the highest MPKI",
                  MONITORED_PCS);
  objStats->append(&profMissPc);
  profMissPcNum.init("highMissPcNum",
                     "Number of misses of Load/Store PCs with the highest MPKI",
                     MONITORED_PCS);
  objStats->append(&profMissPcNum);
  profHitPc.init("highPrefHitPc", "Load/Store PCs with the highest hit rate",
                 MONITORED_PCS);
  objStats->append(&profHitPc);
  profHitPcNum.init(
      "highHitPcNum",
      "Number of misses of Load/Store PCs with the highest hit rate",
      MONITORED_PCS);
  objStats->append(&profHitPcNum);

  profLatePc.init("highPrefLatePc", "Load/Store PCs with the highest late rate",
                  MONITORED_PCS);
  objStats->append(&profLatePc);
  profLatePcNum.init(
      "highLatePcNum",
      "Number of misses of Load/Store PCs with the highest late rate",
      MONITORED_PCS);
  objStats->append(&profLatePcNum);
  profEarlyPc.init("highPrefEarlyPc",
                   "Load/Store PCs with the highest too early rate",
                   MONITORED_PCS);
  objStats->append(&profEarlyPc);
  profEarlyPcNum.init(
      "highEarlyPcNum",
      "Number of misses of Load/Store PCs with the highest too early rate",
      MONITORED_PCS);
  objStats->append(&profEarlyPcNum);
#endif

  profHitDelayCycles.init("hitDelayCycles", "Delay cycles on an inflight hit");
  objStats->append(&profHitDelayCycles);
  profCacheLineUsed.init("cacheLineUsedBytes",
                         "Number of presences with n bytes accessed", 65);
  objStats->append(&profCacheLineUsed);
  parentStat->append(objStats);
}

int32_t SetAssocArray::lookup(const Address lineAddr, const MemReq* req,
                              bool updateReplacement, uint64_t* availCycle) {
  uint32_t set = hf->hash(0, lineAddr) & setMask;
  uint32_t first = set * assoc;
  // Only update prefetch stats if skip is zero (!req->prefetch)
  if (isHWPrefetch(req)) {
    profPrefAccesses.inc();
  }

  for (uint32_t id = first; id < first + assoc; id++) {
    if (array[id].addr == lineAddr) {
      // Lookup without request or prefetch skipping this level
      if (!req || req->prefetch) {
        *availCycle = array[id].availCycle;
        return id;
      }
      if (isHWPrefetch(req)) {
        profPrefInCache.inc();
      }

      if (updateReplacement && !req->prefetch) rp->update(id, req);
      if (req->size > 0) {
        Address baseAddress = lineAddr << (uint64_t)lineBits;
        uint64_t offset = (req->vAddr - baseAddress);
        set_accessed(array[id].accessMask, offset, offset + req->size);
      }
      // cache hit, line is in the cache
      if (req->cycle >= array[id].availCycle) {
        *availCycle = req->cycle;
        if (array[id].prefetch && isDemandLoad(req)) {
          profPrefHit.inc();
          profPrefSavedCycles.inc(array[id].availCycle - array[id].startCycle);
#ifdef MONITOR_MISS_PCS
          if (MONITORED_PCS && isDemandLoad(req)) {
            trackLoadPc(req->pc, hit_pcs, profHitPc, profHitPcNum);
          }
#endif
          array[id].prefetch = false;
        } else if (array[id].prefetch && isHWPrefetch(req)) {
          profPrefHitPref.inc();
        }
      }
      // line is in flight, compensate for potential OOO
      else {
        if (req->cycle < array[id].startCycle) {
          *availCycle =
              array[id].availCycle - (array[id].startCycle - req->cycle);
          // In case of OOO, fix state by storing cycles of the earlier access
          array[id].availCycle = *availCycle;
          array[id].startCycle = req->cycle;
          if (isDemandLoad(req)) {
            profPrefInaccurateOOO.inc();
          }
        } else {
          *availCycle = array[id].availCycle;
        }
        if (array[id].prefetch && isDemandLoad(req)) {
          profPrefLateMiss.inc();
          profPrefLateTotalCycles.inc(*availCycle - req->cycle);
          profPrefSavedCycles.inc(req->cycle - array[id].startCycle);
          if (isHWPrefetch(req)) {
            profPrefHitPref.inc();
          }
#ifdef MONITOR_MISS_PCS
          // Gather Load PC miss stats
          if (MONITORED_PCS) {
            trackLoadPc(array[id].pc, late_addr, profLatePc, profLatePcNum);
          }
#endif
          array[id].prefetch = false;
        } else if (array[id].prefetch && isHWPrefetch(req)) {
          profPrefHitPref.inc();
        }
      }
      if (isDemandLoad(req)) {
        profHitDelayCycles.inc(*availCycle - req->cycle);
      }
      return id;
    }
  }
  if (req && isHWPrefetch(req)) {
    profPrefNotInCache.inc();
  }

#ifdef MONITOR_MISS_PCS
  // Gather Load PC miss stats
  if (MONITORED_PCS && isDemandLoad(req)) {
    trackLoadPc(req->pc, miss_pcs, profMissPc, profMissPcNum);
  }
#endif

  return -1;
}

#ifdef MONITOR_MISS_PCS
void SetAssocArray::trackLoadPc(
    uint64_t pc, g_unordered_map<uint64_t, uint64_t>& tracked_pcs,
    VectorCounter& profPc, VectorCounter& profPcNum) {
  auto miss = tracked_pcs.find(pc);
  if (miss != tracked_pcs.end()) {
    miss->second++;
    if ((miss->second % 100) == 0) {
      g_multimap<uint64_t, uint64_t> sorted;
      for (auto entry : tracked_pcs) {
        sorted.insert(std::pair<uint64_t, uint64_t>(entry.second, entry.first));
      }
      int top_misses = 0;
      for (auto entry = sorted.rbegin(); entry != sorted.rend(); entry++) {
        if (top_misses == MONITORED_PCS) break;

        profPc.set(top_misses, entry->second);
        profPcNum.set(top_misses, entry->first);
        top_misses++;
      }
    }
  } else {
    tracked_pcs.insert(std::pair<uint64_t, uint64_t>(pc, 1));
  }
}
#endif

uint32_t SetAssocArray::preinsert(
    const Address lineAddr, const MemReq* req,
    Address* wbLineAddr) {  // TODO: Give out valid bit of wb cand?
  uint32_t set = hf->hash(0, lineAddr) & setMask;
  uint32_t first = set * assoc;

  uint32_t candidate = rp->rankCands(req, SetAssocCands(first, first + assoc));

  *wbLineAddr = array[candidate].addr;

  return candidate;
}

std::vector<uint8_t> get_block_sizes(const uint64_t& mask) {
  std::vector<uint8_t> result;
  uint8_t prev_bit = 0;
  uint8_t current_bit = 0;
  bool trailing = true;
  uint8_t first = 0;
  for (int byte = 0; byte < 64; byte++) {
    current_bit = ((mask >> byte) & 0x1);
    if (current_bit == 1 && trailing) {
      trailing = false;
      prev_bit = current_bit;
      first = byte;
      continue;
    } else if (current_bit == 0 && trailing) {
      continue;
    }

    // posedge/negedge kind of analysis
    if (current_bit == 0 && prev_bit == 1) {
      uint8_t res = byte - first;
      result.push_back(res);
    } else if (current_bit == 1 && prev_bit == 0) {
      first = byte;
    }
    prev_bit = current_bit;
  }

  if (prev_bit == 1 && current_bit == 1) {
    uint8_t res = 64 - first;
    result.push_back(res);
  }
  return result;
}

void SetAssocArray::postinsert(const Address lineAddr, const MemReq* req,
                               uint32_t candidate, uint64_t respCycle) {
  if (array[candidate].accessMask != 0) {
    std::vector<uint8_t> blockSizes =
        get_block_sizes(array[candidate].accessMask);
    for (const uint8_t size : blockSizes) profCacheLineUsed.inc(size);
  }
  array[candidate].accessMask = 0;  // reset for later use
  rp->replaced(candidate);
  if (isHWPrefetch(req)) {
    profPrefPostInsert.inc();
  }

  if (array[candidate].prefetch) {
    profPrefEarlyMiss.inc();
    if (isHWPrefetch(req)) {
      profPrefReplacePref.inc();
    }

#ifdef MONITOR_MISS_PCS
    // Gather Load PC miss stats
    if (MONITORED_PCS) {
      trackLoadPc(array[candidate].pc, early_addr, profEarlyPc, profEarlyPcNum);
    }
#endif
  }
  array[candidate].prefetch = isHWPrefetch(req);
  array[candidate].addr = lineAddr;
  array[candidate].availCycle = respCycle;
  array[candidate].startCycle = req->cycle;
  array[candidate].pc = req->pc;
  array[candidate].accessMask = 0;  // reset for later use
  rp->update(candidate, req);
}

/* ZCache implementation */

ZArray::ZArray(
    uint32_t _numLines, uint32_t _ways, uint32_t _candidates, ReplPolicy* _rp,
    HashFamily* _hf)  //(int _size, int _lineSize, int _assoc, int _zassoc,
                      // ReplacementPolicy<T>* _rp, int _hashType)
    : rp(_rp), hf(_hf), numLines(_numLines), ways(_ways), cands(_candidates) {
  assert_msg(ways > 1, "zcaches need >=2 ways to work");
  assert_msg(cands >= ways,
             "candidates < ways does not make sense in a zcache");
  assert_msg(numLines % ways == 0, "number of lines is not a multiple of ways");

  // Populate secondary parameters
  numSets = numLines / ways;
  assert_msg(isPow2(numSets),
             "must have a power of 2 # sets, but you specified %d", numSets);
  setMask = numSets - 1;

  lookupArray = gm_calloc<uint32_t>(numLines);
  array = gm_calloc<AddrCycle>(numLines);
  for (uint32_t i = 0; i < numLines; i++) {
    lookupArray[i] = i;  // start with a linear mapping; with swaps, it'll get
                         // progressively scrambled
  }
  swapArray = gm_calloc<uint32_t>(
      cands / ways + 2);  // conservative upper bound (tight within 2 ways)
}

void ZArray::initStats(AggregateStat* parentStat) {
  AggregateStat* objStats = new AggregateStat();
  objStats->init("array", "ZArray stats");
  statSwaps.init("swaps", "Block swaps in replacement process");
  objStats->append(&statSwaps);
  parentStat->append(objStats);
}

int32_t ZArray::lookup(const Address lineAddr, const MemReq* req,
                       bool updateReplacement, uint64_t* availCycle) {
  /* Be defensive: If the line is 0, panic instead of asserting. Now this can
   * only happen on a segfault in the main program, but when we move to full
   * system, phy page 0 might be used, and this will hit us in a very subtle
   * way if we don't check.
   */
  if (unlikely(!lineAddr))
    panic("ZArray::lookup called with lineAddr==0 -- your app just segfaulted");

  for (uint32_t w = 0; w < ways; w++) {
    uint32_t lineId =
        lookupArray[w * numSets + (hf->hash(w, lineAddr) & setMask)];
    if (array[lineId].addr == lineAddr) {
      if (updateReplacement) {
        rp->update(lineId, req);
      }
      if (req->cycle > array[lineId].availCycle) {
        *availCycle = req->cycle;
      } else if (req->cycle < array[lineId].startCycle) {
        *availCycle =
            array[lineId].availCycle - (array[lineId].startCycle - req->cycle);
      } else {
        *availCycle = array[lineId].availCycle;
      }

      return lineId;
    }
  }
  return -1;
}

uint32_t ZArray::preinsert(const Address lineAddr, const MemReq* req,
                           Address* wbLineAddr) {
  ZWalkInfo candidates[cands + ways];  // extra ways entries to avoid checking
                                       // on every expansion

  bool all_valid = true;
  uint32_t fringeStart = 0;
  uint32_t numCandidates = ways;  // seeds

  // info("Replacement for incoming 0x%lx", lineAddr);

  // Seeds
  for (uint32_t w = 0; w < ways; w++) {
    uint32_t pos = w * numSets + (hf->hash(w, lineAddr) & setMask);
    uint32_t lineId = lookupArray[pos];
    candidates[w].set(pos, lineId, -1);
    all_valid &= (array[lineId].addr != 0);
    // info("Seed Candidate %d addr 0x%lx pos %d lineId %d", w, array[lineId],
    // pos, lineId);
  }

  // Expand fringe in BFS fashion
  while (numCandidates < cands && all_valid) {
    uint32_t fringeId = candidates[fringeStart].lineId;
    Address fringeAddr = array[fringeId].addr;
    assert(fringeAddr);
    for (uint32_t w = 0; w < ways; w++) {
      uint32_t hval = hf->hash(w, fringeAddr) & setMask;
      uint32_t pos = w * numSets + hval;
      uint32_t lineId = lookupArray[pos];

      // Logically, you want to do this...
#if 0
            if (lineId != fringeId) {
                //info("Candidate %d way %d addr 0x%lx pos %d lineId %d parent %d", numCandidates, w, array[lineId].addr, pos, lineId, fringeStart);
                candidates[numCandidates++].set(pos, lineId, (int32_t)fringeStart);
                all_valid &= (array[lineId].addr != 0);
            }
#endif
      // But this compiles as a branch and ILP sucks (this data-dependent
      // branch is long-latency and mispredicted often) Logically though, this
      // is just checking for whether we're revisiting ourselves, so we can
      // eliminate the branch as follows:
      candidates[numCandidates].set(pos, lineId, (int32_t)fringeStart);
      all_valid &=
          (array[lineId].addr != 0);  // no problem, if lineId == fringeId the
                                      // line's already valid, so no harm done
      numCandidates +=
          (lineId != fringeId);  // if lineId == fringeId, the cand we just
                                 // wrote will be overwritten
    }
    fringeStart++;
  }

  // Get best candidate (NOTE: This could be folded in the code above, but
  // it's messy since we can expand more than zassoc elements)
  assert(!all_valid || numCandidates >= cands);
  numCandidates = (numCandidates > cands) ? cands : numCandidates;

  // info("Using %d candidates, all_valid=%d", numCandidates, all_valid);

  uint32_t bestCandidate =
      rp->rankCands(req, ZCands(&candidates[0], &candidates[numCandidates]));
  assert(bestCandidate < numLines);

  // Fill in swap array

  // Get the *minimum* index of cands that matches lineId. We need the minimum
  // in case there are loops (rare, but possible)
  uint32_t minIdx = -1;
  for (uint32_t ii = 0; ii < numCandidates; ii++) {
    if (bestCandidate == candidates[ii].lineId) {
      minIdx = ii;
      break;
    }
  }
  assert(minIdx >= 0);
  // info("Best candidate is %d lineId %d", minIdx, bestCandidate);

  lastCandIdx =
      minIdx;  // used by timing simulation code to schedule array accesses

  int32_t idx = minIdx;
  uint32_t swapIdx = 0;
  while (idx >= 0) {
    swapArray[swapIdx++] = candidates[idx].pos;
    idx = candidates[idx].parentIdx;
  }
  swapArrayLen = swapIdx;
  assert(swapArrayLen > 0);

  // Write address of line we're replacing
  *wbLineAddr = array[bestCandidate].addr;

  return bestCandidate;
}

void ZArray::postinsert(const Address lineAddr, const MemReq* req,
                        uint32_t candidate, uint64_t respCycle) {
  // We do the swaps in lookupArray, the array stays the same
  assert(lookupArray[swapArray[0]] == candidate);
  for (uint32_t i = 0; i < swapArrayLen - 1; i++) {
    // info("Moving position %d (lineId %d) <- %d (lineId %d)", swapArray[i],
    // lookupArray[swapArray[i]], swapArray[i+1],
    // lookupArray[swapArray[i+1]]);
    lookupArray[swapArray[i]] = lookupArray[swapArray[i + 1]];
  }
  lookupArray[swapArray[swapArrayLen - 1]] =
      candidate;  // note that in preinsert() we walk the array backwards when
                  // populating swapArray, so the last elem is where the new
                  // line goes
  // info("Inserting lineId %d in position %d", candidate,
  // swapArray[swapArrayLen-1]);

  rp->replaced(candidate);
  array[candidate].addr = lineAddr;
  array[candidate].availCycle = respCycle;
  array[candidate].startCycle = req->cycle;
  rp->update(candidate, req);

  statSwaps.inc(swapArrayLen - 1);
}

VCLCacheArray::VCLCacheArray(uint32_t _num_lines, std::vector<uint8_t> ways,
                             ReplPolicy* _rp, HashFamily* _hf)
    : SetAssocArray(_num_lines, ways.size(), _rp, _hf),
      rp(_rp),
      hf(_hf),
      numLines(_num_lines),
      waySizes(ways),
      assoc(ways.size()) {
  array = gm_calloc<AddrCycleVcl>(_num_lines);
  numSets = numLines / assoc;
  setMask = numSets - 1;
  assert_msg(isPow2(numSets),
             "must have a power of 2 # sets, but you specified %d", numSets);
}

int32_t VCLCacheArray::lookup(const Address lineAddr, const MemReq* req,
                              bool updateReplacement, uint64_t* availCycle) {
  uint32_t set = hf->hash(0, lineAddr) & setMask;
  uint32_t first = set * assoc;
  if (isHWPrefetch(req)) {
    profPrefAccesses.inc();
  }

  for (uint32_t id = first; id < first + assoc; id++) {
    if (array[id].addr == lineAddr) {
      if (!req || req->prefetch) {
        *availCycle = array[id].availCycle;
        return id;
      }
      if (isHWPrefetch(req)) {
        profPrefInCache.inc();
      }

      if (updateReplacement && !req->prefetch) rp->update(id, req);

      // cache hit, line is in the cache
      if (req->cycle >= array[id].availCycle) {
        *availCycle = req->cycle;
        if (array[id].prefetch && isDemandLoad(req)) {
          profPrefHit.inc();
          profPrefSavedCycles.inc(array[id].availCycle - array[id].startCycle);
#ifdef MONITOR_MISS_PCS
          if (MONITORED_PCS && isDemandLoad(req)) {
            trackLoadPc(req->pc, hit_pcs, profHitPc, profHitPcNum);
          }
#endif
          array[id].prefetch = false;
        } else if (array[id].prefetch && isHWPrefetch(req)) {
          profPrefHitPref.inc();
        }
      }
      // line is in flight, compensate for potential OOO
      else {
        if (req->cycle < array[id].startCycle) {
          *availCycle =
              array[id].availCycle - (array[id].startCycle - req->cycle);
          // In case of OOO, fix state by storing cycles of the earlier access
          array[id].availCycle = *availCycle;
          array[id].startCycle = req->cycle;
          if (isDemandLoad(req)) {
            profPrefInaccurateOOO.inc();
          }
        } else {
          *availCycle = array[id].availCycle;
        }
        if (array[id].prefetch && isDemandLoad(req)) {
          profPrefLateMiss.inc();
          profPrefLateTotalCycles.inc(*availCycle - req->cycle);
          profPrefSavedCycles.inc(req->cycle - array[id].startCycle);
          if (isHWPrefetch(req)) {
            profPrefHitPref.inc();
          }
#ifdef MONITOR_MISS_PCS
          // Gather Load PC miss stats
          if (MONITORED_PCS) {
            trackLoadPc(array[id].pc, late_addr, profLatePc, profLatePcNum);
          }
#endif
          array[id].prefetch = false;
        } else if (array[id].prefetch && isHWPrefetch(req)) {
          profPrefHitPref.inc();
        }
      }
      if (isDemandLoad(req)) {
        profHitDelayCycles.inc(*availCycle - req->cycle);
      }
      return id;
    }
  }
  if (req && isHWPrefetch(req)) {
    profPrefNotInCache.inc();
  }

#ifdef MONITOR_MISS_PCS
  // Gather Load PC miss stats
  if (MONITORED_PCS && isDemandLoad(req)) {
    trackLoadPc(req->pc, miss_pcs, profMissPc, profMissPcNum);
  }
#endif

  return -1;
}

/// @brief Lookup entry for lineAddr, returning either Failure code or array
/// idx. For Failure codes
/// @param lineAddr
/// @param req
/// @param updateReplacement
/// @param availCycle
/// @param prevId
/// @return
int32_t VCLCacheArray::lookup(const Address lineAddr, const MemReq* req,
                              bool updateReplacement, uint64_t* availCycle,
                              int32_t* prevId) {
  uint32_t set = hf->hash(0, lineAddr) & setMask;
  uint32_t first = set * assoc;
  if (isHWPrefetch(req)) {
    profPrefAccesses.inc();
  }

  // TODO
  for (uint32_t id = first; id < first + assoc; id++) {
    if (array[id].addr == lineAddr) {
      // hit - proceed as we would normally
      // todo: set access bitmask
      if (!req || req->prefetch) {
        *availCycle = array[id].availCycle;
        return id;
      }
      if (isHWPrefetch(req)) {
        profPrefInCache.inc();
      }

      if (updateReplacement && !req->prefetch) rp->update(id, req);

      // cache hit, line is in the cache
      if (req->cycle >= array[id].availCycle &&
          array[id].startOffset <= (req->vAddr - lineAddr) &&
          (req->vAddr - lineAddr) <
              array[id].startOffset + array[id].blockSize) {
        *availCycle = req->cycle;
        if (array[id].prefetch && isDemandLoad(req)) {
          profPrefHit.inc();
          profPrefSavedCycles.inc(array[id].availCycle - array[id].startCycle);
#ifdef MONITOR_MISS_PCS
          if (MONITORED_PCS && isDemandLoad(req)) {
            trackLoadPc(req->pc, hit_pcs, profHitPc, profHitPcNum);
          }
#endif
          array[id].prefetch = false;
        } else if (array[id].prefetch && isHWPrefetch(req)) {
          profPrefHitPref.inc();
        }
      } else if (req->cycle >= array[id].availCycle) {
        // present, but not in range
        // avail cycle ?
        *prevId = id;
        id = OUTOFRANGEMISS;
      }
      // line is in flight, compensate for potential OOO
      else {
        // When in flight it will be inserted into a 64 byte way - no need to
        // check for out of range
        if (req->cycle < array[id].startCycle) {
          *availCycle =
              array[id].availCycle - (array[id].startCycle - req->cycle);
          // In case of OOO, fix state by storing cycles of the earlier access
          array[id].availCycle = *availCycle;
          array[id].startCycle = req->cycle;
          if (isDemandLoad(req)) {
            profPrefInaccurateOOO.inc();
          }
        } else {
          *availCycle = array[id].availCycle;
        }
        if (array[id].prefetch && isDemandLoad(req)) {
          profPrefLateMiss.inc();
          profPrefLateTotalCycles.inc(*availCycle - req->cycle);
          profPrefSavedCycles.inc(req->cycle - array[id].startCycle);
          if (isHWPrefetch(req)) {
            profPrefHitPref.inc();
          }
#ifdef MONITOR_MISS_PCS
          // Gather Load PC miss stats
          if (MONITORED_PCS) {
            trackLoadPc(array[id].pc, late_addr, profLatePc, profLatePcNum);
          }
#endif
          array[id].prefetch = false;
        } else if (array[id].prefetch && isHWPrefetch(req)) {
          profPrefHitPref.inc();
        }
      }
      if (isDemandLoad(req)) {
        profHitDelayCycles.inc(*availCycle - req->cycle);
      }
      return id;
    }
  }
  if (req && isHWPrefetch(req)) {
    profPrefNotInCache.inc();
  }

#ifdef MONITOR_MISS_PCS
  // Gather Load PC miss stats
  if (MONITORED_PCS && isDemandLoad(req)) {
    trackLoadPc(req->pc, miss_pcs, profMissPc, profMissPcNum);
  }
#endif

  return -1;
}

uint32_t VCLCacheArray::preinsert(const Address lineAddr, const MemReq* req,
                                  Address* wbLineAddr) {
  ReplacementCandidate rc = this->preinsert(lineAddr, req);
  *wbLineAddr = rc.writeBack;
  return rc.arrayIdx;
}

/// @brief
/// @param lineAddr
/// @param req
/// @return
ReplacementCandidate VCLCacheArray::preinsert(const Address lineAddr,
                                              const MemReq* req) {
  uint32_t set = hf->hash(0, lineAddr) & setMask;
  uint32_t first = set * assoc;

  ReplacementCandidate candidate;
  int count = 0;
  for (const auto bufferCandidate : bufferWays) {
    if (!array[first + bufferCandidate].fifoCtr--) {
      candidate.arrayIdx = first + bufferCandidate;
      count++;
    }
  }
  assert(count == 1);

  assert(candidate.arrayIdx < first + waySizes.size());

  candidate.writeBack = array[candidate.arrayIdx].addr;
  candidate.accessMask = array[candidate.arrayIdx].accessMask;

  return candidate;
}

/// @brief This preinsert is for cachelines that have been evicted from the
/// buffer ways and now need to be inserted into lower idxd ways
/// @param lineAddr The line address to be inserted
/// @param req The mem request object containing all the details
/// @param prevIndex The index of the previous place in the cache array. Might
/// be used to initialize content. (could also be done depending on the
/// replacement policies metadata)
/// tuple: idx, wbAddr, start, end (start and end are of new block)
/// @return The index of the new insert location
std::vector<ReplacementCandidate> VCLCacheArray::preinsert(
    const Address lineAddr, const MemReq* req, int32_t prevIndex) {
  std::vector<BasicBlockOffsets> consecutiveBlocks =
      get_start_end_of_bitmask(array[prevIndex].accessMask);
  std::vector<ReplacementCandidate> candidates;
  if (consecutiveBlocks.size() == 0) {
    // No access recorded - no need to insert
    return candidates;
  }

  // sort from large to small to make sure we do not occupy large ways with
  // small blocks. Additionally, we need to do two things:
  // a) check that if we insert into larger block, the other blocks are not
  // already covered in full
  // b) that we do not double select a way
  std::sort(consecutiveBlocks.begin(), consecutiveBlocks.end(),
            [](BasicBlockOffsets a, BasicBlockOffsets b) {
              return (a.second - a.first > b.second - b.first);
            });
  uint32_t first = prevIndex - (prevIndex % waySizes.size());
  uint8_t maxWay = waySizes.size() - bufferWays.size();
  for (const auto block : consecutiveBlocks) {  // mostly 1 block
    uint8_t size = block.second - block.first + 1;
    uint32_t lineId = ((VCLLRUReplPolicy<true>*)rp)
                          ->rank(req,
                                 SetAssocCands(first, first + waySizes.size() -
                                                          bufferWays.size()),
                                 size, maxWay);
    size = std::max(waySizes[lineId % waySizes.size()], size);

    uint8_t start = block.first;

    if (start + size > 64) {
      start = 64 - size;  // Note: largest idx = 63, but start + size points to
                          // start of next block
    }

    uint8_t end = start + size - 1;  // idx to last byte

    ReplacementCandidate entry(lineId, array[lineId].addr, start, end);
    entry.accessMask = array[lineId].accessMask;
    candidates.push_back(entry);
  }
  return candidates;
}

void VCLCacheArray::postinsert(const Address lineAddr, const MemReq* req,
                               uint32_t lineId, uint64_t respCycle) {
  if (array[lineId].accessMask != 0) {
    std::vector<uint8_t> blockSizes = get_block_sizes(array[lineId].accessMask);
    for (const uint8_t size : blockSizes) profCacheLineUsed.inc(size);
  }
  array[lineId].accessMask = 0;  // reset for later use
  rp->replaced(lineId);
  if (isHWPrefetch(req)) {
    profPrefPostInsert.inc();
  }

  if (array[lineId].prefetch) {
    profPrefEarlyMiss.inc();
    if (isHWPrefetch(req)) {
      profPrefReplacePref.inc();
    }

#ifdef MONITOR_MISS_PCS
    // Gather Load PC miss stats
    if (MONITORED_PCS) {
      trackLoadPc(array[lineId].pc, early_addr, profEarlyPc, profEarlyPcNum);
    }
#endif
  }
  array[lineId].prefetch = isHWPrefetch(req);
  array[lineId].addr = lineAddr;
  array[lineId].availCycle = respCycle;
  array[lineId].startCycle = req->cycle;
  array[lineId].pc = req->pc;
  array[lineId].accessMask = 0;  // reset for later use
  rp->update(lineId, req);
  // ((VCLCacheArray*)(0))->preinsert(lineAddr, req, 0);
}

void VCLCacheArray::initStats(AggregateStat* parent) {
  SetAssocArray::initStats(parent);
  AggregateStat* objStats = new AggregateStat();
  objStats->init("array", "Cache array stats");
  profPrefOutOfBoundsMiss.init("prefOutOfBoundsMiss",
                               "Prefetch missing because of out of bounds");
  objStats->append(&profPrefOutOfBoundsMiss);
  parent->append(objStats);
}