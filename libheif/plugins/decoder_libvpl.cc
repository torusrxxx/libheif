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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef USE_MEDIASDK1
#include "mfxvideo.h"
enum {
    MFX_FOURCC_I420 = MFX_FOURCC_IYUV /*!< Alias for the IYUV color format. */
};
#else
#include "vpl/mfxjpeg.h"
#include "vpl/mfxvideo.h"
#endif

#if (MFX_VERSION >= 2000)
#include "vpl/mfxdispatcher.h"
#endif

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#endif

#ifdef _DEBUG
#pragma comment(lib, "vpld.lib")
#else
#pragma comment(lib, "vpl.lib")
#endif

#ifdef LIBVA_SUPPORT
#include "va/va.h"
#include "va/va_drm.h"
#endif

#define WAIT_100_MILLISECONDS 100
#define MAX_WIDTH             3840
#define MAX_HEIGHT            2160

#define ALIGN16(value)           (((value + 15) >> 4) << 4)
#define ALIGN32(X)               (((mfxU32)((X) + 31)) & (~(mfxU32)31))
#define VPLVERSION(major, minor) (major << 16 | minor)

//==============================================================================
// Copyright Intel Corporation
//
// SPDX-License-Identifier: MIT
//==============================================================================

///
/// A minimal Intel® Video Processing Library (Intel® VPL) decode application,
/// using 2.2 or newer API with internal memory management.
/// For more information see
/// https://intel.github.io/libvpl
/// @file

#define MAJOR_API_VERSION_REQUIRED 2
#define MINOR_API_VERSION_REQUIRED 2

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
#include "decoder_libvpl.h"
#include <cassert>
#include <memory>
#include <cstring>
#include <string>
#include "nalu_utils.h"


#define intelvpl_buffer_max_size 4096
struct intelvpl_decoder_image_chain
{
    intelvpl_decoder_image_chain* next = NULL;
    mfxFrameSurface1* decSurfaceOut = NULL;
    mfxSyncPoint syncp = {};
};
struct intelvpl_decoder
{
  bool strict_decoding = false;
  bool initialized = false;
  std::string error_message;
  intelvpl_decoder_image_chain* images = NULL;
  intelvpl_decoder_image_chain* images_current = NULL;
  mfxVideoParam decodeParams = {};
  mfxExtVideoSignalInfo nclx_info = {};
  mfxExtContentLightLevelInfo cll_info = {};
  mfxExtMasteringDisplayColourVolume mdcv_info = {};
  std::vector<uint8_t> data;
  intelvpl_decoder() {
      images = new intelvpl_decoder_image_chain();
      images_current = images;
      nclx_info.Header.BufferId = MFX_EXTBUFF_VIDEO_SIGNAL_INFO;
      nclx_info.Header.BufferSz = sizeof(nclx_info);
      cll_info.Header.BufferId = MFX_EXTBUFF_CONTENT_LIGHT_LEVEL_INFO;
      cll_info.Header.BufferSz = sizeof(cll_info);
      mdcv_info.Header.BufferId = MFX_EXTBUFF_MASTERING_DISPLAY_COLOUR_VOLUME;
      mdcv_info.Header.BufferSz = sizeof(mdcv_info);
  }
};

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
        if(sts != MFX_ERR_NONE)
            surface->FrameInterface->Unmap(surface);
    }
};

static const char kEmptyString[] = "";
static const char kSuccess[] = "Success";

static const int INTELVPL_PLUGIN_PRIORITY = 100;

#define MAX_PLUGIN_NAME_LENGTH 80

static char plugin_name[MAX_PLUGIN_NAME_LENGTH];


static const char* intelvpl_plugin_name()
{
  strcpy(plugin_name, "Intel VPL HEVC decoder");
  return plugin_name;
}

static mfxLoader loader = NULL;
static mfxSession session = NULL;
bool video_decode_initialized = false;
static void intelvpl_init_plugin()
{
    // Initialize session
    loader = MFXLoad();
    if (!loader)
        return;
}

static void intelvpl_deinit_plugin()
{
    if (session) {
        if (video_decode_initialized)
            MFXVideoDECODE_Close(session);
        MFXClose(session);
        session = NULL;
        video_decode_initialized = false;
    }
    if (loader) {
        MFXUnload(loader);
        loader = NULL;
    }
}

static int intelvpl_does_support_format(heif_compression_format format)
{
  if (format == heif_compression_HEVC) {
    return INTELVPL_PLUGIN_PRIORITY;
  }
  else {
    return 0;
  }
}


static int intelvpl_does_support_format2(const heif_decoder_plugin_compressed_format_description* format)
{
  return intelvpl_does_support_format(format->format);
}

