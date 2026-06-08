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

// SIEVE RAM cache replacement policy.
//
// The design follows Zhang, Yang, Yue, Yang, Vinayak & Rashmi, "SIEVE is Simpler than LRU: an
// Efficient Turn-Key Eviction Algorithm for Web Caches" (NSDI 2024) and https://sieve-cache.com.
//
// EXPERIMENTAL: a single FIFO-ordered list with one "visited" bit per object and a lazy eviction
// "hand". New objects are appended (unvisited); a hit only sets the bit (NO list reordering, so a
// hit is cheaper than LRU's move-to-front under the per-stripe lock). To evict, the hand sweeps
// from where it last stopped toward the newest end, clearing visited bits and evicting the first
// unvisited object it meets, then stays there. Objects that earned a hit get one more sweep to
// prove themselves; one-hit-wonders fall out quickly, so SIEVE is scan-resistant without a seen
// filter. Memory is LRU-class (one extra bit per entry, no ghost list). Sized by bytes to fit the
// ATS RAM cache budget. See doc/developer-guide/cache-architecture/ram-cache.en.rst and the
// comparison tests in CacheTest.cc.

#include "P_RamCache.h"
#include "P_CacheInternal.h"
#include "StripeSM.h"
#include "iocore/eventsystem/IOBuffer.h"
#include "tscore/CryptoHash.h"
#include "tscore/List.h"

#define ENTRY_OVERHEAD 128 // per-entry overhead counted against ram_cache.size

struct RamCacheSieveEntry {
  CryptoHash key;
  uint64_t   auxkey;
  bool       visited; // the SIEVE "visited" bit, set on a hit, cleared by the sweeping hand
  LINK(RamCacheSieveEntry, lru_link);
  LINK(RamCacheSieveEntry, hash_link);
  Ptr<IOBufferData> data;
};

struct RamCacheSieve : public RamCache {
  int     get(CryptoHash *key, Ptr<IOBufferData> *ret_data, uint64_t auxkey = 0) override;
  int     put(CryptoHash *key, IOBufferData *data, uint32_t len, bool copy = false, uint64_t auxkey = 0) override;
  int     fixup(const CryptoHash *key, uint64_t old_auxkey, uint64_t new_auxkey) override;
  int64_t size() const override;
  void    init(int64_t max_bytes, StripeSM *stripe) override;

private:
  int64_t _max_bytes = 0;
  int64_t _bytes     = 0;
  int64_t _objects   = 0;

  Que(RamCacheSieveEntry, lru_link) _list;                 // insertion order: head = oldest, tail = newest
  RamCacheSieveEntry *_hand                     = nullptr; // eviction cursor; nullptr means "start at the head"
  DList(RamCacheSieveEntry, hash_link) *_bucket = nullptr;
  int       _nbuckets                           = 0;
  int       _ibuckets                           = 0;
  StripeSM *_stripe                             = nullptr;

  void _resize_hashtable();
  void _free(RamCacheSieveEntry *e);
  void _evict_one();
};

ClassAllocator<RamCacheSieveEntry, false> ramCacheSieveEntryAllocator("RamCacheSieveEntry");

static const int bucket_sizes[] = {8191,    16381,   32749,    65521,    131071,   262139,    524287,    1048573,   2097143,
                                   4194301, 8388593, 16777213, 33554393, 67108859, 134217689, 268435399, 536870909, 1073741827};

