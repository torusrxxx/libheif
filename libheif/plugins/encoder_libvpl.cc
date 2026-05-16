//==============================================================================
// Copyright Intel Corporation
//
// SPDX-License-Identifier: MIT
//==============================================================================
// Example using Intel® Video Processing Library (Intel® VPL)

///
/// Utility library header file for sample code
///
/// @file

#include "hardware_intel_qsv.h"

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#endif
/*
 * HEIF codec.
 * Copyright (c) 2017 Dirk Farin <dirk.farin@gmail.com>
 *
 * This file is part of libheif.
 *
 * libheif is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libheif is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libheif.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "libheif/heif.h"
#include "libheif/heif_plugin.h"
#include "encoder_libvpl.h"
#include <cassert>
#include <string>
#include <deque>


#define intelvpl_buffer_max_size 4096
struct intelvpl_encoder
{
  bool initialized = false;
  mfxVideoParam encodeParams = {};
  mfxU32 codecId = 0;

  // --- output

  struct Packet
  {
    std::vector<uint8_t> data;
    uintptr_t frameNr = 0;
  };

  std::deque<Packet> output_data;

  std::vector<uint8_t> active_data; // holds the data that we just returned
  mfxBitstream bitstream = {};
  mfxSyncPoint syncp = {};
  uint32_t frameNr = 0;
  int quality = 50;
  // Prepare output bitstream
  std::vector<uint8_t> alldata;

  intelvpl_encoder() {
    bitstream.MaxLength = 1 << 20; // TODO
    alldata.resize(bitstream.MaxLength);
    bitstream.Data = alldata.data();
    memset(bitstream.Data, 0, bitstream.MaxLength);
  }
  bool doubleSize() {
    if (bitstream.MaxLength < (1 << 30)) {
      bitstream.MaxLength <<= 1;
      alldata.resize(bitstream.MaxLength);
      bitstream.Data = alldata.data();
      return true;
    }
    else {
      // ???
      return false;
    }
  }
};

static const char kEmptyString[] = "";
static const char kSuccess[] = "Success";

static const int INTELVPL_PLUGIN_PRIORITY = 100;

static const char* intelvpl_HEVC_plugin_name()
{
  return "Intel QSV HEVC encoder";
}

static const char* intelvpl_AVC_plugin_name()
{
  return "Intel QSV AVC encoder";
}

static const char* intelvpl_AV1_plugin_name()
{
  return "Intel QSV AV1 encoder";
}

static const char* intelvpl_JPEG_plugin_name()
{
    return "Intel QSV JPEG encoder";
}

extern mfxLoader loader;
extern mfxSession session;

bool video_encode_initialized = false;
extern bool video_decode_initialized;

#define MAX_NPARAMETERS 10

static heif_encoder_parameter intelvpl_encoder_params[MAX_NPARAMETERS];
static const heif_encoder_parameter* intelvpl_encoder_parameter_ptrs[MAX_NPARAMETERS + 1];

class intelvpl_surface_mapper {
  mfxFrameSurface1* surface;
  mfxStatus sts;
public:
  intelvpl_surface_mapper(mfxFrameSurface1* surface, mfxMemoryFlags access) : surface(surface) {
    sts = surface->FrameInterface->Map(surface, access);
  }
  mfxStatus status() const {
    return sts;
  }
  ~intelvpl_surface_mapper() {
    if (sts != MFX_ERR_NONE)
      surface->FrameInterface->Unmap(surface);
  }
};

static void intelvpl_init_plugin()
{
  // Initialize session
  if (!loader)
    loader = MFXLoad();
  if (!loader)
    return;

  heif_encoder_parameter* p = intelvpl_encoder_params;
  const heif_encoder_parameter** d = intelvpl_encoder_parameter_ptrs;
  int i = 0;

  assert(i < MAX_NPARAMETERS);
  p->version = 2;
  p->name = heif_encoder_parameter_name_quality;
  p->type = heif_encoder_parameter_type_integer;
  p->integer.default_value = 50;
  p->has_default = true;
  p->integer.have_minimum_maximum = true;
  p->integer.minimum = 0;
  p->integer.maximum = 100;
  p->integer.valid_values = NULL;
  p->integer.num_valid_values = 0;
  d[i++] = p++;

  d[i++] = nullptr;
}

static void intelvpl_cleanup_plugin()
{
  if (session) {
    if (video_encode_initialized) {
      MFXVideoENCODE_Close(session);
      video_encode_initialized = false;
    }
    if (!video_decode_initialized) {
      MFXClose(session);
      session = NULL;
    }
  }
  if (loader && !session) {
    MFXUnload(loader);
    loader = NULL;
  }
}

const heif_encoder_parameter** intelvpl_list_parameters(void* encoder)
{
  return intelvpl_encoder_parameter_ptrs;
}

void intelvpl_query_encoded_size(void* encoder_raw, uint32_t input_width, uint32_t input_height,
  uint32_t* encoded_width, uint32_t* encoded_height)
{
  *encoded_width = (input_width + 7) & ~0x7U;
  *encoded_height = (input_height + 7) & ~0x7U;
}

static inline mfxExtVideoSignalInfo heif_to_qsv(const heif_color_profile_nclx& nclx) {
  mfxExtVideoSignalInfo nclx_info = {};
  nclx_info.Header.BufferId = MFX_EXTBUFF_VIDEO_SIGNAL_INFO;
  nclx_info.Header.BufferSz = sizeof(nclx_info);
  nclx_info.MatrixCoefficients = nclx.matrix_coefficients;
  nclx_info.ColourPrimaries = nclx.color_primaries;
  nclx_info.TransferCharacteristics = nclx.transfer_characteristics;
  nclx_info.VideoFullRange = nclx.full_range_flag;
  nclx_info.ColourDescriptionPresent = 1;
  return nclx_info;
}

static inline mfxExtContentLightLevelInfo heif_to_qsv(const heif_content_light_level& cll) {
  mfxExtContentLightLevelInfo cll_info = {};
  cll_info.Header.BufferId = MFX_EXTBUFF_CONTENT_LIGHT_LEVEL_INFO;
  cll_info.Header.BufferSz = sizeof(cll_info);
  cll_info.InsertPayloadToggle = 1;
  cll_info.MaxContentLightLevel = cll.max_content_light_level;
  cll_info.MaxPicAverageLightLevel = cll.max_pic_average_light_level;
  return cll_info;
}

static inline mfxExtMasteringDisplayColourVolume heif_to_qsv(const heif_mastering_display_colour_volume& mdcv) {
  mfxExtMasteringDisplayColourVolume mdcv_info = {};
  mdcv_info.Header.BufferId = MFX_EXTBUFF_MASTERING_DISPLAY_COLOUR_VOLUME;
  mdcv_info.Header.BufferSz = sizeof(mdcv_info);
  mdcv_info.DisplayPrimariesX[0] = mdcv.display_primaries_x[0];
  mdcv_info.DisplayPrimariesY[0] = mdcv.display_primaries_y[0];
  mdcv_info.DisplayPrimariesX[1] = mdcv.display_primaries_x[1];
  mdcv_info.DisplayPrimariesY[1] = mdcv.display_primaries_y[1];
  mdcv_info.DisplayPrimariesX[2] = mdcv.display_primaries_x[2];
  mdcv_info.DisplayPrimariesY[2] = mdcv.display_primaries_y[2];
  mdcv_info.WhitePointX = mdcv.white_point_x;
  mdcv_info.WhitePointY = mdcv.white_point_y;
  mdcv_info.MaxDisplayMasteringLuminance = mdcv.max_display_mastering_luminance;
  mdcv_info.MinDisplayMasteringLuminance = mdcv.min_display_mastering_luminance;
  mdcv_info.InsertPayloadToggle = 1;
  return mdcv_info;
}

static heif_error intelvpl_init_session(mfxU32 codecId) {
  if (session != NULL)
    return { heif_error_Ok, heif_suberror_Unspecified, kSuccess };

  // variables used only in 2.x version
  mfxStatus sts;
  mfxConfig cfg[3];
  mfxVariant cfgVal[3];
  // Implementation used must be the type requested from command line
  cfg[0] = MFXCreateConfig(loader);
  if (NULL == cfg[0]) {
    return {
        heif_error_Encoder_plugin_error,
        heif_suberror_Plugin_loading_error,
        "MFXCreateConfig failed"
    };
  }
  cfgVal[0].Type = MFX_VARIANT_TYPE_U32;
  cfgVal[0].Data.U32 = MFX_IMPL_TYPE_HARDWARE;
  sts = MFXSetConfigFilterProperty(cfg[0], (mfxU8*)"mfxImplDescription.Impl", cfgVal[0]);
  if (NULL == cfg[0]) {
    return {
        heif_error_Encoder_plugin_error,
        heif_suberror_Plugin_loading_error,
        "MFXSetConfigFilterProperty failed for Impl"
    };
  }

  // Implementation must provide an HEVC encoder
  cfg[1] = MFXCreateConfig(loader);
  if (NULL == cfg[1]) {
    return {
        heif_error_Encoder_plugin_error,
        heif_suberror_Plugin_loading_error,
        "MFXCreateConfig failed"
    };
  }
  cfgVal[1].Type = MFX_VARIANT_TYPE_U32;
  cfgVal[1].Data.U32 = codecId;
  sts = MFXSetConfigFilterProperty(
    cfg[1],
    (mfxU8*)"mfxImplDescription.mfxEncoderDescription.encoder.CodecID",
    cfgVal[1]);
  if (MFX_ERR_NONE != sts) {
    return {
        heif_error_Encoder_plugin_error,
        heif_suberror_Plugin_loading_error,
        "MFXSetConfigFilterProperty failed for encoder CodecID"
    };
  }

  // Implementation used must provide API version 2.2 or newer
  cfg[2] = MFXCreateConfig(loader);
  if (NULL == cfg[2]) {
    return {
        heif_error_Encoder_plugin_error,
        heif_suberror_Plugin_loading_error,
        "MFXCreateConfig failed"
    };
  }
  cfgVal[2].Type = MFX_VARIANT_TYPE_U32;
  cfgVal[2].Data.U32 = VPLVERSION(MAJOR_API_VERSION_REQUIRED, MINOR_API_VERSION_REQUIRED);
  sts = MFXSetConfigFilterProperty(cfg[2],
    (mfxU8*)"mfxImplDescription.ApiVersion.Version",
    cfgVal[2]);
  if (MFX_ERR_NONE != sts) {
    return {
        heif_error_Encoder_plugin_error,
        heif_suberror_Plugin_loading_error,
        "MFXSetConfigFilterProperty failed for API version"
    };
  }

  sts = MFXCreateSession(loader, 0, &session);
  if (MFX_ERR_NONE != sts) {
    return {
        heif_error_Encoder_plugin_error,
        heif_suberror_Plugin_loading_error,
        "Cannot create session -- no implementations meet selection criteria"
    };
  }
  return { heif_error_Ok, heif_suberror_Unspecified, kSuccess };
}

// Create a new encoder context for encoding an image

static heif_error intelvpl_new_encoder(mfxU32 codecId, void** enc)
{
  heif_error err = intelvpl_init_session(codecId);
  if (err.code)
    return err;
  intelvpl_encoder* encoder = new intelvpl_encoder();
  encoder->codecId = codecId;
  *enc = encoder;

  return err;
}

static heif_error intelvpl_new_encoder_HEVC(void** enc)
{
  return intelvpl_new_encoder(MFX_CODEC_HEVC, enc);
}

static heif_error intelvpl_new_encoder_AVC(void** enc)
{
  return intelvpl_new_encoder(MFX_CODEC_AVC, enc);
}

static heif_error intelvpl_new_encoder_AV1(void** enc)
{
    return intelvpl_new_encoder(MFX_CODEC_AV1, enc);
}

static heif_error intelvpl_new_encoder_JPEG(void** enc)
{
    return intelvpl_new_encoder(MFX_CODEC_JPEG, enc);
}

static void intelvpl_free_encoder(void* encoder_raw)
{
  intelvpl_encoder* encoder = (intelvpl_encoder*)encoder_raw;
  mfxStatus sts;
  //MFXVideoencode_Close(session);
  delete encoder;
}

static void append_chunk_data(intelvpl_encoder* encoder, uintptr_t pts)
{
  mfxBitstream& bs = encoder->bitstream;
  if (bs.DataLength == 0) {
    return;
  }
  assert(bs.DataLength + bs.DataOffset < bs.MaxLength);

  //std::vector<uint8_t>& pktdata = encoder->output_data.front().data;
  for (;;)
  {
    if (encoder->codecId == MFX_CODEC_HEVC || encoder->codecId == MFX_CODEC_AVC) {
    // AVC, HEVC have start codes
    mfxU32 startIndex = 0;
    while (startIndex < bs.DataLength - 3 &&
      (bs.Data[bs.DataOffset + startIndex] != 0 ||
        bs.Data[bs.DataOffset + startIndex + 1] != 0 ||
        bs.Data[bs.DataOffset + startIndex + 2] != 1)) {
      startIndex++;
    }

    mfxU32 end_idx = startIndex + 1;

    while (end_idx < bs.DataLength - 3 &&
      (bs.Data[bs.DataOffset + end_idx] != 0 ||
        bs.Data[bs.DataOffset + end_idx + 1] != 0 ||
        bs.Data[bs.DataOffset + end_idx + 2] != 1)) {
      end_idx++;
    }

    if (end_idx == bs.DataLength - 3) {
      end_idx = bs.DataLength;
    }

    intelvpl_encoder::Packet pkt;
    pkt.data.resize(end_idx - startIndex - 3);
    memcpy(pkt.data.data(), bs.Data + bs.DataOffset + startIndex + 3, pkt.data.size());
    pkt.frameNr = pts;

    //std::cout << "append frameNr=" << pts << " NAL:" << ((int)pkt.data[1]>>3) << " size:" << pkt.data.size() << "\n";

    encoder->output_data.emplace_back(std::move(pkt));
    if (end_idx == bs.DataLength) {
        bs.DataOffset = 0;
        bs.DataLength = 0;
        break;
    }

    bs.DataOffset += end_idx;
    bs.DataLength -= end_idx - startIndex;
    }
    else {
    // AV1 has no start codes
    intelvpl_encoder::Packet pkt;
    pkt.data.resize(bs.DataLength);
    memcpy(pkt.data.data(), bs.Data + bs.DataOffset, pkt.data.size());
    pkt.frameNr = pts;

    //std::cout << "append frameNr=" << pts << " NAL:" << ((int)pkt.data[1]>>3) << " size:" << pkt.data.size() << "\n";

    encoder->output_data.emplace_back(std::move(pkt));
    bs.DataOffset = 0;
    bs.DataLength = 0;
    }

  }
  if (bs.DataLength != 0) {
    memmove(bs.Data, bs.Data + bs.DataOffset, bs.DataLength);
  }
  bs.DataOffset = 0;
}

static heif_error intelvpl_start_sequence_encoding_intern(void* encoder_raw, const heif_image* image,
  enum heif_image_input_class input_class,
  uint32_t framerate_num, uint32_t framerate_denom,
  const heif_sequence_encoding_options* options,
  bool image_sequence)
{
  intelvpl_encoder* encoder = (intelvpl_encoder*)encoder_raw;
  mfxStatus sts = MFX_ERR_NONE;

  int bit_depth = heif_image_get_bits_per_pixel_range(image, heif_channel_Y);

  heif_chroma chroma = heif_image_get_chroma_format(image);
  bool isGreyscale = (heif_image_get_colorspace(image) == heif_colorspace_monochrome);

  int input_width = heif_image_get_width(image, heif_channel_Y);
  int input_height = heif_image_get_height(image, heif_channel_Y);

  // Initialize encode parameters
  mfxVideoParam encodeParams = {};
  encodeParams.mfx.CodecId = encoder->codecId;
  encodeParams.mfx.FrameInfo.FrameRateExtN = framerate_num;
  encodeParams.mfx.FrameInfo.FrameRateExtD = framerate_denom;
  if (encoder->codecId != MFX_CODEC_JPEG) {
    encodeParams.mfx.TargetUsage = MFX_TARGETUSAGE_BALANCED;
    if (options && options->keyframe_distance_max) {
      encodeParams.mfx.GopPicSize = options->keyframe_distance_max;
    }

    if (image_sequence && options) {
      switch (options->gop_structure) {
      case heif_sequence_gop_structure_intra_only:
        encodeParams.mfx.GopPicSize = 1;
        break;
      case heif_sequence_gop_structure_lowdelay:
        encodeParams.mfx.GopPicSize = 1; // TODO
        break;
      case heif_sequence_gop_structure_unrestricted:
        break;
      }
    }
    else {
      encodeParams.mfx.GopPicSize = 1;
    }
    int qp; // constant quantization parameter
    qp = encoder->quality;
    qp = std::max(qp, 0);
    qp = std::min(qp, 100);
    qp = ((100 - qp) * 63 + 50) / 100;
    encodeParams.mfx.QPI = qp;
    encodeParams.mfx.QPP = qp;
    encodeParams.mfx.QPB = qp;
    encodeParams.mfx.RateControlMethod = MFX_RATECONTROL_CQP;
  }
  else {
    encodeParams.mfx.Interleaved = 0;
    int qp; // constant quantization parameter
    qp = encoder->quality;
    qp = std::max(qp, 0);
    qp = std::min(qp, 100);
    encodeParams.mfx.Quality = qp;
  }
  if (isGreyscale) {
    encodeParams.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_MONOCHROME;
    encodeParams.mfx.FrameInfo.FourCC = MFX_FOURCC_P8;
    return heif_error{
      heif_error_Encoder_plugin_error,
      heif_suberror_Unsupported_image_type,
      "Unsupported bitdepth"
    };
  }
  else if (chroma == heif_chroma_420) {
    encodeParams.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    encodeParams.mfx.FrameInfo.BitDepthLuma = bit_depth;
    encodeParams.mfx.FrameInfo.BitDepthChroma = bit_depth;
    if (bit_depth == 8) {
      encodeParams.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
    }
    else if (bit_depth == 10) {
      encodeParams.mfx.FrameInfo.FourCC = MFX_FOURCC_P010;
    }
    else {
      return heif_error{
        heif_error_Encoder_plugin_error,
        heif_suberror_Unsupported_image_type,
        "Unsupported bitdepth"
      };
    }
  }
  else if (chroma == heif_chroma_422) {
    encodeParams.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV422;
    if (bit_depth == 8) {
      encodeParams.mfx.FrameInfo.FourCC = MFX_FOURCC_YUY2;
    }
    else if (bit_depth == 10) {
      encodeParams.mfx.FrameInfo.FourCC = MFX_FOURCC_Y210;
    }
    else {
      return heif_error{
        heif_error_Encoder_plugin_error,
        heif_suberror_Unsupported_image_type,
        "Unsupported bitdepth"
      };
    }
  }
  else if (chroma == heif_chroma_444) {
    encodeParams.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV444;
    encodeParams.mfx.FrameInfo.FourCC = MFX_FOURCC_AYUV;
    return heif_error{
      heif_error_Encoder_plugin_error,
      heif_suberror_Unsupported_image_type,
      "Unsupported bitdepth"
    };
  }
  else {
    return heif_error{
      heif_error_Encoder_plugin_error,
      heif_suberror_Unsupported_image_type,
      "Unsupported Chroma"
    };
  }
  encodeParams.mfx.FrameInfo.CropW = input_width;
  encodeParams.mfx.FrameInfo.CropH = input_height;
  encodeParams.mfx.FrameInfo.Width = ALIGN16(input_width);
  encodeParams.mfx.FrameInfo.Height = ALIGN16(input_height);

  encodeParams.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;
  // add SEI metadata
  encodeParams.NumExtParam = 0;
  if (encoder->codecId != MFX_CODEC_JPEG) {
    mfxExtBuffer* extBuffer[3];
    mfxExtVideoSignalInfo nclx_info = {};
    mfxExtContentLightLevelInfo cll_info = {};
    mfxExtMasteringDisplayColourVolume mdcv_info = {};
    heif_color_profile_nclx* nclx;
    heif_error get_metadata_err = heif_image_get_nclx_color_profile(image, &nclx);
    if (get_metadata_err.code == 0 && nclx) {
      nclx_info = heif_to_qsv(*nclx);
      heif_nclx_color_profile_free(nclx);
      extBuffer[encodeParams.NumExtParam] = (mfxExtBuffer*)&nclx_info;
      encodeParams.NumExtParam++;
    }
    if (heif_image_has_content_light_level(image)) {
      heif_content_light_level cll = { 0, 0 };
      heif_image_get_content_light_level(image, &cll);
      if (cll.max_content_light_level > 0 || cll.max_pic_average_light_level > 0) {
        cll_info = heif_to_qsv(cll);
        extBuffer[encodeParams.NumExtParam] = (mfxExtBuffer*)&cll_info;
        encodeParams.NumExtParam++;
      }
    }
    if (heif_image_has_mastering_display_colour_volume(image)) {
      heif_mastering_display_colour_volume mdcv = {};
      heif_image_get_mastering_display_colour_volume(image, &mdcv);
      mdcv_info = heif_to_qsv(mdcv);
      extBuffer[encodeParams.NumExtParam] = (mfxExtBuffer*)&mdcv_info;
      encodeParams.NumExtParam++;
    }
    encodeParams.ExtParam = extBuffer;
  }


  // --- encode headers

  // Not needed. Headers are also output by kvazaar together with the images.


  // Initialize encoder
  if (!video_encode_initialized) {
    // Crash here???
    // Validate video encode parameters
    // - In this example the validation result is written to same structure
    // - MFX_WRN_INCOMPATIBLE_VIDEO_PARAM is returned if some of the video parameters are not supported,
    //   instead the encoder will select suitable parameters closest matching the requested configuration,
    //   and it's ignorable.
    sts = MFXVideoENCODE_Query(session, &encodeParams, &encodeParams);
    if (sts == MFX_WRN_INCOMPATIBLE_VIDEO_PARAM)
      sts = MFX_ERR_NONE;
    if (MFX_ERR_NONE != sts) {
      return heif_error{
        heif_error_Encoder_plugin_error,
        heif_suberror_Encoder_encoding,
        "Encode query failed"
      };
    }
    sts = MFXVideoENCODE_Init(session, &encodeParams);
  }
  else {
    //sts = MFXVideoENCODE_Reset(session, &encodeParams); // This does not produce a SPS NAL Unit for the next frame/tile.
    MFXVideoENCODE_Close(session);
    video_encode_initialized = false;
    sts = MFXVideoENCODE_Init(session, &encodeParams);
  }
  if (MFX_ERR_NONE != sts) {
    return heif_error{
      heif_error_Encoder_plugin_error,
      heif_suberror_Encoder_encoding,
      "Encode init failed"
    };
  }
  video_encode_initialized = true;
  return heif_error_ok;
}


static heif_error intelvpl_start_sequence_encoding(void* encoder_raw, const heif_image* image,
  enum heif_image_input_class input_class,
  uint32_t framerate_num, uint32_t framerate_denom,
  const heif_sequence_encoding_options* options)
{
  return intelvpl_start_sequence_encoding_intern(encoder_raw, image, input_class, framerate_num, framerate_denom, options, true);
}



static heif_error intelvpl_encode_sequence_frame(void* encoder_raw, const heif_image* image,
  uintptr_t frame_nr)
{
  intelvpl_encoder* encoder = (intelvpl_encoder*)encoder_raw;

  // Note: it is ok to cast away the const, as the image content is not changed.
  // However, we have to guarantee that there are no plane pointers or stride values kept over calling the svt_encode_image() function.
  /*
  err = heif_image_extend_padding_to_size(const_cast<struct heif_image*>(image),
                                          param->sourceWidth,
                                          param->sourceHeight);
  if (err.code) {
    return err;
  }
*/
  mfxStatus sts;
  bool isGreyscale = (heif_image_get_colorspace(image) == heif_colorspace_monochrome);
  heif_chroma chroma = heif_image_get_chroma_format(image);

  int input_width = heif_image_get_width(image, heif_channel_Y);
  int input_height = heif_image_get_height(image, heif_channel_Y);

  uint32_t encoded_width, encoded_height;
  intelvpl_query_encoded_size(encoder_raw, input_width, input_height, &encoded_width, &encoded_height);
  mfxFrameSurface1* encSurfaceIn = NULL;
  sts = MFXMemory_GetSurfaceForEncode(session, &encSurfaceIn);
  //VERIFY(MFX_ERR_NONE == sts, "Could not get encode surface");
  {
    intelvpl_surface_mapper mapper(encSurfaceIn, MFX_MAP_WRITE);
    sts = mapper.status();
    if (sts != MFX_ERR_NONE) {
      return {
        heif_error_Encoder_plugin_error,
        heif_suberror_Unsupported_bit_depth,
        "Memory mapping failed"
      };
    }

    int bit_depth = heif_image_get_bits_per_pixel_range(image, heif_channel_Y);
    int bit_depth_chroma = heif_image_get_bits_per_pixel_range(image, heif_channel_Cb);

    if (bit_depth != bit_depth_chroma) {
      return {
        heif_error_Encoder_plugin_error,
        heif_suberror_Unsupported_bit_depth,
        "Luma bit depth must equal the chroma bit depth"
      };
    }

    size_t stride[3];
    const uint8_t* data[3];

    data[0] = heif_image_get_plane_readonly2(image, heif_channel_Y, &stride[0]);
    data[1] = heif_image_get_plane_readonly2(image, heif_channel_Cb, &stride[1]);
    data[2] = heif_image_get_plane_readonly2(image, heif_channel_Cr, &stride[2]);

    size_t pitch = encSurfaceIn->Data.PitchHigh << 16 | encSurfaceIn->Data.PitchLow; // Bytes bewteen two rows
    switch (encSurfaceIn->Info.FourCC) {
    case MFX_FOURCC_NV12: {// YYYY .... UVUV ....
      if (pitch == stride[0]) {
        memcpy(encSurfaceIn->Data.Y, data[0], input_height * pitch);
      }
      else {
        for (int y = 0; y < input_height; y++) {
          for (int x = 0; x < input_width; x++) {
            encSurfaceIn->Data.Y[y * pitch + x] = data[0][y * stride[0] + x];
          }
        }
      }
      for (int y = 0; y < (input_height + 1) / 2; y++) {
        for (int x = 0; x < (input_width + 1) / 2; x++) {
          encSurfaceIn->Data.UV[y * pitch + x * 2] = data[1][y * stride[1] + x];
          encSurfaceIn->Data.UV[y * pitch + x * 2 + 1] = data[2][y * stride[2] + x];
        }
      }
    }
                        break;
    case MFX_FOURCC_P010: { // YYYY....UVUV....
      uint16_t* dataY = (uint16_t*)data[0];
      if (encSurfaceIn->Info.Shift == 0) { // No shift
        for (int y = 0; y < input_height; y++) {
          for (int x = 0; x < input_width; x++) {
            encSurfaceIn->Data.Y16[y * (stride[0] / 2) + x] = dataY[y * (stride[0] / 2) + x];
          }
        }
        uint16_t* dataCb = (uint16_t*)data[1];
        uint16_t* dataCr = (uint16_t*)data[2];
        uint16_t* UV = ((uint16_t*)encSurfaceIn->Data.UV); //  encSurfaceIn->Data.Y16 + encSurfaceIn->Info.Width * encSurfaceIn->Info.Height; // segfault ???
        for (int y = 0; y < (input_height + 1) / 2; y++) {
          for (int x = 0; x < (input_width + 1) / 2; x++) {
            UV[y * (pitch / 2) + x * 2] = dataCb[y * (stride[1] / 2) + x];
            UV[y * (pitch / 2) + x * 2 + 1] = dataCr[y * (stride[2] / 2) + x];
          }
        }
      }
      else { // Shift
        for (int y = 0; y < input_height; y++) {
          for (int x = 0; x < input_width; x++) {
            encSurfaceIn->Data.Y16[y * (stride[0] / 2) + x] = dataY[y * (stride[0] / 2) + x] << 6;
          }
        }
        uint16_t* dataCb = (uint16_t*)data[1];
        uint16_t* dataCr = (uint16_t*)data[2];
        uint16_t* UV = ((uint16_t*)encSurfaceIn->Data.UV); //  encSurfaceIn->Data.Y16 + encSurfaceIn->Info.Width * encSurfaceIn->Info.Height; // ???
        for (int y = 0; y < (input_height + 1) / 2; y++) {
          for (int x = 0; x < (input_width + 1) / 2; x++) {
            UV[y * (pitch / 2) + x * 2] = dataCb[y * (stride[1] / 2) + x] << 6;
            UV[y * (pitch / 2) + x * 2 + 1] = dataCr[y * (stride[2] / 2) + x] << 6;
          }
        }
      }
    }
                        break;
    case MFX_FOURCC_YUY2: {
      for(int y = 0; y < input_height; y++) {
        for(int x = 0; x < (input_width + 1) / 2; x++) {
          encSurfaceIn->Data.Y[y * pitch + x * 4] = data[0][y * stride[0] + x * 2];
          encSurfaceIn->Data.Y[y * pitch + x * 4 + 1] = data[1][y * stride[1] + x];
          if (x * 2 + 1 < input_width)
            encSurfaceIn->Data.Y[y * pitch + x * 4 + 2] = data[0][y * stride[0] + x * 2 + 1];
          encSurfaceIn->Data.Y[y * pitch + x * 4 + 3] = data[2][y * stride[2] + x];
        }
      }
    }
                        break;
    case MFX_FOURCC_Y210: {
    if (encSurfaceIn->Info.Shift == 0) { // No shift
      for(int y = 0; y < input_height; y++) {
        for(int x = 0; x < (input_width + 1) / 2; x++) {
          encSurfaceIn->Data.Y16[y * (pitch / 2) + x * 4] = ((uint16_t*)data[0])[y * (stride[0]/2) + x * 2];
          encSurfaceIn->Data.Y16[y * (pitch / 2) + x * 4 + 1] = ((uint16_t*)data[1])[y * (stride[1]/2) + x];
          if (x * 2 + 1 < input_width)
            encSurfaceIn->Data.Y16[y * (pitch / 2) + x * 4 + 2] = ((uint16_t*)data[0])[y * (stride[0]/2) + x * 2 + 1];
          encSurfaceIn->Data.Y16[y * (pitch / 2) + x * 4 + 3] = ((uint16_t*)data[2])[y * (stride[2]/2) + x];
        }
      }
    }
    else {
      for(int y = 0; y < input_height; y++) {
        for(int x = 0; x < (input_width + 1) / 2; x++) {
          encSurfaceIn->Data.Y16[y * (pitch / 2) + x * 4] = ((uint16_t*)data[0])[y * (stride[0]/2) + x * 2] << 6;
          encSurfaceIn->Data.Y16[y * (pitch / 2) + x * 4 + 1] = ((uint16_t*)data[1])[y * (stride[1]/2) + x] << 6;
          if (x * 2 + 1 < input_width)
            encSurfaceIn->Data.Y16[y * (pitch / 2) + x * 4 + 2] = ((uint16_t*)data[0])[y * (stride[0]/2) + x * 2 + 1] << 6;
          encSurfaceIn->Data.Y16[y * (pitch / 2) + x * 4 + 3] = ((uint16_t*)data[2])[y * (stride[2]/2) + x] << 6;
        }
      }
    }
    }
                        break;
    default:
      return {
        heif_error_Encoder_plugin_error,
        heif_suberror_Unsupported_bit_depth,
        "Luma bit depth must equal the chroma bit depth"
      };
      break;
    }
  } // Unmap buffers
  do {
    sts = MFXVideoENCODE_EncodeFrameAsync(session,
      NULL,
      encSurfaceIn,
      &encoder->bitstream,
      &encoder->syncp);

    switch (sts) {
    case MFX_ERR_NONE:
      // MFX_ERR_NONE and syncp indicate output is available
      if (encoder->syncp) {
        // Encode output is not available on CPU until sync operation
        // completes
        do {
          sts = MFXVideoCORE_SyncOperation(session, encoder->syncp, WAIT_100_MILLISECONDS);
          if (MFX_ERR_NONE == sts) {
            append_chunk_data(encoder, encoder->frameNr);
            encoder->frameNr++;
          }
        } while (sts == MFX_WRN_IN_EXECUTION);
      }
      //sts == MFX_ERR_NONE; // reset status so it won't interfere with loop condition
      break;
    case MFX_ERR_NOT_ENOUGH_BUFFER:
      // This example deliberatly uses a large output buffer with immediate
      // write to disk for simplicity. Handle when frame size exceeds
      // available buffer here
      if (!encoder->doubleSize()) {
        return {
          heif_error_Encoder_plugin_error,
          heif_suberror_Security_limit_exceeded,
          "bitstream buffer too big"
        };
      }
      break;
    case MFX_ERR_MORE_DATA:
      // The function requires more data to generate any output
      //if (isDraining == true)
      //    isStillGoing = false;
      break;
    case MFX_ERR_DEVICE_LOST:
      // For non-CPU implementations,
      // Cleanup if device is lost
      break;
    case MFX_WRN_DEVICE_BUSY:
      // For non-CPU implementations,
      // Wait a few milliseconds then try again
      break;
    default:
      printf("unknown status %d\n", sts);
      //isStillGoing = false;
      break;
    }

    //if (data) {
    //    api->chunk_free(data);
    //    data = nullptr;
    //}

    //api->picture_free(picture_out); //???
  } while (sts == MFX_WRN_DEVICE_BUSY || sts == MFX_ERR_NOT_ENOUGH_BUFFER);

  return heif_error_ok;
}