static heif_error intelvpl_init_session() {
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
            heif_error_Decoder_plugin_error,
            heif_suberror_Plugin_loading_error,
            "MFXCreateConfig failed"
        };
    }
    cfgVal[0].Type = MFX_VARIANT_TYPE_U32;
    cfgVal[0].Data.U32 = MFX_IMPL_TYPE_HARDWARE;
    sts = MFXSetConfigFilterProperty(cfg[0], (mfxU8*)"mfxImplDescription.Impl", cfgVal[0]);
    if (NULL == cfg[0]) {
        return {
            heif_error_Decoder_plugin_error,
            heif_suberror_Plugin_loading_error,
            "MFXSetConfigFilterProperty failed for Impl"
        };
    }

    // Implementation must provide an HEVC decoder
    cfg[1] = MFXCreateConfig(loader);
    if (NULL == cfg[1]) {
        return {
            heif_error_Decoder_plugin_error,
            heif_suberror_Plugin_loading_error,
            "MFXCreateConfig failed"
        };
    }
    cfgVal[1].Type = MFX_VARIANT_TYPE_U32;
    cfgVal[1].Data.U32 = MFX_CODEC_HEVC;
    sts = MFXSetConfigFilterProperty(
        cfg[1],
        (mfxU8*)"mfxImplDescription.mfxDecoderDescription.decoder.CodecID",
        cfgVal[1]);
    if (MFX_ERR_NONE != sts) {
        return {
            heif_error_Decoder_plugin_error,
            heif_suberror_Plugin_loading_error,
            "MFXSetConfigFilterProperty failed for decoder CodecID"
        };
    }

    // Implementation used must provide API version 2.2 or newer
    cfg[2] = MFXCreateConfig(loader);
    if (NULL == cfg[2]) {
        return {
            heif_error_Decoder_plugin_error,
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
            heif_error_Decoder_plugin_error,
            heif_suberror_Plugin_loading_error,
            "MFXSetConfigFilterProperty failed for API version"
        };
    }

    sts = MFXCreateSession(loader, 0, &session);
    if (MFX_ERR_NONE != sts) {
        return {
            heif_error_Decoder_plugin_error,
            heif_suberror_Plugin_loading_error,
            "Cannot create session -- no implementations meet selection criteria"
        };
    }
    return { heif_error_Ok, heif_suberror_Unspecified, kSuccess };
}

// Create a new decoder context for decoding an image
heif_error intelvpl_new_decoder2(void** dec, const heif_decoder_plugin_options* options)
{
    heif_error err = intelvpl_init_session();
    if (err.code)
        return err;
    intelvpl_decoder* decoder = new intelvpl_decoder();
    *dec = decoder;
    return err;
}


static heif_error intelvpl_new_decoder(void** dec)
{
  heif_decoder_plugin_options options;
  options.format = heif_compression_HEVC;
  options.num_threads = 0;
  options.strict_decoding = false;

  return intelvpl_new_decoder2(dec, &options);
}

static void intelvpl_free_decoder(void* decoder_raw)
{
    intelvpl_decoder* decoder = (intelvpl_decoder*)decoder_raw;
    while (decoder->images != NULL) {
        intelvpl_decoder_image_chain* next = decoder->images->next;
        if (decoder->images->decSurfaceOut) {
            decoder->images->decSurfaceOut->FrameInterface->Release(decoder->images->decSurfaceOut);
        }
        delete decoder->images;
        decoder->images = next;
    }
    mfxStatus sts;
    //MFXVideoDECODE_Close(session);
    delete decoder;
}


void intelvpl_set_strict_decoding(void* decoder_raw, int flag)
{
    intelvpl_decoder* decoder = (intelvpl_decoder*) decoder_raw;

  decoder->strict_decoding = flag; // TODO
}