void
RamCacheSieve::_resize_hashtable()
{
  ink_release_assert(_ibuckets < static_cast<int>(sizeof(bucket_sizes) / sizeof(bucket_sizes[0])));
  int     anbuckets = bucket_sizes[_ibuckets];
  int64_t s         = anbuckets * sizeof(DList(RamCacheSieveEntry, hash_link));

  DList(RamCacheSieveEntry, hash_link) *new_bucket = static_cast<DList(RamCacheSieveEntry, hash_link) *>(ats_malloc(s));
  memset(static_cast<void *>(new_bucket), 0, s);
  if (_bucket) {
    for (int64_t i = 0; i < _nbuckets; i++) {
      RamCacheSieveEntry *e = nullptr;
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
RamCacheSieve::init(int64_t abytes, StripeSM *astripe)
{
  _stripe    = astripe;
  _max_bytes = abytes;
  if (!_max_bytes) {
    return;
  }
  _resize_hashtable();
}

int64_t
RamCacheSieve::size() const
{
  int64_t s = 0;
  forl_LL(RamCacheSieveEntry, e, _list)
  {
    s += sizeof(*e);
    if (e->data) {
      s += sizeof(*e->data);
      s += e->data->block_size();
    }
  }
  return s;
}

void
RamCacheSieve::_free(RamCacheSieveEntry *e)
{
  if (e == _hand) { // keep the hand valid across any removal
    _hand = e->lru_link.next;
  }
  uint32_t b = e->key.slice32(3) % _nbuckets;
  _bucket[b].remove(e);
  _list.remove(e);
  _bytes -= ENTRY_OVERHEAD + e->data->block_size();
  ts::Metrics::Gauge::decrement(cache_rsb.ram_cache_bytes, ENTRY_OVERHEAD + e->data->block_size());
  ts::Metrics::Gauge::decrement(_stripe->cache_vol->vol_rsb.ram_cache_bytes, ENTRY_OVERHEAD + e->data->block_size());
  e->data = nullptr;
  ramCacheSieveEntryAllocator.free(e);
  _objects--;
}

// Sweep the hand from its last position toward the newest end: clear the visited bit of every
// object it passes (granting a second chance) and evict the first unvisited one, leaving the hand
// just past the victim. Wraps from the newest end back to the head. Terminates in at most ~2N
// steps because bits are only ever cleared here, never set.
void
RamCacheSieve::_evict_one()
{
  RamCacheSieveEntry *e = _hand ? _hand : _list.head;
  while (e && e->visited) {
    e->visited = false;
    e          = e->lru_link.next ? e->lru_link.next : _list.head;
  }
  if (!e) {
    return; // empty
  }
  _hand = e->lru_link.next; // resume past the victim next time (nullptr -> head)
  _free(e);
}

int
RamCacheSieve::get(CryptoHash *key, Ptr<IOBufferData> *ret_data, uint64_t auxkey)
{
  if (!_max_bytes) {
    return 0;
  }
  uint32_t            i = key->slice32(3) % _nbuckets;
  RamCacheSieveEntry *e = _bucket[i].head;
  while (e) {
    if (e->key == *key && e->auxkey == auxkey) {
      e->visited  = true; // the only work on a hit: set the bit, no list reordering
      (*ret_data) = e->data;
      ts::Metrics::Counter::increment(cache_rsb.ram_cache_hits);
      ts::Metrics::Counter::increment(_stripe->cache_vol->vol_rsb.ram_cache_hits);
      return 1;
    }
    e = e->hash_link.next;
  }
  ts::Metrics::Counter::increment(cache_rsb.ram_cache_misses);
  ts::Metrics::Counter::increment(_stripe->cache_vol->vol_rsb.ram_cache_misses);
  return 0;
}

int
RamCacheSieve::put(CryptoHash *key, IOBufferData *data, [[maybe_unused]] uint32_t len, bool, uint64_t auxkey)
{
  if (!_max_bytes) {
    return 0;
  }
  uint32_t            i = key->slice32(3) % _nbuckets;
  RamCacheSieveEntry *e = _bucket[i].head;
  while (e) {
    if (e->key == *key) {
      if (e->auxkey == auxkey) {
        e->visited = true; // already resident: treat the re-put as an access (matches LRU's bump)
        return 1;
      }
      RamCacheSieveEntry *next = e->hash_link.next; // aux keys conflict: discard the stale entry
      _free(e);
      e = next;
      continue;
    }
    e = e->hash_link.next;
  }

  // Make room before inserting, so the new (still unvisited) object is never the victim of its
  // own insertion -- matches the reference SIEVE's evict-then-insert ordering.
  int64_t need = ENTRY_OVERHEAD + data->block_size();
  while (_bytes + need > _max_bytes && _objects > 0) {
    _evict_one();
  }

  e          = ramCacheSieveEntryAllocator.alloc();
  e->key     = *key;
  e->auxkey  = auxkey;
  e->visited = false; // new objects must earn their first hit before the hand comes around
  e->data    = data;
  _bucket[i].push(e);
  _list.enqueue(e); // append at the newest end
  _bytes += need;
  _objects++;
  ts::Metrics::Gauge::increment(cache_rsb.ram_cache_bytes, need);
  ts::Metrics::Gauge::increment(_stripe->cache_vol->vol_rsb.ram_cache_bytes, need);

  if (_objects > _nbuckets * 0.75) {
    ++_ibuckets;
    _resize_hashtable();
  }
  return 1;
}

int
RamCacheSieve::fixup(const CryptoHash *key, uint64_t old_auxkey, uint64_t new_auxkey)
{
  if (!_max_bytes) {
    return 0;
  }
  uint32_t            i = key->slice32(3) % _nbuckets;
  RamCacheSieveEntry *e = _bucket[i].head;
  while (e) {
    if (e->key == *key && e->auxkey == old_auxkey) {
      e->auxkey = new_auxkey;
      return 1;
    }
    e = e->hash_link.next;
  }
  return 0;
}

RamCache *
new_RamCacheSieve()
{
  return new RamCacheSieve;
}