static heif_error intelvpl_end_sequence_encoding(void* encoder_raw)
{
  intelvpl_encoder* encoder = (intelvpl_encoder*)encoder_raw;
  int framenum = 0;
  mfxStatus sts;

  for (;;) {
    sts = MFXVideoENCODE_EncodeFrameAsync(session,
      NULL,
      NULL,
      &encoder->bitstream,
      &encoder->syncp);

    switch (sts) {
    case MFX_ERR_NONE:
      // MFX_ERR_NONE and syncp indicate output is available
      if (encoder->syncp) {
        // Encode output is not available on CPU until sync operation
        // completes
        do {
          sts = MFXVideoCORE_SyncOperation(session, encoder->syncp, WAIT_100_MILLISECONDS);
          if (MFX_ERR_NONE == sts) {
            //WriteEncodedStream(bitstream, sink);
            framenum++;
          }
        } while (sts == MFX_WRN_IN_EXECUTION);
      }
      break;
    case MFX_ERR_NOT_ENOUGH_BUFFER:
      // This example deliberatly uses a large output buffer with immediate
      // write to disk for simplicity. Handle when frame size exceeds
      // available buffer here
      if (!encoder->doubleSize()) {
        return {
          heif_error_Encoder_plugin_error,
          heif_suberror_Security_limit_exceeded,
          "bitstream buffer too big"
        };
      }
      break;
    case MFX_ERR_MORE_DATA:
      // The function requires more data to generate any output
      return heif_error_ok;
    case MFX_ERR_DEVICE_LOST:
      // For non-CPU implementations,
      // Cleanup if device is lost
      break;
    case MFX_WRN_DEVICE_BUSY:
      // For non-CPU implementations,
      // Wait a few milliseconds then try again
      break;
    default:
      printf("unknown status %d\n", sts);
      break;
    }

    append_chunk_data(encoder, encoder->frameNr);
    // free ???
  }

  return heif_error_ok;
}