/*static heif_error intelvpl_push_datax(intelvpl_decoder* decoder, const uint8_t* ptr, uint32_t nal_size) {
    mfxStatus sts;
    uint32_t remaining = nal_size;
    mfxBitstream& bs = decoder->bitstream;
    if (!decoder->initialized) {
        decoder->decodeParams.mfx.CodecId = MFX_CODEC_HEVC;
        decoder->decodeParams.IOPattern = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
        while (true) {
        if (bs.DataLength > 0) {
            memmove(bs.Data, bs.Data + bs.DataOffset, bs.DataLength);
        }
        bs.DataOffset = 0;
        if (remaining > intelvpl_buffer_max_size) {
            bs.DataLength = intelvpl_buffer_max_size;
            memcpy(bs.Data, ptr, intelvpl_buffer_max_size);
            ptr += intelvpl_buffer_max_size;
            remaining -= intelvpl_buffer_max_size;
        }
        else {
            bs.DataLength = remaining;
            memcpy(bs.Data, ptr, remaining);
            ptr += remaining;
            remaining = 0;
        }


        sts = MFXVideoDECODE_DecodeHeader(decoder->session, &bs, &decoder->decodeParams);
        if (MFX_ERR_NONE != sts && MFX_ERR_MORE_DATA != sts) {
            return {
              heif_error_Decoder_plugin_error,
              heif_suberror_End_of_data,
              "Error decoding header\n"
            };
        }
        if (MFX_ERR_NONE == sts)
            break;
        if (MFX_ERR_MORE_DATA == sts && remaining == 0) {
            return heif_error_ok;
        }
        }

        // input parameters finished, now initialize decode
        sts = MFXVideoDECODE_Init(decoder->session, &decoder->decodeParams);
        if (MFX_ERR_NONE != sts) {
            return {
              heif_error_Decoder_plugin_error,
              heif_suberror_End_of_data,
              "Error initializing decode\n"
            };
        }
        decoder->initialized = true;
    }
    while (remaining != 0) {
        if (bs.DataLength > 0 && bs.DataOffset > 0) {
            // Fix data offset to 0
            memmove(bs.Data, bs.Data + bs.DataOffset, bs.DataLength);
        }
        bs.DataOffset = 0;
        if (bs.DataLength + remaining <= intelvpl_buffer_max_size) {
            memcpy(bs.Data + bs.DataLength, ptr, remaining);
            bs.DataLength += remaining;
            ptr += remaining;
            remaining = 0;
        }
        else {
            memcpy(bs.Data + bs.DataLength, ptr, intelvpl_buffer_max_size - bs.DataLength);
            remaining -= intelvpl_buffer_max_size - bs.DataLength;
            ptr += intelvpl_buffer_max_size - bs.DataLength;
            bs.DataLength = intelvpl_buffer_max_size;
        }
        printf("Debug: calling MFXVideoDECODE_DecodeFrameAsync\n");
        sts = MFXVideoDECODE_DecodeFrameAsync(decoder->session,
            &bs, //(isDraining) ? NULL : &bs,
            NULL,
            &decoder->images_current->decSurfaceOut,
            &decoder->images_current->syncp);
        switch (sts) {
        case MFX_ERR_NONE:
            printf("Debug: MFXVideoDECODE_DecodeFrameAsync return MFX_ERR_NONE\n");
            decoder->images_current->next = new intelvpl_decoder_image_chain();
            if (decoder->images_current->next == NULL) {
                return {
                  heif_error_Decoder_plugin_error,
                  heif_suberror_End_of_data,
                  "new failure\n"
                };
            }
            decoder->images_current = decoder->images_current->next;
            break;
        case MFX_ERR_MORE_DATA:
            // The function requires more bitstream at input before decoding can
            // proceed
            break;
        case MFX_ERR_MORE_SURFACE:
            // The function requires more frame surface at output before decoding
            // can proceed. This applies to external memory allocations and should
            // not be expected for a simple internal allocation case like this
            return {
              heif_error_Decoder_plugin_error,
              heif_suberror_End_of_data,
              "MFX_ERR_MORE_SURFACE\n"
            };
            break;
        case MFX_ERR_DEVICE_LOST:
            // For non-CPU implementations,
            // Cleanup if device is lost
            return {
              heif_error_Decoder_plugin_error,
              heif_suberror_End_of_data,
              "MFX_ERR_DEVICE_LOST\n"
            };
            break;
        case MFX_WRN_DEVICE_BUSY:
            // For non-CPU implementations,
            // Wait a few milliseconds then try again
            break;
        case MFX_WRN_VIDEO_PARAM_CHANGED:
            // The decoder detected a new sequence header in the bitstream.
            // Video parameters may have changed.
            // In external memory allocation case, might need to reallocate the
            // output surface
            break;
        case MFX_ERR_INCOMPATIBLE_VIDEO_PARAM:
            // The function detected that video parameters provided by the
            // application are incompatible with initialization parameters. The
            // application should close the component and then reinitialize it
            return {
              heif_error_Decoder_plugin_error,
              heif_suberror_End_of_data,
              "MFX_ERR_INCOMPATIBLE_VIDEO_PARAM\n"
            };
            break;
        case MFX_ERR_REALLOC_SURFACE:
            // Bigger surface_work required. May be returned only if
            // mfxInfoMFX::EnableReallocRequest was set to ON during initialization.
            // This applies to external memory allocations and should not be
            // expected for a simple internal allocation case like this
            return {
              heif_error_Decoder_plugin_error,
              heif_suberror_End_of_data,
              "MFX_ERR_REALLOC_SURFACE\n"
            };
            break;
        default:
            return {
              heif_error_Decoder_plugin_error,
              heif_suberror_End_of_data,
              "unknown status\n"
            };
        }
    }
}*/

