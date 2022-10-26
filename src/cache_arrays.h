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

#ifndef CACHE_ARRAYS_H_
#define CACHE_ARRAYS_H_

#define FULLMISS -1
#define OUTOFRANGEMISS -2
#define HIT -3

#include <algorithm>
#include <iostream>

#include "g_std/g_multimap.h"
#include "g_std/g_unordered_map.h"
#include "memory_hierarchy.h"
#include "stats.h"
// #define MONITOR_MISS_PCS  // Uncomment to enable monitoring of cache misses

struct AddrCycle {
  Address addr;         // block address
  uint64_t availCycle;  // cycle when the block is available
  uint64_t
      startCycle;  // start cycle of the memory req that inserted this block
  bool prefetch;
  uint64_t pc;
  uint64_t accessMask;
};

struct AddrCycleVcl : public AddrCycle {
  uint8_t startOffset;
  uint8_t blockSize;
  Address addr;  // matches address of a default (64B) line/block
  uint64_t availCycle;
  uint64_t startCycle;
  bool prefetch;
  uint64_t pc;
  uint64_t accessMask;
  uint8_t fifoCtr;
};

struct ReplacementCandidate {
  uint32_t arrayIdx;
  Address writeBack;
  uint8_t startOffset;
  uint8_t endOffset;
  uint64_t accessMask;

 public:
  /// @brief Create a replacement candidate struct
  /// @param idx The index into the cache array that this entry had at the time
  /// this struct has been created.
  /// @param wb The address of the block stored and which we would need to write
  /// back in case of an eviction.
  /// @param start Where the newly to be inserted block starts (for anything
  /// except VCL this will always be 0)
  /// @param end Where the newly to be inserted block ends (for anything except
  /// VCL this will always be 63)
  ReplacementCandidate(uint32_t idx, Address wb, uint8_t start, uint8_t end)
      : arrayIdx(idx), writeBack(wb), startOffset(start), endOffset(end) {
    assert(start < end);
    assert(start > 0);
    assert(end < 64);
  };
  ReplacementCandidate() : startOffset(0), endOffset(63){};
};

/* General interface of a cache array. The array is a fixed-size associative
 * container that translates addresses to line IDs. A line ID represents the
 * position of the tag. The other cache components store tag data in
 * non-associative arrays indexed by line ID.
 */
class CacheArray : public GlobAlloc {
 public:
  /* Returns tag's ID if present, -1 otherwise. If updateReplacement is set,
   * call the replacement policy's update() on the line accessed Also set the
   * block availability cycle via 'availCycle' if the tag's ID is present*/
  virtual int32_t lookup(const Address lineAddr, const MemReq* req,
                         bool updateReplacement, uint64_t* availCycle) = 0;

  /* Runs replacement scheme, returns tag ID of new pos and address of line to
   * write back*/
  virtual uint32_t preinsert(const Address lineAddr, const MemReq* req,
                             Address* wbLineAddr) = 0;

  /* Actually do the replacement, writing the new address in lineId.
   * NOTE: This method is guaranteed to be called after preinsert, although
   * there may be some intervening calls to lookup. The implementation is
   * allowed to keep internal state in preinsert() and use it in postinsert()
   */
  virtual void postinsert(const Address lineAddr, const MemReq* req,
                          uint32_t lineId, uint64_t respCycle) = 0;

  virtual void initStats(AggregateStat* parent) {}
};

class ReplPolicy;
class HashFamily;

/* Set-associative cache array */
class SetAssocArray : public CacheArray {
 protected:
  AddrCycle* array;
  ReplPolicy* rp;
  HashFamily* hf;
  uint32_t numLines;
  uint32_t numSets;
  uint32_t assoc;
  uint32_t setMask;

#ifdef MONITOR_MISS_PCS
  static const uint32_t MONITORED_PCS = 10;
  g_unordered_map<uint64_t, uint64_t> miss_pcs;
  g_unordered_map<uint64_t, uint64_t> hit_pcs;
  g_unordered_map<uint64_t, uint64_t> late_addr;
  g_unordered_map<uint64_t, uint64_t> early_addr;