static heif_error intelvpl_encode_image(void* encoder_raw, const heif_image* image,
  heif_image_input_class input_class)
{
  heif_error err;
  err = intelvpl_start_sequence_encoding_intern(encoder_raw, image, input_class, 25, 1, nullptr, false);
  if (err.code) {
    return err;
  }

  err = intelvpl_encode_sequence_frame(encoder_raw, image, 0);
  if (err.code) {
    return err;
  }

  return intelvpl_end_sequence_encoding(encoder_raw);
}

static heif_error intelvpl_get_compressed_data_intern(void* encoder_raw, uint8_t** data, int* size,
  uintptr_t* frame_nr, int* more_frame_packets)
{
  intelvpl_encoder* encoder = (intelvpl_encoder*)encoder_raw;

  if (encoder->output_data.empty()) {
    *data = nullptr;
    *size = 0;

    return heif_error_ok;
  }

  std::vector<uint8_t>& pktdata = encoder->output_data.front().data;

  if (frame_nr) {
    *frame_nr = encoder->output_data.front().frameNr;
  }

  if (more_frame_packets) {
    if (encoder->output_data.size() > 1 &&
      encoder->output_data[0].frameNr == encoder->output_data[1].frameNr) {
      *more_frame_packets = 1;
    }
    else {
      *more_frame_packets = 0;
    }
  }

  encoder->active_data = std::move(pktdata);
  encoder->output_data.pop_front();

  *data = encoder->active_data.data();
  *size = static_cast<int>(encoder->active_data.size());

  return heif_error_ok;
}