static heif_error intelvpl_push_data2(void* decoder_raw, const void* data, size_t size, uintptr_t userdata)
{
    intelvpl_decoder* decoder = (intelvpl_decoder*)decoder_raw;
    const uint8_t* input_data = (const uint8_t*)data;
    decoder->data.insert(decoder->data.end(), input_data, input_data + size);
    return heif_error_success;
}


static heif_error intelvpl_push_data(void* decoder_raw, const void* data, size_t size)
{
  return intelvpl_push_data2(decoder_raw, data, size, 0);
}

static heif_error intelvpl_flush_data(void* decoder_raw)
{
    // TODO: actually flush
    //intelvpl_decoder* decoder = (intelvpl_decoder*) decoder_raw;

  //de265_flush_data(decoder->ctx);

  return heif_error_ok;
}

static heif_chroma intelvpl_get_chroma_format(const mfxFrameInfo* info) {
    if(info->FourCC == 0)
        return heif_chroma_undefined;
    switch (info->ChromaFormat) {
    case MFX_CHROMAFORMAT_MONOCHROME:
        return heif_chroma_monochrome;
    case MFX_CHROMAFORMAT_YUV420:
        return heif_chroma_420;
    case MFX_CHROMAFORMAT_YUV422:
        return heif_chroma_422;
    case MFX_CHROMAFORMAT_YUV444:
        return heif_chroma_444;
    default:
        return heif_chroma_undefined;
    }
}

