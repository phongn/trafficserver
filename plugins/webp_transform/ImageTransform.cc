/**
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

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "ts/ts.h"

#include "tscpp/api/PluginInit.h"
#include "tscpp/api/GlobalPlugin.h"
#include "tscpp/api/TransformationPlugin.h"
#include "tscpp/api/Logger.h"
#include "tscpp/api/Stat.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#if !defined(__clang__)
#pragma GCC diagnostic ignored "-Wsuggest-override"
#endif
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif
#include <Magick++.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

using namespace Magick;
using namespace atscppapi;

#define TAG "webp_transform"

namespace
{
DbgCtl webp_dbg_ctl{TAG};

enum class ImageEncoding { webp, jpeg, png, unknown };

bool config_convert_to_webp = false;
bool config_convert_to_jpeg = false;

// Encoder quality (1-100), overridable with the webp_quality/jpeg_quality
// arguments. It is always explicit: inheriting the source's quality turns a
// q100 JPEG into a lossless WebP and a lossless WebP into a q100 JPEG, both
// larger than needed and far slower to encode.
int config_webp_quality = 80;
int config_jpeg_quality = 85;

Stat stat_convert_to_webp;
Stat stat_convert_to_jpeg;
Stat stat_convert_to_png;

bool
has_signature_for(std::string_view data, ImageEncoding encoding)
{
  constexpr std::string_view png_signature{"\x89PNG\r\n\x1a\n", 8};

  switch (encoding) {
  case ImageEncoding::webp:
    return data.size() >= 12 && data.substr(0, 4) == "RIFF" && data.substr(8, 4) == "WEBP";
  case ImageEncoding::jpeg:
    return data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xff && static_cast<unsigned char>(data[1]) == 0xd8 &&
           static_cast<unsigned char>(data[2]) == 0xff;
  case ImageEncoding::png:
    return data.starts_with(png_signature);
  case ImageEncoding::unknown:
    return false;
  }

  return false;
}

// Cap the buffered (encoded) response body. 16 MiB fits every
// realistic image asset while keeping the worst case bounded. The default is
// overridable with the max_buffer_size plugin argument (see TSPluginInit).
constexpr size_t DEFAULT_MAX_BUFFERED_IMAGE_SIZE = 16ULL * 1024 * 1024;
size_t           max_buffered_image_size         = DEFAULT_MAX_BUFFERED_IMAGE_SIZE;

// Parse a byte count with an optional binary suffix (K, M, or G, 1024-based),
// for the max_buffer_size argument. Returns 0 on any malformed input so the
// caller can reject it and keep the default.
size_t
parse_size(const char *value)
{
  // strtoull() skips leading whitespace and then accepts an optional sign, so a
  // value like "-1" would wrap to a huge size_t and silently disable the cap.
  // Reject any signed input up front. Skip the SAME whitespace set strtoull()
  // skips (the full isspace() set, not just space/tab); otherwise a value like
  // "\n-1" slips past this guard and strtoull() still wraps it.
  const char *p = value;
  while (std::isspace(static_cast<unsigned char>(*p))) {
    ++p;
  }
  if (*p == '-' || *p == '+') {
    return 0;
  }

  errno        = 0;
  char *end    = nullptr;
  auto  scaled = std::strtoull(p, &end, 10);
  if (errno != 0 || end == p) {
    return 0;
  }
  size_t multiplier = 1;
  if (*end != '\0') {
    if (end[1] != '\0') { // at most one suffix character
      return 0;
    }
    switch (*end) {
    case 'k':
    case 'K':
      multiplier = 1024ULL;
      break;
    case 'm':
    case 'M':
      multiplier = 1024ULL * 1024;
      break;
    case 'g':
    case 'G':
      multiplier = 1024ULL * 1024 * 1024;
      break;
    default:
      return 0;
    }
  }
  // Treat a multiply that would overflow size_t as invalid input rather than
  // letting the cap wrap to an unintended (small) value.
  if (scaled > std::numeric_limits<size_t>::max() / multiplier) {
    return 0;
  }
  return static_cast<size_t>(scaled) * multiplier;
}

// Decode-side limits. A small crafted image can declare huge
// dimensions and decode into a multi-gigabyte pixel buffer even though its
// encoded form fits under max_buffered_image_size. width/height/area bound the
// dimensions of a single decode; memory/map bound ImageMagick's pixel-cache RAM
// and disk(0) makes an over-limit decode fail as a caught Magick::Error rather
// than spilling to disk. area is sized to one full pixel cache (64 Mpixels at 8
// bytes per pixel for a Q16 build is 512 MiB) so the dimension and RAM limits
// are mutually consistent. The RAM limits are process-wide and shared across
// concurrent decodes, so under load an over-budget decode reverts to the
// original bytes rather than converting.
constexpr size_t MAX_IMAGE_DIMENSION_PX = 16000;
constexpr size_t MAX_IMAGE_AREA_PX      = 64ULL * 1024 * 1024;
constexpr size_t MAX_PIXEL_CACHE_BYTES  = 512ULL * 1024 * 1024;

// Parse an encoder quality (1-100) for the webp_quality/jpeg_quality
// arguments. Returns -1 on malformed or out-of-range input.
int
parse_quality(const char *value)
{
  errno         = 0;
  char *end     = nullptr;
  long  quality = std::strtol(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || quality < 1 || quality > 100) {
    return -1;
  }
  return static_cast<int>(quality);
}

// Map an encoding to the client-facing Content-Type it should be labeled with.
const char *
content_type_for(ImageEncoding encoding)
{
  switch (encoding) {
  case ImageEncoding::webp:
    return "image/webp";
  case ImageEncoding::jpeg:
    return "image/jpeg";
  case ImageEncoding::png:
    return "image/png";
  default:
    return nullptr;
  }
}
} // namespace

class ImageTransform : public TransformationPlugin
{
public:
  ImageTransform(Transaction &transaction, std::string input_content_type, ImageEncoding input_image_type,
                 ImageEncoding transform_image_type)
    : TransformationPlugin(transaction, TransformationPlugin::RESPONSE_TRANSFORMATION),
      _txnp(static_cast<TSHttpTxn>(transaction.getAtsHandle())),
      _input_content_type(std::move(input_content_type)),
      _input_image_type(input_image_type),
      _transform_image_type(transform_image_type)
  {
    TransformationPlugin::registerHook(HOOK_READ_RESPONSE_HEADERS);
    TransformationPlugin::registerHook(HOOK_SEND_RESPONSE_HEADERS);
  }

  void
  handleReadResponseHeaders(Transaction &transaction) override
  {
    // Content-Type is set by label_output() once the body is decoded, since
    // only then is its final encoding known.
    transaction.getServerResponse().getHeaders()["Vary"] = "Accept"; // separate cache entry per Accept
    Dbg(webp_dbg_ctl, "url %s", transaction.getServerRequest().getUrl().getUrlString().c_str());
    transaction.resume();
  }

  void
  handleSendResponseHeaders(Transaction &transaction) override
  {
    // If the body exceeded the cap we produced no transformed
    // body. The client response headers are not built until setOutputComplete,
    // so we can still turn the 200 into an error here, giving the client a
    // clear failure with an empty body instead of a truncated 200. (We cannot
    // use Transaction::error() for this; it asserts once the response is in
    // flight.)
    if (_refused) {
      Response &response = transaction.getClientResponse();
      response.setStatusCode(HTTP_STATUS_BAD_GATEWAY);
      response.setReasonPhrase("Bad Gateway");
      // This is an empty error response, not an image.
      response.getHeaders().erase("Content-Type");
      response.getHeaders().erase("Vary");
      transaction.resume();
      return;
    }
    transaction.resume();
  }

  void
  consume(std::string_view data) override
  {
    // The response body is buffered in full before being handed
    // to ImageMagick. A malicious or just unusually large origin response can
    // drive the proxy to OOM, so the buffer is bounded by a per-transaction
    // cap. When the cap is exceeded we drop what we have and stop buffering;
    // handleInputComplete then produces no body and handleSendResponseHeaders
    // turns the response into a 502. Because the transform emits nothing until
    // then, the client gets an error with an empty body rather than the
    // oversized image. Transaction::error() cannot be used here: it asserts
    // once the response is in flight.
    if (_refused) {
      return;
    }
    if (_img.size() + data.length() > max_buffered_image_size) {
      TSError("[webp_transform] response body exceeds cap %zu, returning 502", max_buffered_image_size);
      _refused = true;
      _img.clear();
      _img.shrink_to_fit();
      return;
    }
    _img.append(data.data(), data.length());
  }

  void
  handleInputComplete() override
  {
    if (_refused) {
      setOutputComplete(); // no body produced; handleSendResponseHeaders turns this into a 502
      return;
    }

    if (!has_signature_for(_img, _input_image_type)) {
      TSError("[webp_transform] input body does not match its declared image encoding: %d, length: %zu",
              static_cast<int>(_input_image_type), _img.length());
      pass_through();
      setOutputComplete();
      return;
    }

    Blob  input_blob(_img.data(), _img.length());
    Image image;

    try {
      image.read(input_blob);

      // JPEG cannot carry transparency, so a WebP with an alpha channel is
      // sent as PNG instead.
      ImageEncoding produced =
        _transform_image_type == ImageEncoding::jpeg && image.alpha() ? ImageEncoding::png : _transform_image_type;
      switch (produced) {
      case ImageEncoding::webp:
        Dbg(webp_dbg_ctl, "Transforming jpeg or png to webp");
        image.quality(config_webp_quality);
        // Otherwise quality 100 silently switches the encoder to lossless.
        image.defineValue("webp", "lossless", "false");
        image.magick("WEBP");
        break;
      case ImageEncoding::jpeg:
        Dbg(webp_dbg_ctl, "Transforming webp to jpeg");
        image.quality(config_jpeg_quality);
        image.magick("JPEG");
        break;
      default:
        Dbg(webp_dbg_ctl, "Transforming webp with transparency to png");
        // For PNG, quality selects the zlib level and filter; 75 is
        // ImageMagick's own default (level 7, adaptive filtering), rather than
        // the 100 a lossless WebP would otherwise carry over.
        image.quality(75);
        image.magick("PNG");
        break;
      }

      Blob output_blob;
      image.write(&output_blob);
      std::string_view output(static_cast<const char *>(output_blob.data()), output_blob.length());
      if (produced == ImageEncoding::webp && output.size() >= _img.size()) {
        // The client accepts the original too, so never send it a re-encode
        // that is no smaller (e.g. a low-quality source at a higher quality).
        Dbg(webp_dbg_ctl, "webp is %zu bytes, original %zu; sending the original", output.size(), _img.size());
        pass_through();
      } else {
        switch (produced) {
        case ImageEncoding::webp:
          stat_convert_to_webp.increment(1);
          break;
        case ImageEncoding::png:
          stat_convert_to_png.increment(1);
          break;
        default:
          stat_convert_to_jpeg.increment(1);
          break;
        }
        label_output(content_type_for(produced));
        produce(output);
      }
    } catch (const Magick::Warning &warning) {
      TSError("ImageMagick++ warning: %s", warning.what());
      pass_through();
    } catch (const Magick::Error &error) {
      TSError("ImageMagick++ error: %s _image_type: %d input length: %zu", error.what(), (int)_transform_image_type, _img.length());
      pass_through();
    } catch (const std::exception &e) {
      // ImageMagick++ can throw other exception types (e.g.
      // std::bad_alloc on huge or malformed inputs). Catch them so an
      // uncaught exception does not terminate the process. Log the input type
      // and buffered length so a memory-pressure attack (repeated bad_alloc on
      // large inputs) is distinguishable from a one-off decode hiccup.
      TSError("[webp_transform] std::exception during transform: %s _image_type: %d input length: %zu", e.what(),
              (int)_transform_image_type, _img.length());
      pass_through();
    } catch (...) {
      TSError("[webp_transform] unknown exception during transform _image_type: %d input length: %zu", (int)_transform_image_type,
              _img.length());
      pass_through();
    }

    setOutputComplete();
  }

  ~ImageTransform() override = default;

private:
  // Set the Content-Type of what this transform sends. The client response
  // and the cached copy are both built from the transform response when the
  // first output is produced, so this must run before produce().
  void
  label_output(std::string_view content_type)
  {
    TSMBuffer bufp;
    TSMLoc    hdr_loc;

    if (TSHttpTxnTransformRespGet(_txnp, &bufp, &hdr_loc) != TS_SUCCESS) {
      return;
    }
    TSMLoc field = TSMimeHdrFieldFind(bufp, hdr_loc, TS_MIME_FIELD_CONTENT_TYPE, TS_MIME_LEN_CONTENT_TYPE);
    if (field == TS_NULL_MLOC &&
        TSMimeHdrFieldCreateNamed(bufp, hdr_loc, TS_MIME_FIELD_CONTENT_TYPE, TS_MIME_LEN_CONTENT_TYPE, &field) == TS_SUCCESS) {
      TSMimeHdrFieldAppend(bufp, hdr_loc, field);
    }
    if (field != TS_NULL_MLOC) {
      TSMimeHdrFieldValuesClear(bufp, hdr_loc, field);
      TSMimeHdrFieldValueStringInsert(bufp, hdr_loc, field, -1, content_type.data(), content_type.size());
      TSHandleMLocRelease(bufp, hdr_loc, field);
    }
    TSHandleMLocRelease(bufp, TS_NULL_MLOC, hdr_loc);
  }

  // Send the original body, labeled with the origin's Content-Type.
  void
  pass_through()
  {
    label_output(_input_content_type);
    if (!_img.empty()) {
      produce(_img);
    }
  }

  TSHttpTxn     _txnp;
  std::string   _img;
  std::string   _input_content_type;
  bool          _refused = false;
  ImageEncoding _input_image_type;
  ImageEncoding _transform_image_type;
};

class GlobalHookPlugin : public GlobalPlugin
{
public:
  GlobalHookPlugin() { registerHook(HOOK_READ_RESPONSE_HEADERS); }
  void
  handleReadResponseHeaders(Transaction &transaction) override
  {
    // This variable stores the incoming image type
    ImageEncoding input_image_type = ImageEncoding::unknown;

    // This method tries to optimize the amount of string searching at the expense of double checking some of the booleans

    std::string ctype = transaction.getServerResponse().getHeaders().values("Content-Type");

    // Test to if in this transaction we might want to convert jpeg or png to webp
    bool transaction_convert_to_webp = false;
    if (config_convert_to_webp == true) {
      if (ctype.find("image/jpeg") != std::string::npos) {
        input_image_type            = ImageEncoding::jpeg;
        transaction_convert_to_webp = true;
      }
      if (ctype.find("image/png") != std::string::npos) {
        input_image_type            = ImageEncoding::png;
        transaction_convert_to_webp = true;
      }
    }

    // Test to if in this transaction we might want to convert webp to jpeg
    bool transaction_convert_to_jpeg = false;
    if (config_convert_to_jpeg == true && transaction_convert_to_webp == false) {
      transaction_convert_to_jpeg = ctype.find("image/webp") != std::string::npos;
      if (transaction_convert_to_jpeg) {
        input_image_type = ImageEncoding::webp;
      }
    }

    Dbg(webp_dbg_ctl, "Content-Type: %s transaction_convert_to_webp: %d transaction_convert_to_jpeg: %d", ctype.c_str(),
        transaction_convert_to_webp, transaction_convert_to_jpeg);

    // If we might need to convert check to see if what the browser supports
    if (transaction_convert_to_webp == true || transaction_convert_to_jpeg == true) {
      // When the origin advertises a body larger than the cap,
      // decline the transform up front so the original response passes through
      // untouched rather than being buffered up to the cap and then forwarded
      // under a transformed Content-Type. Bodies without a Content-Length are
      // still bounded by the per-transaction cap inside ImageTransform.
      bool        content_length_usable = false;
      std::string content_length        = transaction.getServerResponse().getHeaders().values("Content-Length");
      if (!content_length.empty()) {
        const char *cstr = content_length.c_str();
        // Reject a leading sign: strtoull() would wrap a negative value to a
        // huge size_t and spuriously decline the transform.
        bool signed_input = (*cstr == '-' || *cstr == '+');

        errno                       = 0;
        char              *end      = nullptr;
        unsigned long long declared = std::strtoull(cstr, &end, 10);

        // Require the entire header value to be a single integer (optional
        // trailing whitespace only). Headers::values() comma-joins duplicate
        // Content-Length headers, so "1,20971520" would otherwise parse as "1"
        // and bypass this up-front decline.
        while (*end == ' ' || *end == '\t') {
          ++end;
        }
        bool fully_parsed = (errno == 0) && (end != cstr) && (*end == '\0') && !signed_input;

        if (fully_parsed && declared > max_buffered_image_size) {
          Dbg(webp_dbg_ctl, "origin Content-Length %llu exceeds cap %zu, not transforming", declared, max_buffered_image_size);
          transaction.resume();
          return;
        }
        // A fully parsed Content-Length at or under the cap guarantees the body
        // fits and the transform will complete, so the result stays cacheable.
        content_length_usable = fully_parsed;
      }

      std::string accept         = transaction.getServerRequest().getHeaders().values("Accept");
      bool        webp_supported = accept.find("image/webp") != std::string::npos;
      Dbg(webp_dbg_ctl, "Accept: %s webp_suppported: %d", accept.c_str(), webp_supported);

      // Without a usable Content-Length the body may exceed the cap mid-stream
      // and be refused, which yields an empty 502 to the client. The cacheable
      // object would be the origin 200 plus the empty transform output relabeled
      // with the target Content-Type, so mark such responses no-store to keep a
      // poisoned 200/empty-body entry out of the cache. Bodies declared over the
      // cap are declined above; bodies at or under the cap transform fully and
      // remain cacheable.
      if (webp_supported == true && transaction_convert_to_webp == true) {
        Dbg(webp_dbg_ctl, "Content type is either jpeg or png. Converting to webp");
        if (!content_length_usable) {
          TSHttpTxnServerRespNoStoreSet(static_cast<TSHttpTxn>(transaction.getAtsHandle()), 1);
        }
        transaction.addPlugin(new ImageTransform(transaction, std::move(ctype), input_image_type, ImageEncoding::webp));
      } else if (webp_supported == false && transaction_convert_to_jpeg == true) {
        Dbg(webp_dbg_ctl, "Content type is webp. Converting to jpeg");
        if (!content_length_usable) {
          TSHttpTxnServerRespNoStoreSet(static_cast<TSHttpTxn>(transaction.getAtsHandle()), 1);
        }
        transaction.addPlugin(new ImageTransform(transaction, std::move(ctype), input_image_type, ImageEncoding::jpeg));
      } else {
        Dbg(webp_dbg_ctl, "Nothing to convert");
      }
    }

    transaction.resume();
  }
};

void
TSPluginInit(int argc, const char *argv[])
{
  if (!RegisterGlobalPlugin("CPP_Webp_Transform", "apache", "dev@trafficserver.apache.org")) {
    return;
  }

  constexpr std::string_view max_buffer_prefix   = "max_buffer_size=";
  constexpr std::string_view webp_quality_prefix = "webp_quality=";
  constexpr std::string_view jpeg_quality_prefix = "jpeg_quality=";

  bool convert_specified = false;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    bool             recognized = false;
    // Independent checks (not mutually exclusive) so the legacy comma-combined
    // form "convert_to_jpeg,convert_to_webp" in a single argument still enables
    // both directions.
    if (arg.find("convert_to_webp") != std::string_view::npos) {
      Dbg(webp_dbg_ctl, "Configured to convert to webp");
      config_convert_to_webp = true;
      convert_specified      = true;
      recognized             = true;
    }
    if (arg.find("convert_to_jpeg") != std::string_view::npos) {
      Dbg(webp_dbg_ctl, "Configured to convert to jpeg");
      config_convert_to_jpeg = true;
      convert_specified      = true;
      recognized             = true;
    }
    if (arg.substr(0, max_buffer_prefix.size()) == max_buffer_prefix) {
      size_t parsed = parse_size(argv[i] + max_buffer_prefix.size());
      if (parsed == 0) {
        TSError("[webp_transform] invalid %.*s, keeping default %zu bytes", static_cast<int>(arg.size()), arg.data(),
                max_buffered_image_size);
      } else {
        max_buffered_image_size = parsed;
        Dbg(webp_dbg_ctl, "max buffered image size set to %zu bytes", max_buffered_image_size);
      }
      recognized = true;
    }
    for (auto [prefix, target] : {
           std::pair{webp_quality_prefix, &config_webp_quality},
           std::pair{jpeg_quality_prefix, &config_jpeg_quality}
    }) {
      if (arg.substr(0, prefix.size()) == prefix) {
        if (int quality = parse_quality(argv[i] + prefix.size()); quality < 0) {
          TSError("[webp_transform] invalid %.*s, keeping default %d", static_cast<int>(arg.size()), arg.data(), *target);
        } else {
          *target = quality;
        }
        recognized = true;
      }
    }
    if (!recognized) {
      TSError("[webp_transform] unknown option: %.*s", static_cast<int>(arg.size()), arg.data());
    }
  }

  // If no conversion direction was named, default to converting both, matching
  // the no-argument behavior.
  if (!convert_specified) {
    Dbg(webp_dbg_ctl, "No conversion direction given; converting both webp and jpeg");
    config_convert_to_webp = true;
    config_convert_to_jpeg = true;
  }

  stat_convert_to_webp.init("plugin." TAG ".convert_to_webp", Stat::SYNC_SUM, false);
  stat_convert_to_jpeg.init("plugin." TAG ".convert_to_jpeg", Stat::SYNC_SUM, false);
  stat_convert_to_png.init("plugin." TAG ".convert_to_png", Stat::SYNC_SUM, false);
  Dbg(webp_dbg_ctl, "webp_quality=%d jpeg_quality=%d", config_webp_quality, config_jpeg_quality);

  InitializeMagick("");

  // Bound the decode so a small image declaring huge dimensions
  // cannot decode into a multi-gigabyte pixel buffer. disk(0) turns an
  // over-budget decode into a fast Magick::Error (which we catch and revert)
  // rather than letting ImageMagick spill the oversized pixel cache to disk.
  // See the limit constants above for how the dimension and RAM caps line up.
  Magick::ResourceLimits::width(MAX_IMAGE_DIMENSION_PX);
  Magick::ResourceLimits::height(MAX_IMAGE_DIMENSION_PX);
  Magick::ResourceLimits::area(MAX_IMAGE_AREA_PX);       // max width*height in pixels held in the cache
  Magick::ResourceLimits::memory(MAX_PIXEL_CACHE_BYTES); // heap pixel-cache budget
  Magick::ResourceLimits::map(MAX_PIXEL_CACHE_BYTES);    // memory-mapped pixel-cache budget
  Magick::ResourceLimits::disk(0);                       // no disk-backed spill; fail rather than thrash

  new GlobalHookPlugin();
}