static heif_error intelvpl_get_compressed_data(void* encoder_raw, uint8_t** data, int* size,
  heif_encoded_data_type* type)
{
  return intelvpl_get_compressed_data_intern(encoder_raw, data, size, nullptr, nullptr);
}

static heif_error intelvpl_get_compressed_data2(void* encoder_raw, uint8_t** data, int* size,
  uintptr_t* frame_nr, int* is_keyframe, int* more_frame_packets)
{
  return intelvpl_get_compressed_data_intern(encoder_raw, data, size, frame_nr, more_frame_packets);
}

static heif_chroma intelvpl_get_chroma_format(const mfxFrameInfo* info) {
  switch (info->FourCC) {
  case MFX_FOURCC_NV12:
  case MFX_FOURCC_I420:
  case MFX_FOURCC_P010:
    return heif_chroma_420;
  case MFX_FOURCC_YUY2:
  case MFX_FOURCC_Y210:
    return heif_chroma_422;
  case MFX_FOURCC_RGB4:
    return heif_chroma_interleaved_RGBA;
  default:
    return heif_chroma_undefined;
  }
}

static heif_error intelvpl_set_parameter_quality(void* encoder_raw, int quality)
{
  if (quality <= 0 || quality > 100) {
    return heif_error_invalid_parameter_value;
  }
  intelvpl_encoder* encoder = (intelvpl_encoder*)encoder_raw;
  encoder->quality = quality;
  return heif_error_ok;
}

