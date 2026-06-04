/** @file

  A brief file description

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

// Window-TinyLFU (W-TinyLFU) RAM cache replacement policy.
//
// The design follows two references:
//   * G. Einziger, R. Friedman, B. Manes, "TinyLFU: A Highly Efficient Cache Admission
//     Policy", ACM Trans. Storage 13(4), 2017 (arXiv:1512.00727) -- the frequency-sketch
//     admission filter with periodic aging.
//   * Caffeine (https://github.com/ben-manes/caffeine, Apache-2.0) -- the window plus
//     segmented-LRU structure, the sketch's increment-then-halve aging, and the adaptive
//     window.
//
// EXPERIMENTAL: a small "window" LRU in front of a main SLRU (probation +
// protected), with admission to the main cache gated by a TinyLFU frequency
// estimator (a Count-Min Sketch with periodic aging). The window absorbs bursts
// and recency; the CMS keeps one-hit-wonders out of the main cache; the periodic
// halving of the sketch is the built-in aging that lets the policy follow a
// shifting working set. Sized by bytes (not entry count) to fit the ATS RAM
// cache budget. See doc/developer-guide/cache-architecture/ram-cache.en.rst and
// the comparison tests in CacheTest.cc (ram_cache, ram_cache_adaptivity,
// ram_cache_drift, ram_cache_trace).

#include "P_RamCache.h"
#include "P_CacheInternal.h"
#include "StripeSM.h"
#include "iocore/eventsystem/IOBuffer.h"
#include "tscore/CryptoHash.h"
#include "tscore/List.h"
#include <vector>
#include <cstdlib>

#define ENTRY_OVERHEAD      128   // per-entry overhead counted against ram_cache.size
#define WINDOW_PERCENT      1     // window cache is this percent of total capacity
#define PROTECTED_PERCENT   80    // protected segment is this percent of the main cache
#define CMS_DEPTH           4     // number of Count-Min Sketch counters consulted per key
#define AVG_OBJECT_ESTIMATE 16384 // assumed average object size for capacity/reset estimates
// CMS_SAMPLE_FACTOR: halve the sketch once it has recorded ~ capacity * this many accesses
// (TinyLFU aging). Caffeine uses 10; 2 tracks non-stationary (drifting) workloads far better on
// the synthetic suite with no stationary-Zipfian loss. Tunable via TS_WTLFU_SAMPLE_FACTOR; the
// best value is workload-dependent.
#define CMS_SAMPLE_FACTOR 2
#define CMS_MAX           15 // 4-bit saturating counters
// Adaptive-window hill climber: step the window by this percent of capacity each adaptation
// period. The step must be large enough that the per-step hit-rate change stays above the
// sampling noise of one period; a 1% step stalls near the optimum on recency-heavy traces.
#define ADAPT_STEP_PERCENT 5

// Experimental tuning hooks: the policy parameters can be overridden from the environment so a
// sweep can be run without recompiling. Unset in production, so the #define defaults apply.
static int
wtlfu_env_int(const char *name, int dflt)
{
  const char *v = getenv(name);
  return v ? atoi(v) : dflt;
}

enum { SEG_WINDOW = 0, SEG_PROBATION = 1, SEG_PROTECTED = 2 };

struct RamCacheWTinyLFUEntry {
  CryptoHash key;
  uint64_t   auxkey;
  uint32_t   size;    // block_size of the data, the bytes accounted for this entry
  uint16_t   segment; // SEG_WINDOW / SEG_PROBATION / SEG_PROTECTED
  LINK(RamCacheWTinyLFUEntry, lru_link);
  LINK(RamCacheWTinyLFUEntry, hash_link);
  Ptr<IOBufferData> data;
};

struct RamCacheWTinyLFU : public RamCache {
  int     get(CryptoHash *key, Ptr<IOBufferData> *ret_data, uint64_t auxkey = 0) override;
  int     put(CryptoHash *key, IOBufferData *data, uint32_t len, bool copy = false, uint64_t auxkey = 0) override;
  int     fixup(const CryptoHash *key, uint64_t old_auxkey, uint64_t new_auxkey) override;
  int64_t size() const override;
  void    init(int64_t max_bytes, StripeSM *stripe) override;

private:
  int64_t _max_bytes       = 0;
  int64_t _bytes           = 0;
  int64_t _objects         = 0;
  int64_t _window_limit    = 0;
  int64_t _protected_limit = 0;
  int64_t _seg_bytes[3]    = {0, 0, 0};

  // Adaptive window (Caffeine-style hill climbing): the window/main split is nudged toward the
  // better recent hit rate. Disabled when the window is pinned via TS_WTLFU_WINDOW_PCT.
  int     _protected_pct  = PROTECTED_PERCENT;
  bool    _adapt          = true;
  int64_t _window_lo      = 0;
  int64_t _window_hi      = 0;
  int64_t _window_step    = 0;
  int64_t _adapt_interval = 0;
  int64_t _adapt_clock    = 0;
  int64_t _adapt_hits     = 0;
  double  _prev_hitrate   = -1.0;

  Que(RamCacheWTinyLFUEntry, lru_link) _seg[3];
  DList(RamCacheWTinyLFUEntry, hash_link) *_bucket = nullptr;
  int _nbuckets                                    = 0;
  int _ibuckets                                    = 0;

  // TinyLFU frequency sketch (Count-Min Sketch with 4-bit-ish saturating counters).
  std::vector<uint8_t> _freq;
  uint64_t             _freq_mask   = 0;
  int64_t              _freq_sample = 0;
  int64_t              _freq_reset  = 0;

  StripeSM *_stripe = nullptr;

  void                   _resize_hashtable();
  RamCacheWTinyLFUEntry *_lookup(const CryptoHash *key, uint64_t auxkey);
  void                   _move(RamCacheWTinyLFUEntry *e, int to_segment);
  void                   _free(RamCacheWTinyLFUEntry *e);
  void                   _evict_lru(int segment);
  void                   _enforce_budget();

  uint64_t _khash(const CryptoHash *key) const;
  uint32_t _freq_estimate(uint64_t h) const;
  void     _freq_record(uint64_t h);
  void     _adapt_window(bool hit);
};

ClassAllocator<RamCacheWTinyLFUEntry, false> ramCacheWTinyLFUEntryAllocator("RamCacheWTinyLFUEntry");

static const int bucket_sizes[] = {8191,    16381,   32749,    65521,    131071,   262139,    524287,    1048573,   2097143,
                                   4194301, 8388593, 16777213, 33554393, 67108859, 134217689, 268435399, 536870909, 1073741827};

void
RamCacheWTinyLFU::_resize_hashtable()
{
  int     anbuckets = bucket_sizes[_ibuckets];
  int64_t s         = anbuckets * sizeof(DList(RamCacheWTinyLFUEntry, hash_link));

  DList(RamCacheWTinyLFUEntry, hash_link) *new_bucket = static_cast<DList(RamCacheWTinyLFUEntry, hash_link) *>(ats_malloc(s));
  memset(static_cast<void *>(new_bucket), 0, s);
  if (_bucket) {
    for (int64_t i = 0; i < _nbuckets; i++) {
      RamCacheWTinyLFUEntry *e = nullptr;
      while ((e = _bucket[i].pop())) {
        new_bucket[e->key.slice32(3) % anbuckets].push(e);
      }
    }
    ats_free(_bucket);
  }
  _bucket   = new_bucket;
  _nbuckets = anbuckets;
}

void
RamCacheWTinyLFU::init(int64_t abytes, StripeSM *astripe)
{
  _stripe    = astripe;
  _max_bytes = abytes;
  if (!_max_bytes) {
    return;
  }
  const char *window_env = getenv("TS_WTLFU_WINDOW_PCT");
  int         window_pct = window_env ? atoi(window_env) : WINDOW_PERCENT;
  _protected_pct         = wtlfu_env_int("TS_WTLFU_PROTECTED_PCT", PROTECTED_PERCENT);
  _window_limit          = _max_bytes * window_pct / 100;
  _protected_limit       = (_max_bytes - _window_limit) * _protected_pct / 100;
  // Hill-climb the window between 1% and 80% of the cache (see _adapt_window), unless it was
  // pinned via TS_WTLFU_WINDOW_PCT (which is for experiments / sweeps).
  _adapt       = (window_env == nullptr) && wtlfu_env_int("TS_WTLFU_ADAPT", 1) != 0;
  _window_lo   = _max_bytes / 100;
  _window_hi   = _max_bytes * 80 / 100;
  _window_step = _max_bytes * ADAPT_STEP_PERCENT / 100;
  _resize_hashtable();

  // Size the frequency sketch to roughly the entry capacity (rounded up to a power of two), and
  // age it (halve every counter) once it has recorded ~ capacity * CMS_SAMPLE_FACTOR accesses.
  // The reset is tied to capacity in entries, NOT the sketch width: this is the TinyLFU aging
  // that decays a once-hot key's estimate as the working set turns over, so W-TinyLFU can follow
  // a shifting working set.
  int64_t est_objects = _max_bytes / AVG_OBJECT_ESTIMATE;
  if (est_objects < 64) {
    est_objects = 64;
  }
  uint64_t width = 1024;
  while (width < static_cast<uint64_t>(est_objects)) {
    width <<= 1;
  }
  _freq.assign(width * CMS_DEPTH, 0);
  _freq_mask   = width - 1;
  _freq_sample = 0;
  _freq_reset  = est_objects * wtlfu_env_int("TS_WTLFU_SAMPLE_FACTOR", CMS_SAMPLE_FACTOR);

  _adapt_interval = est_objects * 10; // re-evaluate the window roughly every 10 cache turnovers
  if (_adapt_interval < 10000) {
    _adapt_interval = 10000;
  }
}

uint64_t
RamCacheWTinyLFU::_khash(const CryptoHash *key) const
{
  // Combine all four 32-bit words of the object key into one 64-bit value.
  uint64_t h  = static_cast<uint64_t>(key->slice32(0)) * 0xff51afd7ed558ccdull;
  h          ^= static_cast<uint64_t>(key->slice32(1)) * 0xc4ceb9fe1a85ec53ull;
  h          ^= static_cast<uint64_t>(key->slice32(2)) * 0x9e3779b97f4a7c15ull;
  h          ^= static_cast<uint64_t>(key->slice32(3)) * 0xbf58476d1ce4e5b9ull;
  return h;
}

uint32_t
RamCacheWTinyLFU::_freq_estimate(uint64_t h) const
{
  static const uint64_t mul[CMS_DEPTH] = {0x9e3779b97f4a7c15ull, 0xc2b2ae3d27d4eb4full, 0x165667b19e3779f9ull,
                                          0xff51afd7ed558ccdull};
  uint32_t              m              = CMS_MAX;
  for (int d = 0; d < CMS_DEPTH; d++) {
    uint64_t idx = ((h * mul[d]) >> 32) & _freq_mask;
    uint8_t  c   = _freq[d * (_freq_mask + 1) + idx];
    if (c < m) {
      m = c;
    }
  }
  return m;
}

void
RamCacheWTinyLFU::_freq_record(uint64_t h)
{
  static const uint64_t mul[CMS_DEPTH] = {0x9e3779b97f4a7c15ull, 0xc2b2ae3d27d4eb4full, 0x165667b19e3779f9ull,
                                          0xff51afd7ed558ccdull};
  for (int d = 0; d < CMS_DEPTH; d++) {
    uint64_t idx = ((h * mul[d]) >> 32) & _freq_mask;
    uint8_t &c   = _freq[d * (_freq_mask + 1) + idx];
    if (c < CMS_MAX) {
      c++;
    }
  }
  if (++_freq_sample >= _freq_reset) {
    for (auto &c : _freq) {
      c >>= 1;
    }
    _freq_sample >>= 1;
  }
}

RamCacheWTinyLFUEntry *
RamCacheWTinyLFU::_lookup(const CryptoHash *key, uint64_t auxkey)
{
  uint32_t               i = key->slice32(3) % _nbuckets;
  RamCacheWTinyLFUEntry *e = _bucket[i].head;
  while (e) {
    if (e->key == *key && e->auxkey == auxkey) {
      return e;
    }
    e = e->hash_link.next;
  }
  return nullptr;
}

void
RamCacheWTinyLFU::_move(RamCacheWTinyLFUEntry *e, int to_segment)
{
  _seg[e->segment].remove(e);
  _seg_bytes[e->segment] -= e->size;
  e->segment              = to_segment;
  _seg[to_segment].enqueue(e);
  _seg_bytes[to_segment] += e->size;
}

void
RamCacheWTinyLFU::_free(RamCacheWTinyLFUEntry *e)
{
  uint32_t b = e->key.slice32(3) % _nbuckets;
  _bucket[b].remove(e);
  _bytes -= e->size + ENTRY_OVERHEAD;
  ts::Metrics::Gauge::decrement(cache_rsb.ram_cache_bytes, e->size);
  ts::Metrics::Gauge::decrement(_stripe->cache_vol->vol_rsb.ram_cache_bytes, e->size);
  _objects--;
  e->data = nullptr;
  ramCacheWTinyLFUEntryAllocator.free(e);
}

void
RamCacheWTinyLFU::_evict_lru(int segment)
{
  RamCacheWTinyLFUEntry *e = _seg[segment].dequeue();
  if (!e) {
    return;
  }
  _seg_bytes[segment] -= e->size;
  _free(e);
}

void
RamCacheWTinyLFU::_enforce_budget()
{
  // Evict from the main cache first (probation, then protected), then the window.
  while (_bytes > _max_bytes) {
    if (_seg[SEG_PROBATION].head) {
      _evict_lru(SEG_PROBATION);
    } else if (_seg[SEG_PROTECTED].head) {
      _evict_lru(SEG_PROTECTED);
    } else if (_seg[SEG_WINDOW].head) {
      _evict_lru(SEG_WINDOW);
    } else {
      break;
    }
  }
}

int
RamCacheWTinyLFU::get(CryptoHash *key, Ptr<IOBufferData> *ret_data, uint64_t auxkey)
{
  if (!_max_bytes) {
    return 0;
  }
  _freq_record(_khash(key)); // record the access frequency for every request
  RamCacheWTinyLFUEntry *e = _lookup(key, auxkey);
  if (e) {
    if (e->segment == SEG_PROBATION) {
      _move(e, SEG_PROTECTED); // a hit promotes a probationary entry
      while (_seg_bytes[SEG_PROTECTED] > _protected_limit && _seg[SEG_PROTECTED].head) {
        RamCacheWTinyLFUEntry *demote  = _seg[SEG_PROTECTED].dequeue();
        _seg_bytes[SEG_PROTECTED]     -= demote->size;
        demote->segment                = SEG_PROBATION;
        _seg[SEG_PROBATION].enqueue(demote);
        _seg_bytes[SEG_PROBATION] += demote->size;
      }
    } else {
      _seg[e->segment].remove(e); // refresh recency within window/protected
      _seg[e->segment].enqueue(e);
    }
    (*ret_data) = e->data;
    ts::Metrics::Counter::increment(cache_rsb.ram_cache_hits);
    ts::Metrics::Counter::increment(_stripe->cache_vol->vol_rsb.ram_cache_hits);
  } else {
    ts::Metrics::Counter::increment(cache_rsb.ram_cache_misses);
    ts::Metrics::Counter::increment(_stripe->cache_vol->vol_rsb.ram_cache_misses);
  }
  if (_adapt) {
    _adapt_window(e != nullptr);
  }
  return e != nullptr ? 1 : 0;
}

// Hill-climb the window/main split toward the better recent hit rate: each adaptation period,
// nudge the window by one step; if the hit rate fell versus the previous period, reverse the
// step direction. This lets W-TinyLFU find the recency/frequency balance a fixed window cannot.
void
RamCacheWTinyLFU::_adapt_window(bool hit)
{
  _adapt_hits += hit ? 1 : 0;
  if (++_adapt_clock < _adapt_interval) {
    return;
  }
  double hr = static_cast<double>(_adapt_hits) / static_cast<double>(_adapt_clock);
  // Reverse direction only on a real decline (beyond per-period sampling noise); otherwise keep
  // stepping the same way. The step magnitude is held constant rather than decayed: a fixed step
  // keeps the window probing on both sides of the current optimum, which tracks a shifting
  // (non-stationary) working set -- the realistic case -- far better than converging to a point.
  // The hysteresis keeps noise near a flat optimum from thrashing the direction.
  if (_prev_hitrate >= 0.0 && hr < _prev_hitrate - 0.002) {
    _window_step = -_window_step; // the last move hurt; go the other way
  }
  int64_t nw = _window_limit + _window_step;
  if (nw < _window_lo) {
    nw           = _window_lo;
    _window_step = -_window_step;
  } else if (nw > _window_hi) {
    nw           = _window_hi;
    _window_step = -_window_step;
  }
  _window_limit    = nw;
  _protected_limit = (_max_bytes - _window_limit) * _protected_pct / 100;
  _prev_hitrate    = hr;
  _adapt_hits      = 0;
  _adapt_clock     = 0;
}

int
RamCacheWTinyLFU::put(CryptoHash *key, IOBufferData *data, [[maybe_unused]] uint32_t len, bool, uint64_t auxkey)
{
  if (!_max_bytes) {
    return 0;
  }
  uint32_t               size = data->block_size();
  RamCacheWTinyLFUEntry *e    = _lookup(key, auxkey);
  if (e) { // update in place, keep current segment, refresh recency
    int64_t delta           = static_cast<int64_t>(size) - static_cast<int64_t>(e->size);
    _bytes                 += delta;
    _seg_bytes[e->segment] += delta;
    ts::Metrics::Gauge::increment(cache_rsb.ram_cache_bytes, delta);
    ts::Metrics::Gauge::increment(_stripe->cache_vol->vol_rsb.ram_cache_bytes, delta);
    e->size = size;
    e->data = data;
    _seg[e->segment].remove(e);
    _seg[e->segment].enqueue(e);
    _enforce_budget();
    return 1;
  }

  // New object: insert into the window cache.
  uint32_t i = key->slice32(3) % _nbuckets;
  e          = ramCacheWTinyLFUEntryAllocator.alloc();
  e->key     = *key;
  e->auxkey  = auxkey;
  e->size    = size;
  e->segment = SEG_WINDOW;
  e->data    = data;
  _bucket[i].push(e);
  _seg[SEG_WINDOW].enqueue(e);
  _seg_bytes[SEG_WINDOW] += size;
  _bytes                 += size + ENTRY_OVERHEAD;
  _objects++;
  ts::Metrics::Gauge::increment(cache_rsb.ram_cache_bytes, size);
  ts::Metrics::Gauge::increment(_stripe->cache_vol->vol_rsb.ram_cache_bytes, size);

  if (_objects > _nbuckets) {
    ++_ibuckets;
    _resize_hashtable();
  }

  // Drain the window: each overflow candidate is admitted to the main cache only if TinyLFU
  // estimates it is used at least as often as the main cache's eviction victim (probation LRU).
  // This is what keeps one-hit-wonders and scans out of the main cache.
  while (_seg_bytes[SEG_WINDOW] > _window_limit) {
    RamCacheWTinyLFUEntry *candidate = _seg[SEG_WINDOW].head;
    if (!candidate) {
      break;
    }
    RamCacheWTinyLFUEntry *victim = _seg[SEG_PROBATION].head;
    if (victim && _freq_estimate(_khash(&candidate->key)) <= _freq_estimate(_khash(&victim->key))) {
      _seg[SEG_WINDOW].dequeue();
      _seg_bytes[SEG_WINDOW] -= candidate->size;
      _free(candidate); // rejected admission
    } else {
      _move(candidate, SEG_PROBATION); // admitted
    }
  }
  _enforce_budget();
  return 1;
}

int
RamCacheWTinyLFU::fixup(const CryptoHash *key, uint64_t old_auxkey, uint64_t new_auxkey)
{
  if (!_max_bytes) {
    return 0;
  }
  RamCacheWTinyLFUEntry *e = _lookup(key, old_auxkey);
  if (e) {
    e->auxkey = new_auxkey;
    return 1;
  }
  return 0;
}

int64_t
RamCacheWTinyLFU::size() const
{
  int64_t s = 0;
  for (int seg = 0; seg < 3; seg++) {
    forl_LL(RamCacheWTinyLFUEntry, e, _seg[seg])
    {
      s += sizeof(*e);
      if (e->data) {
        s += sizeof(*e->data);
        s += e->data->block_size();
      }
    }
  }
  s += _freq.size(); // the frequency sketch
  return s;
}

RamCache *
new_RamCacheWTinyLFU()
{
  return new RamCacheWTinyLFU;
}