static heif_error intelvpl_decode_next_image2(void* decoder_raw,
    heif_image** out_img,
    uintptr_t* out_user_data,
    const heif_security_limits* limits)
{
    intelvpl_decoder* decoder = (intelvpl_decoder*)decoder_raw;
    heif_error err = { heif_error_Ok, heif_suberror_Unspecified, kSuccess };
    mfxBitstream bs;
    memset(&bs, 0, sizeof(bs));
    mfxStatus sts;
    uint8_t* hevc_data = NULL; // TODO: free data
    std::unique_ptr<uint8_t, void(*)(void*)> hevc_data_free(hevc_data, free);
    size_t hevc_data_size;
    if (!decoder->initialized) {
        NalMap nalus;
        err = nalus.parseHevcNalu(decoder->data.data(), decoder->data.size());
        if (err.code != heif_error_Ok) {
            return err;
        }

        err = nalus.buildWithStartCodesHevc(&hevc_data, &hevc_data_size, 0);

        if (err.code != heif_error_Ok) {
            return err;
        }
        bs.Data = hevc_data;
        bs.MaxLength = hevc_data_size;
        bs.DataLength = hevc_data_size;
        decoder->decodeParams.mfx.CodecId = MFX_CODEC_HEVC;
        decoder->decodeParams.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY | MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
        decoder->decodeParams.NumExtParam = 3;
        mfxExtBuffer* extBuffer[3];
        extBuffer[0] = (mfxExtBuffer*)&decoder->nclx_info;
        extBuffer[1] = (mfxExtBuffer*)&decoder->cll_info;
        extBuffer[2] = (mfxExtBuffer*)&decoder->mdcv_info;
        decoder->decodeParams.ExtParam = extBuffer;
        sts = MFXVideoDECODE_DecodeHeader(session, &bs, &decoder->decodeParams);
        if (MFX_ERR_NONE != sts && MFX_ERR_MORE_DATA != sts) {
            return {
              heif_error_Decoder_plugin_error,
              heif_suberror_End_of_data,
              "Error decoding header\n"
            };
        }
        if (MFX_ERR_NONE != sts)
            return err;
        decoder->decodeParams.NumExtParam = 0;
        // input parameters finished, now initialize decode
        if (!video_decode_initialized) {
            sts = MFXVideoDECODE_Init(session, &decoder->decodeParams);
            if (MFX_ERR_NONE != sts) {
                return {
                  heif_error_Decoder_plugin_error,
                  heif_suberror_End_of_data,
                  "Error initializing decode\n"
                };
            }
            video_decode_initialized = true;
        }
        //else {
        //    MFXVideoDECODE_Close(session);
        //    sts = MFXVideoDECODE_Init(session, &decoder->decodeParams);
        //    if (MFX_ERR_NONE != sts) {
        //        return {
        //          heif_error_Decoder_plugin_error,
        //          heif_suberror_End_of_data,
        //          "Error initializing decode\n"
        //        };
        //    }
        //}
        decoder->initialized = true;
    }

    bool setempty = false;
    while (bs.DataLength > 0 || setempty) {
        /*if (bs.DataLength > 0 && bs.DataOffset > 0) {
            // Fix data offset to 0
            memmove(bs.Data, bs.Data + bs.DataOffset, bs.DataLength);
        }
        bs.DataOffset = 0;
        if (bs.DataLength + remaining <= intelvpl_buffer_max_size) {
            memcpy(bs.Data + bs.DataLength, ptr, remaining);
            bs.DataLength += remaining;
            ptr += remaining;
            remaining = 0;
        }
        else {
            memcpy(bs.Data + bs.DataLength, ptr, intelvpl_buffer_max_size - bs.DataLength);
            remaining -= intelvpl_buffer_max_size - bs.DataLength;
            ptr += intelvpl_buffer_max_size - bs.DataLength;
            bs.DataLength = intelvpl_buffer_max_size;
        }*/
        sts = MFXVideoDECODE_DecodeFrameAsync(session,
            bs.DataLength == 0 ? NULL : &bs, //(isDraining) ? NULL : &bs,
            NULL,
            &decoder->images_current->decSurfaceOut,
            &decoder->images_current->syncp);
        switch (sts) {
        case MFX_ERR_NONE:
            decoder->images_current->next = new intelvpl_decoder_image_chain();
            if (decoder->images_current->next == NULL) {
                return {
                    heif_error_Decoder_plugin_error,
                    heif_suberror_End_of_data,
                    "new failure\n"
                };
            }
            decoder->images_current = decoder->images_current->next;
            break;
        case MFX_ERR_MORE_DATA:
            // The function requires more bitstream at input before decoding can
            // proceed
            if (setempty == false && bs.DataLength == 0) {
                setempty = true; // Needs one more MFXVideoDECODE_DecodeFrameAsync call with NULL bitstream
            }
            else {
                setempty = false;
            }
            break;
        case MFX_ERR_MORE_SURFACE:
            // The function requires more frame surface at output before decoding
            // can proceed. This applies to external memory allocations and should
            // not be expected for a simple internal allocation case like this
            return {
                heif_error_Decoder_plugin_error,
                heif_suberror_End_of_data,
                "MFX_ERR_MORE_SURFACE\n"
            };
            break;
        case MFX_ERR_DEVICE_LOST:
            // For non-CPU implementations,
            // Cleanup if device is lost
            return {
                heif_error_Decoder_plugin_error,
                heif_suberror_End_of_data,
                "MFX_ERR_DEVICE_LOST\n"
            };
            break;
        case MFX_WRN_DEVICE_BUSY:
            // For non-CPU implementations,
            // Wait a few milliseconds then try again
            break;
        case MFX_WRN_VIDEO_PARAM_CHANGED:
            // The decoder detected a new sequence header in the bitstream.
            // Video parameters may have changed.
            // In external memory allocation case, might need to reallocate the
            // output surface
            break;
        case MFX_ERR_INCOMPATIBLE_VIDEO_PARAM:
            // TODO: it reuses the same video decoder for all HEVC files, implement it
            // The function detected that video parameters provided by the
            // application are incompatible with initialization parameters. The
            // application should close the component and then reinitialize it
            return {
                heif_error_Decoder_plugin_error,
                heif_suberror_End_of_data,
                "MFX_ERR_INCOMPATIBLE_VIDEO_PARAM\n"
            };
            break;
        case MFX_ERR_REALLOC_SURFACE:
            // Bigger surface_work required. May be returned only if
            // mfxInfoMFX::EnableReallocRequest was set to ON during initialization.
            // This applies to external memory allocations and should not be
            // expected for a simple internal allocation case like this
            return {
                heif_error_Decoder_plugin_error,
                heif_suberror_End_of_data,
                "MFX_ERR_REALLOC_SURFACE\n"
            };
            break;
        default:
            return {
                heif_error_Decoder_plugin_error,
                heif_suberror_End_of_data,
                "unknown status\n"
            };
        }
    }

    *out_img = nullptr;
    if (decoder->images != NULL && decoder->images->decSurfaceOut != NULL) {
        mfxStatus sts;
        do {
            sts = decoder->images->decSurfaceOut->FrameInterface->Synchronize(decoder->images->decSurfaceOut,
                WAIT_100_MILLISECONDS);
            if (MFX_ERR_NONE == sts) {
                mfxFrameSurface1* surface = decoder->images->decSurfaceOut;
                mfxU16 w, h, pitch;
                mfxFrameInfo* info = &surface->Info;
                mfxFrameData* data = &surface->Data;

                w = info->CropW;
                h = info->CropH;
                if (w == 0 || h == 0) {
                    err = { heif_error_Decoder_plugin_error,
                            heif_suberror_Invalid_image_size,
                            kEmptyString };
                    return err;
                }

                if (data->Corrupted != MFX_CORRUPTION_NO && data->Corrupted != MFX_CORRUPTION_MINOR) {
                    return { heif_error_Invalid_input,
                            heif_suberror_Decompression_invalid_data,
                            "Bitstream is corrupted" };
                }

                if (limits &&
                    limits->max_image_size_pixels &&
                    limits->max_image_size_pixels / h < w) {

                    //std::stringstream sstr;
                    //sstr << "Allocating an image of size " << w << "x" << h << " exceeds the security limit of "
                    //    << limits->max_image_size_pixels << " pixels";

                    return { heif_error_Memory_allocation_error,
                            heif_suberror_Security_limit_exceeded,
                            ""}; // sstr.str().c_str()
                }
                //sts = WriteRawFrame_InternalMem(decoder->images->decSurfaceOut, sink);

                intelvpl_surface_mapper surfaceMap(decoder->images->decSurfaceOut, MFX_MAP_READ);
                mfxStatus sts = surfaceMap.status();
                if (sts != MFX_ERR_NONE) {
                    return err; // "mfxFrameSurfaceInterface->Map failed (%d)\n"
                }

                //sts = WriteRawFrame(decoder->images->decSurfaceOut, f);
                {
                    // TODO: mono not supported

                    heif_error err;
                    err = heif_image_create(w,
                        h,
                        heif_colorspace_YCbCr, // TODO: Mono
                        intelvpl_get_chroma_format(info),
                        out_img);
                    if (err.code) {
                        return err;
                    }
                    // Y
                    err = heif_image_add_plane_safe(*out_img, heif_channel_Y, w, h, info->BitDepthLuma, limits);
                    if (err.code) {
                        // copy error message to decoder object because heif_image will be released
                        decoder->error_message = err.message;
                        err.message = decoder->error_message.c_str();

                        heif_image_release(*out_img);
                        out_img = NULL;
                        return err;
                    }
                    // Cb Cr
                    switch (intelvpl_get_chroma_format(info)) {
                    case heif_chroma_420:
                        err = heif_image_add_plane_safe(*out_img, heif_channel_Cb, (w + 1) / 2, (h + 1) / 2, info->BitDepthChroma, limits);
                        err = heif_image_add_plane_safe(*out_img, heif_channel_Cr, (w + 1) / 2, (h + 1) / 2, info->BitDepthChroma, limits);
                        break;
                    case heif_chroma_422:
                        err = heif_image_add_plane_safe(*out_img, heif_channel_Cb, (w + 1) / 2, h, info->BitDepthChroma, limits);
                        err = heif_image_add_plane_safe(*out_img, heif_channel_Cr, (w + 1) / 2, h, info->BitDepthChroma, limits);
                        break;
                    case heif_chroma_444:
                        err = heif_image_add_plane_safe(*out_img, heif_channel_Cb, w, h, info->BitDepthChroma, limits);
                        err = heif_image_add_plane_safe(*out_img, heif_channel_Cr, w, h, info->BitDepthChroma, limits);
                        break;
                    }
                    pitch = data->PitchHigh << 16 | data->Pitch;
                    switch (info->FourCC) {
                    case MFX_FOURCC_NV12: { // YYYY....UVUV....
                        size_t dst_stride_Y;
                        uint8_t* dst_mem_Y = heif_image_get_plane2(*out_img, heif_channel_Y, &dst_stride_Y);
                        if (dst_stride_Y == pitch) {
                            memcpy(dst_mem_Y, data->Y, w* pitch);
                        }
                        else {
                            for (int y = 0; y < h; y++) {
                                memcpy(dst_mem_Y + y * dst_stride_Y, data->Y + y * pitch, w);
                            }
                        }
                        // UV
                        h = (h + 1) / 2;
                        w = (w + 1) / 2;
                        size_t dst_stride_Cb;
                        uint8_t* dst_mem_Cb = heif_image_get_plane2(*out_img, heif_channel_Cb, &dst_stride_Cb);
                        size_t dst_stride_Cr;
                        uint8_t* dst_mem_Cr = heif_image_get_plane2(*out_img, heif_channel_Cr, &dst_stride_Cr);
                        for (int y = 0; y < h; y++) {
                            for (int x = 0; x < w; x++) {
                                dst_mem_Cb[y * dst_stride_Cb + x] = data->UV[y * pitch + x * 2];
                                dst_mem_Cr[y * dst_stride_Cr + x] = data->UV[y * pitch + x * 2 + 1];
                            }
                        }
                    }
                        break;
                    case MFX_FOURCC_P010: { // YYYY....UVUV....
                        size_t dst_stride_Y;
                        uint16_t* dst_mem_Y = (uint16_t *)heif_image_get_plane2(*out_img, heif_channel_Y, &dst_stride_Y);
                        dst_stride_Y /= 2;

                        for (int y = 0; y < h; y++) {
                            for (int x = 0; x < w; x++) {
                                dst_mem_Y[y * dst_stride_Y + x] = data->Y16[y * (pitch / 2) + x] >> 6;
                            }
                        }
                        // UV
                        h = (h + 1) / 2;
                        w = (w + 1) / 2;
                        size_t dst_stride_Cb;
                        uint16_t* dst_mem_Cb = (uint16_t*)heif_image_get_plane2(*out_img, heif_channel_Cb, &dst_stride_Cb);
                        size_t dst_stride_Cr;
                        uint16_t* dst_mem_Cr = (uint16_t*)heif_image_get_plane2(*out_img, heif_channel_Cr, &dst_stride_Cr);
                        dst_stride_Cb /= 2;
                        dst_stride_Cr /= 2;
                        pitch /= 2;
                        for (int y = 0; y < h; y++) {
                            for (int x = 0; x < w; x++) {
                                dst_mem_Cb[y * dst_stride_Cb + x] = ((uint16_t*)data->UV)[y * pitch + x * 2] >> 6;
                                dst_mem_Cr[y * dst_stride_Cr + x] = ((uint16_t*)data->UV)[y * pitch + x * 2 + 1] >> 6;
                            }
                        }
                    }
                                        break;
                    case MFX_FOURCC_YUY2: { // YUYV YUYV ....
                        size_t dst_stride_Cb;
                        uint8_t* dst_mem_Cb = (uint8_t*)heif_image_get_plane2(*out_img, heif_channel_Cb, &dst_stride_Cb);
                        size_t dst_stride_Cr;
                        uint8_t* dst_mem_Cr = (uint8_t*)heif_image_get_plane2(*out_img, heif_channel_Cr, &dst_stride_Cr);
                        size_t dst_stride_Y;
                        uint8_t* dst_mem_Y = (uint8_t*)heif_image_get_plane2(*out_img, heif_channel_Y, &dst_stride_Y);

                        for (int y = 0; y < h; y++) {
                            for (int x = 0; x < (w + 1) / 2; x++) {
                                dst_mem_Y[y * dst_stride_Y + x * 2] = data->Y[y * pitch + x * 4];
                                dst_mem_Cb[y * dst_stride_Cb + x] = data->Y[y * pitch + x * 4 + 1];
                                if (x * 2 + 1 < w)
                                    dst_mem_Y[y * dst_stride_Y + x * 2 + 1] = data->Y[y * pitch + x * 4 + 2];
                                dst_mem_Cr[y * dst_stride_Cr + x] = data->Y[y * pitch + x * 4 + 3];
                            }
                        }
                    }
                                        break;
                    case MFX_FOURCC_Y210: { // YUYV YUYV ....
                        size_t dst_stride_Cb;
                        uint16_t* dst_mem_Cb = (uint16_t*)heif_image_get_plane2(*out_img, heif_channel_Cb, &dst_stride_Cb);
                        size_t dst_stride_Cr;
                        uint16_t* dst_mem_Cr = (uint16_t*)heif_image_get_plane2(*out_img, heif_channel_Cr, &dst_stride_Cr);
                        size_t dst_stride_Y;
                        uint16_t* dst_mem_Y = (uint16_t*)heif_image_get_plane2(*out_img, heif_channel_Y, &dst_stride_Y);
                        dst_stride_Cb /= 2;
                        dst_stride_Cr /= 2;
                        dst_stride_Y /= 2;

                        pitch /= 2;
                        for (int y = 0; y < h; y++) {
                            for (int x = 0; x < (w + 1) / 2; x++) {
                                dst_mem_Y[y * dst_stride_Y + x * 2] = data->Y16[y * pitch + x * 4] >> 6;
                                dst_mem_Cb[y * dst_stride_Cb + x] = data->Y16[y * pitch + x * 4 + 1] >> 6;
                                if(x * 2 + 1 < w)
                                    dst_mem_Y[y * dst_stride_Y + x * 2 + 1] = data->Y16[y * pitch + x * 4 + 2] >> 6;
                                dst_mem_Cr[y * dst_stride_Cr + x] = data->Y16[y * pitch + x * 4 + 3] >> 6;
                            }
                        }
                    }
                                        break;
                    default:
                        return err;
                    }
                }
            }

            if (sts != MFX_WRN_IN_EXECUTION) {
                sts = decoder->images->decSurfaceOut->FrameInterface->Release(decoder->images->decSurfaceOut);
                if(sts != MFX_ERR_NONE)
                    return err; // "Could not release decode output surface"
                intelvpl_decoder_image_chain* next = decoder->images->next;
                if (decoder->images_current == decoder->images) {
                    decoder->images_current = next;
                }
                delete decoder->images;
                decoder->images = next;
            }


        } while (sts == MFX_WRN_IN_EXECUTION);
        // Set NCLX
        if (decoder->nclx_info.ColourDescriptionPresent) {
            heif_color_profile_nclx* nclx = heif_nclx_color_profile_alloc();
            heif_error nclx_err[3];
            nclx->full_range_flag = !!decoder->nclx_info.VideoFullRange;
            nclx_err[0] = heif_nclx_color_profile_set_color_primaries(nclx, decoder->nclx_info.ColourPrimaries);
            nclx_err[1] = heif_nclx_color_profile_set_transfer_characteristics(nclx, decoder->nclx_info.TransferCharacteristics);
            nclx_err[2] = heif_nclx_color_profile_set_matrix_coefficients(nclx, decoder->nclx_info.MatrixCoefficients);
            if(nclx_err[0].code == heif_error_Ok && nclx_err[1].code == heif_error_Ok && nclx_err[2].code == heif_error_Ok)
                heif_image_set_nclx_color_profile(*out_img, nclx);
            heif_nclx_color_profile_free(nclx);
        }
        // Set Content Light Level
        if (decoder->cll_info.InsertPayloadToggle == MFX_PAYLOAD_IDR) {
            heif_content_light_level cll_inf;
            cll_inf.max_content_light_level = decoder->cll_info.MaxContentLightLevel;
            cll_inf.max_pic_average_light_level = decoder->cll_info.MaxPicAverageLightLevel;
            heif_image_set_content_light_level(*out_img, &cll_inf);
        }
        // Set Mastering Display Colour Volume
        if (decoder->mdcv_info.InsertPayloadToggle == MFX_PAYLOAD_IDR) {
            heif_mastering_display_colour_volume mdcv_inf;
            mdcv_inf.white_point_x = decoder->mdcv_info.WhitePointX;
            mdcv_inf.white_point_y = decoder->mdcv_info.WhitePointY;
            for (char rgb = 0; rgb < 3; rgb++) {
                mdcv_inf.display_primaries_x[rgb] = decoder->mdcv_info.DisplayPrimariesX[rgb];
                mdcv_inf.display_primaries_y[rgb] = decoder->mdcv_info.DisplayPrimariesY[rgb];
            }
            mdcv_inf.max_display_mastering_luminance = decoder->mdcv_info.MaxDisplayMasteringLuminance;
            mdcv_inf.min_display_mastering_luminance = decoder->mdcv_info.MinDisplayMasteringLuminance;
            heif_image_set_mastering_display_colour_volume(*out_img, &mdcv_inf);
        }
        return { heif_error_Ok, heif_suberror_Unspecified, kSuccess };
    }
else {
    return err;
    }
}