static heif_error intelvpl_get_parameter_quality(void* encoder_raw, int* quality)
{
  if (!quality || !encoder_raw) {
    return heif_error_null_pointer_argument;
  }
  intelvpl_encoder* encoder = (intelvpl_encoder*)encoder_raw;
  *quality = encoder->quality;
  return heif_error_ok;
}

static heif_error intelvpl_set_parameter_lossless(void* encoder_raw, int enable)
{
  return heif_error_unsupported_parameter;
}

static heif_error intelvpl_get_parameter_lossless(void* encoder_raw, int* enable)
{
  *enable = 0;
  return heif_error_unsupported_parameter;
}

static heif_error intelvpl_set_parameter_logging_level(void* encoder_raw, int logging)
{
  return heif_error_ok;
}

static heif_error intelvpl_get_parameter_logging_level(void* encoder_raw, int* loglevel)
{
  *loglevel = 0;

  return heif_error_ok;
}

static heif_error intelvpl_set_parameter_integer(void* encoder_raw, const char* name, int value)
{
  return heif_error_ok;
}

static heif_error intelvpl_get_parameter_integer(void* encoder_raw, const char* name, int* value)
{
  *value = 0;

  return heif_error_ok;
}

static heif_error intelvpl_set_parameter_string(void* encoder_raw, const char* name, const char* value)
{
  return heif_error_unsupported_parameter;
}

