/** @file

  Catch-based unit tests for RAM cache (CLFUS) compression roundtrips.

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

#include "main.h"

#include "../RamCacheCLFUS.h"
#include "../P_CacheInternal.h"

#include "iocore/cache/Cache.h"
#include "tscore/ink_config.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Required by main.h
int  cache_vols           = 1;
bool reuse_existing_cache = false;

namespace
{

// A compression backend to exercise, along with the RAM_HIT_* state get()
// should report once a compressible object has been stored compressed.
struct CompressionCase {
  CacheCompression config;
  RamHitCompress   expected_hit; // RAM_HIT_COMPRESS_* reported by get() for compressible data
  const char      *name;
};

std::vector<CompressionCase>
compression_cases()
{
  std::vector<CompressionCase> cases{
    {CacheCompression::None,    RamHitCompress::None,    "none"  },
    {CacheCompression::FastLZ,  RamHitCompress::FastLZ,  "fastlz"},
    {CacheCompression::DEFLATE, RamHitCompress::DEFLATE, "libz"  },
  };
#ifdef HAVE_LZMA_H
  cases.push_back({CacheCompression::LZMA, RamHitCompress::LZMA, "liblzma"});
#endif
#ifdef HAVE_LZ4_H
  cases.push_back({CacheCompression::LZ4, RamHitCompress::LZ4, "lz4"});
#endif
#ifdef HAVE_ZSTD_H
  cases.push_back({CacheCompression::zstd, RamHitCompress::zstd, "zstd"});
#endif
  return cases;
}

// Minimal CacheDisk wiring needed to construct a StripeSM. Mirrors the helper
// in test_Stripe.cc.
void
init_disk(CacheDisk &disk)
{
  disk.path                = static_cast<char *>(ats_malloc(1));
  disk.path[0]             = '\0';
  disk.disk_stripes        = static_cast<DiskStripe **>(ats_malloc(sizeof(DiskStripe *)));
  disk.disk_stripes[0]     = nullptr;
  disk.header              = static_cast<DiskHeader *>(ats_malloc(sizeof(DiskHeader)));
  disk.header->num_volumes = 0;
}

// The CLFUS get/put/compress paths touch only these metrics and the stripe
// mutex, so that is all the stripe needs for these tests.
void
wire_stripe(StripeSM &stripe, CacheVol &cache_vol)
{
  stripe.cache_vol = &cache_vol;

  cache_rsb.ram_cache_bytes               = ts::Metrics::Gauge::createPtr("unit_test.clfus.ram_cache.bytes");
  cache_rsb.ram_cache_hits                = ts::Metrics::Counter::createPtr("unit_test.clfus.ram_cache.hits");
  cache_rsb.ram_cache_misses              = ts::Metrics::Counter::createPtr("unit_test.clfus.ram_cache.misses");
  cache_rsb.ram_cache_decompress_failures = ts::Metrics::Counter::createPtr("unit_test.clfus.ram_cache.decompress.failure");
  cache_vol.vol_rsb.ram_cache_bytes       = ts::Metrics::Gauge::createPtr("unit_test.clfus.vol.ram_cache.bytes");
  cache_vol.vol_rsb.ram_cache_hits        = ts::Metrics::Counter::createPtr("unit_test.clfus.vol.ram_cache.hits");
  cache_vol.vol_rsb.ram_cache_misses      = ts::Metrics::Counter::createPtr("unit_test.clfus.vol.ram_cache.misses");
  cache_vol.vol_rsb.ram_cache_decompress_failures =
    ts::Metrics::Counter::createPtr("unit_test.clfus.vol.ram_cache.decompress.failure");
}

Ptr<IOBufferData>
make_buffer(const std::vector<char> &bytes)
{
  int64_t           idx = iobuffer_size_to_index(bytes.size(), MAX_BUFFER_SIZE_INDEX);
  Ptr<IOBufferData> data{make_ptr(new_IOBufferData(idx, MEMALIGNED))};
  std::memcpy(data->data(), bytes.data(), bytes.size());
  return data;
}

// Highly compressible: a short repeating pattern.
std::vector<char>
compressible_bytes(std::size_t len)
{
  std::vector<char> bytes(len);
  for (std::size_t i = 0; i < len; i++) {
    bytes[i] = static_cast<char>('A' + (i % 26));
  }
  return bytes;
}

// Effectively incompressible: a deterministic xorshift byte stream.
std::vector<char>
incompressible_bytes(std::size_t len)
{
  std::vector<char> bytes(len);
  uint32_t          state = 0x9e3779b9;
  for (std::size_t i = 0; i < len; i++) {
    state    ^= state << 13;
    state    ^= state >> 17;
    state    ^= state << 5;
    bytes[i]  = static_cast<char>(state & 0xff);
  }
  return bytes;
}

struct RoundtripResult {
  RamHitCompress    hit         = RamHitCompress::None;
  int64_t           size_before = 0; // rc.size() after put, before the compression pass
  int64_t           size_after  = 0; // rc.size() after the compression pass
  std::vector<char> out;
};

// Store payload under a fresh key, force a synchronous compression pass with
// `config`, then read it back.
RoundtripResult
store_compress_get(StripeSM &stripe, CacheCompression config, const std::vector<char> &payload)
{
  // Initialize with compression disabled so init() does not schedule the
  // background compressor (which would retain a pointer to this stack object).
  cache_config_ram_cache_compress         = static_cast<uint8_t>(CacheCompression::None);
  cache_config_ram_cache_compress_percent = 100;
  cache_config_ram_cache_use_seen_filter  = 0;

  RamCacheCLFUS rc;
  rc.init(1 << 20, &stripe);

  Ptr<IOBufferData> in  = make_buffer(payload);
  uint32_t          len = static_cast<uint32_t>(payload.size());

  static uint64_t salt = 0;
  ++salt;
  CryptoHash key;
  key.u64[0] = 0xc0ffee00 + salt;
  key.u64[1] = 0xdeadbeef + salt;

  REQUIRE(rc.put(&key, in.get(), len) == 1);

  RoundtripResult r;

  r.size_before                   = rc.size();
  cache_config_ram_cache_compress = static_cast<uint8_t>(config);
  rc.compress_entries(this_ethread());
  r.size_after = rc.size();

  Ptr<IOBufferData> ret;

  r.hit = static_cast<RamHitCompress>(rc.get(&key, &ret));
  REQUIRE(ret.get() != nullptr);
  r.out.assign(ret->data(), ret->data() + len);
  return r;
}

} // namespace

TEST_CASE("CLFUS compressible objects roundtrip cleanly", "[cache][ramcache][compress]")
{
  CacheDisk disk;
  init_disk(disk);
  StripeSM stripe{&disk, 10, 0};
  CacheVol cache_vol;
  wire_stripe(stripe, cache_vol);

  // Large enough to exercise the *_comCompressionCasepressBound() arithmetic and uint32_t
  // casts in compress_entries(), not just small-buffer paths.
  auto                  payload = compressible_bytes(256 * 1024);
  const CompressionCase c       = GENERATE(from_range(compression_cases()));
  INFO("compression backend: " << c.name);

  RoundtripResult r = store_compress_get(stripe, c.config, payload);

  CHECK(r.hit == c.expected_hit);
  CHECK(r.out == payload);
  if (c.config != CacheCompression::None) {
    // The feature's contract is that compression saves memory, not merely
    // that the entry is tagged compressed.
    CHECK(r.size_after < r.size_before);
  }
}

TEST_CASE("CLFUS incompressible objects fall back to uncompressed storage", "[cache][ramcache][compress]")
{
  CacheDisk disk;
  init_disk(disk);
  StripeSM stripe{&disk, 10, 0};
  CacheVol cache_vol;
  wire_stripe(stripe, cache_vol);

  auto payload = incompressible_bytes(256 * 1024);

  // Only the backends that actually attempt compression are interesting here;
  // skip the NONE case.
  auto cases = compression_cases();
  cases.erase(cases.begin());
  const CompressionCase c = GENERATE_REF(from_range(cases));
  INFO("compression backend: " << c.name);

  RoundtripResult r = store_compress_get(stripe, c.config, payload);

  // Incompressible data is kept verbatim, so a read reports no compression.
  CHECK(r.hit == RamHitCompress::None);
  CHECK(r.out == payload);
}

TEST_CASE("CLFUS single-byte payload roundtrips", "[cache][ramcache][compress]")
{
  CacheDisk disk;
  init_disk(disk);
  StripeSM stripe{&disk, 10, 0};
  CacheVol cache_vol;
  wire_stripe(stripe, cache_vol);

  RoundtripResult r = store_compress_get(stripe, CacheCompression::None, compressible_bytes(1));

  CHECK(r.hit == RamHitCompress::None);
  CHECK(r.out == compressible_bytes(1));
}

// A backend that is not compiled in silently disappears from the parametrized
// cases above; make that visible in the test output rather than shipping an
// untested backend behind a green run.
TEST_CASE("CLFUS compression backends compiled in", "[cache][ramcache][compress]")
{
#ifndef HAVE_LZMA_H
  WARN("liblzma is not compiled in; the liblzma RAM cache compression backend is NOT tested");
#endif
#ifndef HAVE_LZ4_H
  WARN("lz4 is not compiled in; the lz4 RAM cache compression backend is NOT tested");
#endif
#ifndef HAVE_ZSTD_H
  WARN("zstd is not compiled in; the zstd RAM cache compression backend is NOT tested");
#endif
  CHECK(compression_cases().size() >= 3);
}