  VectorCounter profMissPc;
  VectorCounter profMissPcNum;
  VectorCounter profHitPc;
  VectorCounter profHitPcNum;

  VectorCounter profEarlyPc;
  VectorCounter profEarlyPcNum;
  VectorCounter profLatePc;
  VectorCounter profLatePcNum;
#endif

  Counter profPrefHit;
  Counter profPrefEarlyMiss;
  Counter profPrefLateMiss;
  Counter profPrefLateTotalCycles;
  Counter profPrefSavedCycles;
  Counter profPrefInaccurateOOO;
  Counter profHitDelayCycles;
  Counter profPrefHitPref;
  Counter profPrefAccesses;
  Counter profPrefInCache;
  Counter profPrefNotInCache;
  Counter profPrefPostInsert;
  Counter profPrefReplacePref;
  VectorCounter profCacheLineUsed;
  VectorCounter profBufferLineUsed;
  VectorCounter profVCLLineUsed;

 public:
  SetAssocArray(uint32_t _numLines, uint32_t _assoc, ReplPolicy* _rp,
                HashFamily* _hf);

  int32_t lookup(const Address lineAddr, const MemReq* req,
                 bool updateReplacement, uint64_t* availCycle);
  uint32_t preinsert(const Address lineAddr, const MemReq* req,
                     Address* wbLineAddr);
  void postinsert(const Address lineAddr, const MemReq* req, uint32_t candidate,
                  uint64_t respCycle);

  void trackLoadPc(uint64_t pc,
                   g_unordered_map<uint64_t, uint64_t>& tracked_pcs,
                   VectorCounter& profPc, VectorCounter& profPcNum);

  void initStats(AggregateStat* parentStat) override;
};

class VCLCacheArray : public SetAssocArray {
 private:
  AddrCycleVcl* array;
  uint32_t* lookupArray;
  ReplPolicy* rp;
  HashFamily* hf;
  uint32_t numLines;
  uint32_t numSets;
  std::vector<uint8_t> waySizes;
  std::vector<uint8_t> bufferWays;
  uint32_t assoc;
  uint32_t setMask;

  Counter profPrefOutOfBoundsMiss;

 public:
  VCLCacheArray(uint32_t _num_lines, std::vector<uint8_t> ways, ReplPolicy* _rp,
                HashFamily* _hf);
  virtual int32_t lookup(const Address lineAddr, const MemReq* req,
                         bool updateReplacement, uint64_t* availCycle) override;
  virtual int32_t lookup(const Address lineAddr, const MemReq* req,
                         bool updateReplacement, uint64_t* availCycle,
                         int32_t* prevId);
  virtual std::vector<ReplacementCandidate> getAllEntries(
      const Address lineAddr, const MemReq* req, bool invalidateEntries = true);
  virtual uint32_t preinsert(const Address lineAddr, const MemReq* req,
                             Address* wbLineAddr) override;
  virtual ReplacementCandidate preinsert(const Address lineAddr,
                                         const MemReq* req);
  std::vector<ReplacementCandidate> preinsert(
      const Address lineAddr, const MemReq* req,
      int32_t prevIndex); /*no-override*/

  virtual void postinsert(const Address lineAddr, const MemReq* req,
                          uint32_t lineId, uint64_t respCycle) override;

  //  range miss - reinsert into buffer way
  virtual void postinsert(const Address lineAddr, const MemReq* req,
                          uint32_t lineId,
                          std::vector<ReplacementCandidate> previousEntries,
                          uint64_t respCycle); /*no-override*/

  // eviction from buffer way
  virtual void postinsert(const Address lineAddr, const MemReq* req,
                          std::vector<ReplacementCandidate> targets,
                          uint64_t respCycle); /*no-override*/

  virtual void initStats(AggregateStat* parent) override;