static heif_error intelvpl_get_parameter_string(void* encoder_raw, const char* name,
  char* value, int value_size)
{
  return heif_error_unsupported_parameter;
}

static void intelvpl_query_input_colorspace(heif_colorspace* colorspace, heif_chroma* chroma)
{
  *colorspace = heif_colorspace_YCbCr;
  *chroma = heif_chroma_420;
}


static void intelvpl_query_input_colorspace2(void* encoder_raw, heif_colorspace* colorspace, heif_chroma* chroma)
{
  *colorspace = heif_colorspace_YCbCr;
  *chroma = heif_chroma_420;
}


static const heif_encoder_plugin encoder_libvpl_HEVC
{
  /* plugin_api_version */ 4,
  /* compression_format */ heif_compression_HEVC,
  /* id_name */ "hevc_qsv",
  /* priority */ INTELVPL_PLUGIN_PRIORITY,
  /* supports_lossy_compression */ true,
  /* supports_lossless_compression */ false,
  /* get_plugin_name */ intelvpl_HEVC_plugin_name,
  /* init_plugin */ intelvpl_init_plugin,
  /* cleanup_plugin */ intelvpl_cleanup_plugin,
  /* new_encoder */ intelvpl_new_encoder_HEVC,
  /* free_encoder */ intelvpl_free_encoder,
  /* set_parameter_quality */ intelvpl_set_parameter_quality,
  /* get_parameter_quality */ intelvpl_get_parameter_quality,
  /* set_parameter_lossless */ intelvpl_set_parameter_lossless,
  /* get_parameter_lossless */ intelvpl_get_parameter_lossless,
  /* set_parameter_logging_level */ intelvpl_set_parameter_logging_level,
  /* get_parameter_logging_level */ intelvpl_get_parameter_logging_level,
  /* list_parameters */ intelvpl_list_parameters,
  /* set_parameter_integer */ intelvpl_set_parameter_integer,
  /* get_parameter_integer */ intelvpl_get_parameter_integer,
  /* set_parameter_boolean */ intelvpl_set_parameter_integer, // boolean also maps to integer function
  /* get_parameter_boolean */ intelvpl_get_parameter_integer, // boolean also maps to integer function
  /* set_parameter_string */ intelvpl_set_parameter_string,
  /* get_parameter_string */ intelvpl_get_parameter_string,
  /* query_input_colorspace */ intelvpl_query_input_colorspace,
  /* encode_image */ intelvpl_encode_image,
  /* get_compressed_data */ intelvpl_get_compressed_data,
  /* query_input_colorspace (v2) */ intelvpl_query_input_colorspace2,
  /* query_encoded_size (v3) */ intelvpl_query_encoded_size,
  /* minimum_required_libheif_version */ LIBHEIF_MAKE_VERSION(1,21,0),
  /* start_sequence_encoding (v4) */ intelvpl_start_sequence_encoding,
  /* encode_sequence_frame (v4) */ intelvpl_encode_sequence_frame,
  /* end_sequence_encoding (v4) */ intelvpl_end_sequence_encoding,
  /* get_compressed_data2 (v4) */ intelvpl_get_compressed_data2,
  /* does_indicate_keyframes (v4) */ 0
};