static heif_error intelvpl_decode_next_image(void* decoder_raw,
                                                heif_image** out_img,
                                                const heif_security_limits* limits)
{
  return intelvpl_decode_next_image2(decoder_raw, out_img, nullptr, limits);
}

static heif_error intelvpl_decode_image(void* decoder_raw,
                                           heif_image** out_img)
{
  auto* limits = heif_get_global_security_limits();
  return intelvpl_decode_next_image(decoder_raw, out_img, limits);
}

static const heif_decoder_plugin decoder_libvpl
    {
        5,
        intelvpl_plugin_name,
        intelvpl_init_plugin,
        intelvpl_deinit_plugin,
        intelvpl_does_support_format,
        intelvpl_new_decoder,
        intelvpl_free_decoder,
        intelvpl_push_data,
        intelvpl_decode_image,
        intelvpl_set_strict_decoding,
        "intelvpl",
        intelvpl_decode_next_image,
        /* minimum_required_libheif_version */ LIBHEIF_MAKE_VERSION(1,21,0),
        intelvpl_does_support_format2,
        intelvpl_new_decoder2,
        intelvpl_push_data2,
        intelvpl_flush_data,
        intelvpl_decode_next_image2
    };

const heif_decoder_plugin* get_decoder_plugin_libvpl()
{
  return &decoder_libvpl;
}



#if PLUGIN_INTELVPL
heif_plugin_info plugin_info {
  1,
  heif_plugin_type_decoder,
  &decoder_libvpl
};
#endif