  virtual void setBufferWays(std::vector<uint8_t> wayIndexes) {
    assert(wayIndexes.size() <= waySizes.size());
    std::sort(wayIndexes.begin(), wayIndexes.end());
    assert(wayIndexes.back() < waySizes.size());
    bufferWays = wayIndexes;
    // initialize buffer way entries
    for (size_t i = 0; i < bufferWays.size(); ++i) {
      // TODO: we might need to pass num of sets from the config for arbitrary
      // VCL configurations
      for (uint32_t j = 0; j < (numLines / waySizes.size()); j++) {
        array[j * waySizes.size() + bufferWays[i]].fifoCtr =
            i + 1;  // we evaluate ctr-1
      }
    }
  };
};

/* The cache array that started this simulator :) */
class ZArray : public CacheArray {
 private:
  AddrCycle* array;       // maps line id to {address, cycle}
  uint32_t* lookupArray;  // maps physical position to lineId
  ReplPolicy* rp;
  HashFamily* hf;
  uint32_t numLines;
  uint32_t numSets;
  uint32_t ways;
  uint32_t cands;
  uint32_t setMask;

  // preinsert() stores the swaps that must be done here, postinsert() does the
  // swaps
  uint32_t* swapArray;    // contains physical positions
  uint32_t swapArrayLen;  // set in preinsert()

  uint32_t lastCandIdx;

  Counter statSwaps;

 public:
  ZArray(uint32_t _numLines, uint32_t _ways, uint32_t _candidates,
         ReplPolicy* _rp, HashFamily* _hf);

  int32_t lookup(const Address lineAddr, const MemReq* req,
                 bool updateReplacement, uint64_t* availCycle);
  uint32_t preinsert(const Address lineAddr, const MemReq* req,
                     Address* wbLineAddr);
  void postinsert(const Address lineAddr, const MemReq* req, uint32_t candidate,
                  uint64_t respCycle);

  // zcache-specific, since timing code needs to know the number of swaps, and
  // these depend on idx Should be called after preinsert(). Allows intervening
  // lookups
  uint32_t getLastCandIdx() const { return lastCandIdx; }

  void initStats(AggregateStat* parentStat);
};

// Simple wrapper classes and iterators for candidates in each case; simplifies
// replacement policy interface without sacrificing performance NOTE: All must
// implement the same interface and be POD (we pass them by value)
struct SetAssocCands {
  struct iterator {
    uint32_t x;
    explicit inline iterator(uint32_t _x) : x(_x) {}
    inline void inc() { x++; }  // overloading prefix/postfix too messy
    inline uint32_t operator*() const { return x; }
    inline bool operator==(const iterator& it) const { return it.x == x; }
    inline bool operator!=(const iterator& it) const { return it.x != x; }
  };

  uint32_t b, e;
  inline SetAssocCands(uint32_t _b, uint32_t _e) : b(_b), e(_e) {}
  inline iterator begin() const { return iterator(b); }
  inline iterator end() const { return iterator(e); }
  inline uint32_t numCands() const { return e - b; }
};

struct ZWalkInfo {
  uint32_t pos;
  uint32_t lineId;
  int32_t parentIdx;

  inline void set(uint32_t p, uint32_t i, int32_t x) {
    pos = p;
    lineId = i;
    parentIdx = x;
  }
};

struct ZCands {
  struct iterator {
    ZWalkInfo* x;
    explicit inline iterator(ZWalkInfo* _x) : x(_x) {}
    inline void inc() { x++; }  // overloading prefix/postfix too messy
    inline uint32_t operator*() const { return x->lineId; }
    inline bool operator==(const iterator& it) const { return it.x == x; }
    inline bool operator!=(const iterator& it) const { return it.x != x; }
  };

  ZWalkInfo* b;
  ZWalkInfo* e;
  inline ZCands(ZWalkInfo* _b, ZWalkInfo* _e) : b(_b), e(_e) {}
  inline iterator begin() const { return iterator(b); }
  inline iterator end() const { return iterator(e); }
  inline uint32_t numCands() const { return e - b; }
};

#endif  // CACHE_ARRAYS_H_