const heif_encoder_plugin* get_encoder_plugin_libvpl_HEVC()
{
  return &encoder_libvpl_HEVC;
}

static const heif_encoder_plugin encoder_libvpl_AVC
{
    /* plugin_api_version */ 4,
    /* compression_format */ heif_compression_AVC,
    /* id_name */ "h264_qsv",
    /* priority */ INTELVPL_PLUGIN_PRIORITY,
    /* supports_lossy_compression */ true,
    /* supports_lossless_compression */ false,
    /* get_plugin_name */ intelvpl_AVC_plugin_name,
    /* init_plugin */ intelvpl_init_plugin,
    /* cleanup_plugin */ intelvpl_cleanup_plugin,
    /* new_encoder */ intelvpl_new_encoder_AVC,
    /* free_encoder */ intelvpl_free_encoder,
    /* set_parameter_quality */ intelvpl_set_parameter_quality,
    /* get_parameter_quality */ intelvpl_get_parameter_quality,
    /* set_parameter_lossless */ intelvpl_set_parameter_lossless,
    /* get_parameter_lossless */ intelvpl_get_parameter_lossless,
    /* set_parameter_logging_level */ intelvpl_set_parameter_logging_level,
    /* get_parameter_logging_level */ intelvpl_get_parameter_logging_level,
    /* list_parameters */ intelvpl_list_parameters,
    /* set_parameter_integer */ intelvpl_set_parameter_integer,
    /* get_parameter_integer */ intelvpl_get_parameter_integer,
    /* set_parameter_boolean */ intelvpl_set_parameter_integer, // boolean also maps to integer function
    /* get_parameter_boolean */ intelvpl_get_parameter_integer, // boolean also maps to integer function
    /* set_parameter_string */ intelvpl_set_parameter_string,
    /* get_parameter_string */ intelvpl_get_parameter_string,
    /* query_input_colorspace */ intelvpl_query_input_colorspace,
    /* encode_image */ intelvpl_encode_image,
    /* get_compressed_data */ intelvpl_get_compressed_data,
    /* query_input_colorspace (v2) */ intelvpl_query_input_colorspace2,
    /* query_encoded_size (v3) */ intelvpl_query_encoded_size,
    /* minimum_required_libheif_version */ LIBHEIF_MAKE_VERSION(1,21,0),
    /* start_sequence_encoding (v4) */ intelvpl_start_sequence_encoding,
    /* encode_sequence_frame (v4) */ intelvpl_encode_sequence_frame,
    /* end_sequence_encoding (v4) */ intelvpl_end_sequence_encoding,
    /* get_compressed_data2 (v4) */ intelvpl_get_compressed_data2,
    /* does_indicate_keyframes (v4) */ 0
};

