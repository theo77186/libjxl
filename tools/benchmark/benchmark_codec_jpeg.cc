// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.
#include "tools/benchmark/benchmark_codec_jpeg.h"

#include <stddef.h>
#include <stdio.h>
// After stddef/stdio
#include <stdint.h>
#include <string.h>

#include <numeric>  // partial_sum
#include <string>

#include "lib/extras/dec/jpg.h"
#include "lib/extras/dec/jxl.h"
#include "lib/extras/enc/jpg.h"
#include "lib/extras/enc/jxl.h"
#include "lib/extras/encode_jpeg.h"
#include "lib/extras/packed_image.h"
#include "lib/extras/packed_image_convert.h"
#include "lib/extras/time.h"
#include "lib/jxl/base/padded_bytes.h"
#include "lib/jxl/base/span.h"
#include "lib/jxl/base/thread_pool_internal.h"
#include "lib/jxl/codec_in_out.h"
#include "tools/cmdline.h"

namespace jxl {

namespace {

struct JPEGArgs {
  std::string jpeg_encoder = "libjpeg";
  std::string chroma_subsampling = "444";
};

JPEGArgs* const jpegargs = new JPEGArgs;

}  // namespace

Status AddCommandLineOptionsJPEGCodec(BenchmarkArgs* args) {
  args->cmdline.AddOptionValue(
      '\0', "chroma_subsampling", "444/422/420/411",
      "default JPEG chroma subsampling (default: 444).",
      &jpegargs->chroma_subsampling, &jpegxl::tools::ParseString);
  return true;
}

class JPEGCodec : public ImageCodec {
 public:
  explicit JPEGCodec(const BenchmarkArgs& args) : ImageCodec(args) {
    jpeg_encoder_ = jpegargs->jpeg_encoder;
    chroma_subsampling_ = jpegargs->chroma_subsampling;
  }

  Status ParseParam(const std::string& param) override {
    if (ImageCodec::ParseParam(param)) {
      return true;
    }
    if (param == "sjpeg" || param == "libjxl") {
      jpeg_encoder_ = param;
      return true;
    }
    if (param == "djxl8") {
      use_jxl_decoder_ = true;
      jxl_decoder_data_type_ = JXL_TYPE_UINT8;
      return true;
    }
    if (param == "djxl16") {
      use_jxl_decoder_ = true;
      jxl_decoder_data_type_ = JXL_TYPE_UINT16;
      return true;
    }
    if (param.compare(0, 3, "yuv") == 0) {
      if (param.size() != 6) return false;
      chroma_subsampling_ = param.substr(3);
      return true;
    } else if (param.substr(0, 2) == "nr") {
      normalize_bitrate_ = true;
      return true;
    }
    return false;
  }

  Status Compress(const std::string& filename, const CodecInOut* io,
                  ThreadPoolInternal* pool, std::vector<uint8_t>* compressed,
                  jpegxl::tools::SpeedStats* speed_stats) override {
    double elapsed = 0.0;
    if (jpeg_encoder_ != "libjxl" || normalize_bitrate_) {
      extras::PackedPixelFile ppf;
      JxlPixelFormat format = {0, JXL_TYPE_UINT8, JXL_BIG_ENDIAN, 0};
      JXL_RETURN_IF_ERROR(ConvertCodecInOutToPackedPixelFile(
          *io, format, io->metadata.m.color_encoding, pool, &ppf));
      extras::EncodedImage encoded;
      std::unique_ptr<extras::Encoder> encoder = extras::GetJPEGEncoder();
      std::ostringstream os;
      os << static_cast<int>(std::round(q_target_));
      encoder->SetOption("q", os.str());
      std::string jpeg_encoder = normalize_bitrate_ ? "libjpeg" : jpeg_encoder_;
      encoder->SetOption("jpeg_encoder", jpeg_encoder);
      encoder->SetOption("chroma_subsampling", chroma_subsampling_);
      const double start = Now();
      JXL_RETURN_IF_ERROR(encoder->Encode(ppf, &encoded, pool));
      const double end = Now();
      elapsed = end - start;
      *compressed = encoded.bitstreams.back();
    }
    if (jpeg_encoder_ == "libjxl") {
      size_t target_size = normalize_bitrate_ ? compressed->size() : 0;
      compressed->clear();
      const double start = Now();
      JXL_RETURN_IF_ERROR(extras::EncodeJpeg(
          io->Main(), target_size, butteraugli_target_, pool, compressed));
      const double end = Now();
      elapsed = end - start;
    }
    speed_stats->NotifyElapsed(elapsed);
    return true;
  }

  Status Decompress(const std::string& filename,
                    const Span<const uint8_t> compressed,
                    ThreadPoolInternal* pool, CodecInOut* io,
                    jpegxl::tools::SpeedStats* speed_stats) override {
    extras::PackedPixelFile ppf;
    if (use_jxl_decoder_) {
      extras::JXLCompressParams cparams;
      cparams.AddOption(JXL_ENC_FRAME_SETTING_JPEG_RECON_CFL, 0);
      std::vector<uint8_t> jpeg_bytes(compressed.data(),
                                      compressed.data() + compressed.size());
      const double start = Now();
      std::vector<uint8_t> jxl_bytes;
      JXL_RETURN_IF_ERROR(
          extras::EncodeImageJXL(cparams, ppf, &jpeg_bytes, &jxl_bytes));
      extras::JXLDecompressParams dparams;
      for (uint32_t num_channels : {1, 3}) {
        dparams.accepted_formats.push_back(JxlPixelFormat{
            num_channels, jxl_decoder_data_type_, JXL_BIG_ENDIAN, 0});
      }
      JXL_RETURN_IF_ERROR(extras::DecodeImageJXL(
          jxl_bytes.data(), jxl_bytes.size(), dparams, nullptr, &ppf, nullptr));
      const double end = Now();
      speed_stats->NotifyElapsed(end - start);
    } else {
      const double start = Now();
      JXL_RETURN_IF_ERROR(DecodeImageJPG(compressed, extras::ColorHints(),
                                         SizeConstraints(), &ppf));
      const double end = Now();
      speed_stats->NotifyElapsed(end - start);
    }
    JXL_RETURN_IF_ERROR(ConvertPackedPixelFileToCodecInOut(ppf, pool, io));
    return true;
  }

 protected:
  bool normalize_bitrate_ = false;
  std::string jpeg_encoder_;
  std::string chroma_subsampling_;
  bool use_jxl_decoder_ = false;
  JxlDataType jxl_decoder_data_type_ = JXL_TYPE_UINT8;
};

ImageCodec* CreateNewJPEGCodec(const BenchmarkArgs& args) {
  return new JPEGCodec(args);
}

}  // namespace jxl