const heif_encoder_plugin* get_encoder_plugin_libvpl_AVC()
{
    return &encoder_libvpl_AVC;
}

static const heif_encoder_plugin encoder_libvpl_AV1
{
  /* plugin_api_version */ 4,
  /* compression_format */ heif_compression_AV1,
  /* id_name */ "av1_qsv",
  /* priority */ INTELVPL_PLUGIN_PRIORITY,
  /* supports_lossy_compression */ true,
  /* supports_lossless_compression */ false,
  /* get_plugin_name */ intelvpl_AV1_plugin_name,
  /* init_plugin */ intelvpl_init_plugin,
  /* cleanup_plugin */ intelvpl_cleanup_plugin,
  /* new_encoder */ intelvpl_new_encoder_AV1,
  /* free_encoder */ intelvpl_free_encoder,
  /* set_parameter_quality */ intelvpl_set_parameter_quality,
  /* get_parameter_quality */ intelvpl_get_parameter_quality,
  /* set_parameter_lossless */ intelvpl_set_parameter_lossless,
  /* get_parameter_lossless */ intelvpl_get_parameter_lossless,
  /* set_parameter_logging_level */ intelvpl_set_parameter_logging_level,
  /* get_parameter_logging_level */ intelvpl_get_parameter_logging_level,
  /* list_parameters */ intelvpl_list_parameters,
  /* set_parameter_integer */ intelvpl_set_parameter_integer,
  /* get_parameter_integer */ intelvpl_get_parameter_integer,
  /* set_parameter_boolean */ intelvpl_set_parameter_integer, // boolean also maps to integer function
  /* get_parameter_boolean */ intelvpl_get_parameter_integer, // boolean also maps to integer function
  /* set_parameter_string */ intelvpl_set_parameter_string,
  /* get_parameter_string */ intelvpl_get_parameter_string,
  /* query_input_colorspace */ intelvpl_query_input_colorspace,
  /* encode_image */ intelvpl_encode_image,
  /* get_compressed_data */ intelvpl_get_compressed_data,
  /* query_input_colorspace (v2) */ intelvpl_query_input_colorspace2,
  /* query_encoded_size (v3) */ intelvpl_query_encoded_size,
  /* minimum_required_libheif_version */ LIBHEIF_MAKE_VERSION(1,21,0),
  /* start_sequence_encoding (v4) */ intelvpl_start_sequence_encoding,
  /* encode_sequence_frame (v4) */ intelvpl_encode_sequence_frame,
  /* end_sequence_encoding (v4) */ intelvpl_end_sequence_encoding,
  /* get_compressed_data2 (v4) */ intelvpl_get_compressed_data2,
  /* does_indicate_keyframes (v4) */ 0
};

const heif_encoder_plugin* get_encoder_plugin_libvpl_AV1()
{
  return &encoder_libvpl_AV1;
}

static const heif_encoder_plugin encoder_libvpl_JPEG
{
    /* plugin_api_version */ 4,
    /* compression_format */ heif_compression_JPEG,
    /* id_name */ "jpeg_qsv",
    /* priority */ INTELVPL_PLUGIN_PRIORITY,
    /* supports_lossy_compression */ true,
    /* supports_lossless_compression */ false,
    /* get_plugin_name */ intelvpl_JPEG_plugin_name,
    /* init_plugin */ intelvpl_init_plugin,
    /* cleanup_plugin */ intelvpl_cleanup_plugin,
    /* new_encoder */ intelvpl_new_encoder_JPEG,
    /* free_encoder */ intelvpl_free_encoder,
    /* set_parameter_quality */ intelvpl_set_parameter_quality,
    /* get_parameter_quality */ intelvpl_get_parameter_quality,
    /* set_parameter_lossless */ intelvpl_set_parameter_lossless,
    /* get_parameter_lossless */ intelvpl_get_parameter_lossless,
    /* set_parameter_logging_level */ intelvpl_set_parameter_logging_level,
    /* get_parameter_logging_level */ intelvpl_get_parameter_logging_level,
    /* list_parameters */ intelvpl_list_parameters,
    /* set_parameter_integer */ intelvpl_set_parameter_integer,
    /* get_parameter_integer */ intelvpl_get_parameter_integer,
    /* set_parameter_boolean */ intelvpl_set_parameter_integer, // boolean also maps to integer function
    /* get_parameter_boolean */ intelvpl_get_parameter_integer, // boolean also maps to integer function
    /* set_parameter_string */ intelvpl_set_parameter_string,
    /* get_parameter_string */ intelvpl_get_parameter_string,
    /* query_input_colorspace */ intelvpl_query_input_colorspace,
    /* encode_image */ intelvpl_encode_image,
    /* get_compressed_data */ intelvpl_get_compressed_data,
    /* query_input_colorspace (v2) */ intelvpl_query_input_colorspace2,
    /* query_encoded_size (v3) */ intelvpl_query_encoded_size,
    /* minimum_required_libheif_version */ LIBHEIF_MAKE_VERSION(1,21,0),
    /* start_sequence_encoding (v4) */ intelvpl_start_sequence_encoding,
    /* encode_sequence_frame (v4) */ intelvpl_encode_sequence_frame,
    /* end_sequence_encoding (v4) */ intelvpl_end_sequence_encoding,
    /* get_compressed_data2 (v4) */ intelvpl_get_compressed_data2,
    /* does_indicate_keyframes (v4) */ 0
};

const heif_encoder_plugin* get_encoder_plugin_libvpl_JPEG()
{
    return &encoder_libvpl_JPEG;
}

#if PLUGIN_INTELVPL
heif_plugin_info plugin_info{
  1,
  heif_plugin_type_encoder,
  &encoder_libvpl
};
#endif
