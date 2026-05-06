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
#define IS_ARG_EQ(a, b)       (!strcmp((a), (b)))

#define VERIFY(x, y)       \
    if (!(x)) {            \
        printf("%s\n", y); \
        isFailed = true;   \
        goto end;          \
    }

#define ALIGN16(value)           (((value + 15) >> 4) << 4)
#define ALIGN32(X)               (((mfxU32)((X) + 31)) & (~(mfxU32)31))
#define VPLVERSION(major, minor) (major << 16 | minor)

enum ExampleParams { PARAM_IMPL = 0, PARAM_INFILE, PARAM_INRES, PARAM_COUNT };
enum ParamGroup {
    PARAMS_CREATESESSION = 0,
    PARAMS_DECODE,
    PARAMS_ENCODE,
    PARAMS_VPP,
    PARAMS_TRANSCODE
};

typedef struct _Params {
    char* infileName;
    char* inmodelName;

    mfxU16 srcWidth;
    mfxU16 srcHeight;
} Params;

void* InitAcceleratorHandle(mfxSession session, int* fd) {
    mfxIMPL impl;
    mfxStatus sts = MFXQueryIMPL(session, &impl);
    if (sts != MFX_ERR_NONE)
        return NULL;

#ifdef LIBVA_SUPPORT
    if ((impl & MFX_IMPL_VIA_VAAPI) == MFX_IMPL_VIA_VAAPI) {
        if (!fd)
            return NULL;
        VADisplay va_dpy = NULL;
        // initialize VAAPI context and set session handle (req in Linux)
        *fd = open("/dev/dri/renderD128", O_RDWR);
        if (*fd >= 0) {
            va_dpy = vaGetDisplayDRM(*fd);
            if (va_dpy) {
                int major_version = 0, minor_version = 0;
                if (VA_STATUS_SUCCESS == vaInitialize(va_dpy, &major_version, &minor_version)) {
                    MFXVideoCORE_SetHandle(session,
                        static_cast<mfxHandleType>(MFX_HANDLE_VA_DISPLAY),
                        va_dpy);
                }
            }
        }
        return va_dpy;
    }
#endif

    return NULL;
}

void FreeAcceleratorHandle(void* accelHandle, int fd) {
#ifdef LIBVA_SUPPORT
    if (accelHandle) {
        vaTerminate((VADisplay)accelHandle);
    }
    if (fd) {
        close(fd);
    }
#endif
}

//Shows implementation info for Media SDK or Intel® VPL
mfxVersion ShowImplInfo(mfxSession session) {
    mfxIMPL impl;
    mfxVersion version = { 0, 1 };

    mfxStatus sts = MFXQueryIMPL(session, &impl);
    if (sts != MFX_ERR_NONE)
        return version;

    sts = MFXQueryVersion(session, &version);
    if (sts != MFX_ERR_NONE)
        return version;

    printf("Session loaded: ApiVersion = %d.%d \timpl= ", version.Major, version.Minor);

    switch (impl) {
    case MFX_IMPL_SOFTWARE:
        puts("Software");
        break;
    case MFX_IMPL_HARDWARE | MFX_IMPL_VIA_VAAPI:
        puts("Hardware:VAAPI");
        break;
    case MFX_IMPL_HARDWARE | MFX_IMPL_VIA_D3D11:
        puts("Hardware:D3D11");
        break;
    case MFX_IMPL_HARDWARE | MFX_IMPL_VIA_D3D9:
        puts("Hardware:D3D9");
        break;
    default:
        puts("Unknown");
        break;
    }

    return version;
}

// Shows implementation info with Intel® VPL
void ShowImplementationInfo(mfxLoader loader, mfxU32 implnum) {
    mfxImplDescription* idesc = nullptr;
    mfxStatus sts;
    //Loads info about implementation at specified list location
    sts = MFXEnumImplementations(loader, implnum, MFX_IMPLCAPS_IMPLDESCSTRUCTURE, (mfxHDL*)&idesc);
    if (!idesc || (sts != MFX_ERR_NONE))
        return;

    printf("Implementation details:\n");
    printf("  ApiVersion:           %hu.%hu  \n", idesc->ApiVersion.Major, idesc->ApiVersion.Minor);
    printf("  Implementation type:  HW\n");
    printf("  AccelerationMode via: ");
    switch (idesc->AccelerationMode) {
    case MFX_ACCEL_MODE_NA:
        printf("NA \n");
        break;
    case MFX_ACCEL_MODE_VIA_D3D9:
        printf("D3D9\n");
        break;
    case MFX_ACCEL_MODE_VIA_D3D11:
        printf("D3D11\n");
        break;
    case MFX_ACCEL_MODE_VIA_VAAPI:
        printf("VAAPI\n");
        break;
    case MFX_ACCEL_MODE_VIA_VAAPI_DRM_MODESET:
        printf("VAAPI_DRM_MODESET\n");
        break;
    case MFX_ACCEL_MODE_VIA_VAAPI_GLX:
        printf("VAAPI_GLX\n");
        break;
    case MFX_ACCEL_MODE_VIA_VAAPI_X11:
        printf("VAAPI_X11\n");
        break;
    case MFX_ACCEL_MODE_VIA_VAAPI_WAYLAND:
        printf("VAAPI_WAYLAND\n");
        break;
    case MFX_ACCEL_MODE_VIA_HDDLUNITE:
        printf("HDDLUNITE\n");
        break;
    default:
        printf("unknown\n");
        break;
    }
    printf("  DeviceID:             %s \n", idesc->Dev.DeviceID);
    MFXDispReleaseImplDescription(loader, idesc);

#if (MFX_VERSION >= 2004)
    //Show implementation path, added in 2.4 API
    mfxHDL implPath = nullptr;
    sts = MFXEnumImplementations(loader, implnum, MFX_IMPLCAPS_IMPLPATH, &implPath);
    if (!implPath || (sts != MFX_ERR_NONE))
        return;

    printf("  Path: %s\n\n", reinterpret_cast<mfxChar*>(implPath));
    MFXDispReleaseImplDescription(loader, implPath);
#endif
}

void PrepareFrameInfo(mfxFrameInfo* fi, mfxU32 format, mfxU16 w, mfxU16 h) {
    // Video processing input data format
    fi->FourCC = format;
    fi->ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    fi->CropX = 0;
    fi->CropY = 0;
    fi->CropW = w;
    fi->CropH = h;
    fi->PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    fi->FrameRateExtN = 30;
    fi->FrameRateExtD = 1;
    // width must be a multiple of 16
    // height must be a multiple of 16 in case of frame picture and a multiple of 32 in case of field picture
    fi->Width = ALIGN16(fi->CropW);
    fi->Height =
        (MFX_PICSTRUCT_PROGRESSIVE == fi->PicStruct) ? ALIGN16(fi->CropH) : ALIGN32(fi->CropH);
}

mfxU32 GetSurfaceSize(mfxU32 FourCC, mfxU32 width, mfxU32 height) {
    mfxU32 nbytes = 0;

    switch (FourCC) {
    case MFX_FOURCC_I420:
    case MFX_FOURCC_NV12:
        nbytes = width * height + (width >> 1) * (height >> 1) + (width >> 1) * (height >> 1);
        break;
    case MFX_FOURCC_I010:
    case MFX_FOURCC_P010:
        nbytes = width * height + (width >> 1) * (height >> 1) + (width >> 1) * (height >> 1);
        nbytes *= 2;
        break;
    case MFX_FOURCC_RGB4:
    case MFX_FOURCC_BGR4:
        nbytes = width * height * 4;
        break;
    default:
        break;
    }

    return nbytes;
}

int GetFreeSurfaceIndex(mfxFrameSurface1* SurfacesPool, mfxU16 nPoolSize) {
    for (mfxU16 i = 0; i < nPoolSize; i++) {
        if (0 == SurfacesPool[i].Data.Locked)
            return i;
    }
    return MFX_ERR_NOT_FOUND;
}

mfxStatus AllocateExternalSystemMemorySurfacePool(mfxU8** buf,
    mfxFrameSurface1* surfpool,
    mfxFrameInfo frame_info,
    mfxU16 surfnum) {
    // initialize surface pool (I420, RGB4 format)
    mfxU32 surfaceSize = GetSurfaceSize(frame_info.FourCC, frame_info.Width, frame_info.Height);
    if (!surfaceSize)
        return MFX_ERR_MEMORY_ALLOC;

    size_t framePoolBufSize = static_cast<size_t>(surfaceSize) * surfnum;
    *buf = reinterpret_cast<mfxU8*>(calloc(framePoolBufSize, 1));

    mfxU16 surfW;
    mfxU16 surfH = frame_info.Height;

    if (frame_info.FourCC == MFX_FOURCC_RGB4) {
        surfW = frame_info.Width * 4;

        for (mfxU32 i = 0; i < surfnum; i++) {
            surfpool[i] = { 0 };
            surfpool[i].Info = frame_info;
            size_t buf_offset = static_cast<size_t>(i) * surfaceSize;
            surfpool[i].Data.B = *buf + buf_offset;
            surfpool[i].Data.G = surfpool[i].Data.B + 1;
            surfpool[i].Data.R = surfpool[i].Data.B + 2;
            surfpool[i].Data.A = surfpool[i].Data.B + 3;
            surfpool[i].Data.Pitch = surfW;
        }
    }
    else if (frame_info.FourCC == MFX_FOURCC_BGR4) {
        surfW = frame_info.Width * 4;

        for (mfxU32 i = 0; i < surfnum; i++) {
            surfpool[i] = { 0 };
            surfpool[i].Info = frame_info;
            size_t buf_offset = static_cast<size_t>(i) * surfaceSize;
            surfpool[i].Data.R = *buf + buf_offset;
            surfpool[i].Data.G = surfpool[i].Data.R + 1;
            surfpool[i].Data.B = surfpool[i].Data.R + 2;
            surfpool[i].Data.A = surfpool[i].Data.R + 3;
            surfpool[i].Data.Pitch = surfW;
        }
    }
    else {
        surfW = (frame_info.FourCC == MFX_FOURCC_P010) ? frame_info.Width * 2 : frame_info.Width;

        for (mfxU32 i = 0; i < surfnum; i++) {
            surfpool[i] = { 0 };
            surfpool[i].Info = frame_info;
            size_t buf_offset = static_cast<size_t>(i) * surfaceSize;
            surfpool[i].Data.Y = *buf + buf_offset;
            surfpool[i].Data.U = *buf + buf_offset + (surfW * surfH);
            surfpool[i].Data.V = surfpool[i].Data.U + ((surfW / 2) * (surfH / 2));
            surfpool[i].Data.Pitch = surfW;
        }
    }

    return MFX_ERR_NONE;
}

void FreeExternalSystemMemorySurfacePool(mfxU8* dec_buf, mfxFrameSurface1* surfpool) {
    if (dec_buf) {
        free(dec_buf);
    }

    if (surfpool)
        free(surfpool);
}

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
    switch (info->FourCC) {
    case MFX_FOURCC_NV12:
    case MFX_FOURCC_I420:
    case MFX_FOURCC_P010:
        return heif_chroma_420;
    case MFX_FOURCC_RGB4:
        return heif_chroma_interleaved_RGBA;
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

                mfxStatus sts = decoder->images->decSurfaceOut->FrameInterface->Map(decoder->images->decSurfaceOut, MFX_MAP_READ);
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
                    switch (info->FourCC) {
                    case MFX_FOURCC_NV12: {
                        // Y
                        err = heif_image_add_plane_safe(*out_img, heif_channel_Y, w, h, 8, limits);
                        if (err.code) {
                            // copy error message to decoder object because heif_image will be released
                            decoder->error_message = err.message;
                            err.message = decoder->error_message.c_str();

                            heif_image_release(*out_img);
                            out_img = NULL;
                            return err;
                        }
                        size_t dst_stride_Y;
                        uint8_t* dst_mem_Y = heif_image_get_plane2(*out_img, heif_channel_Y, &dst_stride_Y);

                        pitch = data->PitchHigh << 16 | data->Pitch;
                        for (int y = 0; y < h; y++) {
                            memcpy(dst_mem_Y + y * dst_stride_Y, data->Y + y * pitch, w);
                        }
                        // UV
                        h = (h + 1) / 2;
                        w = (w + 1) / 2;
                        err = heif_image_add_plane_safe(*out_img, heif_channel_Cb, w, h, 8, limits);
                        size_t dst_stride_Cb;
                        uint8_t* dst_mem_Cb = heif_image_get_plane2(*out_img, heif_channel_Cb, &dst_stride_Cb);
                        err = heif_image_add_plane_safe(*out_img, heif_channel_Cr, w, h, 8, limits);
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
                    case MFX_FOURCC_I420: {
                        // Y
                        err = heif_image_add_plane_safe(*out_img, heif_channel_Y, w, h, 8, limits);
                        if (err.code) {
                            // copy error message to decoder object because heif_image will be released
                            decoder->error_message = err.message;
                            err.message = decoder->error_message.c_str();

                            heif_image_release(*out_img);
                            out_img = NULL;
                            return err;
                        }
                        size_t dst_stride_Y;
                        uint8_t* dst_mem_Y = heif_image_get_plane2(*out_img, heif_channel_Y, &dst_stride_Y);

                        pitch = data->Pitch;
                        for (int y = 0; y < h; y++) {
                            memcpy(dst_mem_Y + y * dst_stride_Y, data->Y + y * pitch, w);
                        }
                        // U
                        h /= 2;
                        pitch /= 2;
                        err = heif_image_add_plane_safe(*out_img, heif_channel_Cb, w, h, 8, limits);
                        size_t dst_stride_Cb;
                        uint8_t* dst_mem_Cb = heif_image_get_plane2(*out_img, heif_channel_Cb, &dst_stride_Cb);
                        for (int y = 0; y < h; y++) {
                            for (int x = 0; x < w / 2; x++) {
                                dst_mem_Cb[y * dst_stride_Cb + x] = data->U[y * pitch + x];
                            }
                        }
                        err = heif_image_add_plane_safe(*out_img, heif_channel_Cr, w, h, 8, limits);
                        size_t dst_stride_Cr;
                        uint8_t* dst_mem_Cr = heif_image_get_plane2(*out_img, heif_channel_Cr, &dst_stride_Cr);
                        for (int y = 0; y < h; y++) {
                            for (int x = 0; x < w / 2; x++) {
                                dst_mem_Cr[y * dst_stride_Cr + x] = data->V[y * pitch + x];
                            }
                        }
                    }
                        break;
                    case MFX_FOURCC_RGB4: {
                        // R
                        err = heif_image_add_plane_safe(*out_img, heif_channel_interleaved, w, h, 8, limits);
                        if (err.code) {
                            // copy error message to decoder object because heif_image will be released
                            decoder->error_message = err.message;
                            err.message = decoder->error_message.c_str();

                            heif_image_release(*out_img);
                            out_img = NULL;
                            return err;
                        }
                        size_t dst_stride;
                        uint8_t* dst_mem = heif_image_get_plane2(*out_img, heif_channel_interleaved, &dst_stride);
                        pitch = data->Pitch;
                        for (int y = 0; y < h; y++) {
                            for (int x = 0; x < w; x++) {
                                //bytes_read = fread(data->B + i * pitch, 1, pitch, f);
                                dst_mem[y * dst_stride + x * 4 + 0] = data->B[y * pitch + x * 4 + 2]; // R
                                dst_mem[y * dst_stride + x * 4 + 1] = data->B[y * pitch + x * 4 + 1]; // G
                                dst_mem[y * dst_stride + x * 4 + 2] = data->B[y * pitch + x * 4 + 0]; // B
                                dst_mem[y * dst_stride + x * 4 + 3] = data->B[y * pitch + x * 4 + 3]; // A
                            }
                        }
                    }
                        break;
                    case MFX_FOURCC_P010: {
                        // Y
                        err = heif_image_add_plane_safe(*out_img, heif_channel_Y, w, h, 10, limits);
                        if (err.code) {
                            // copy error message to decoder object because heif_image will be released
                            decoder->error_message = err.message;
                            err.message = decoder->error_message.c_str();

                            heif_image_release(*out_img);
                            out_img = NULL;
                            return err;
                        }
                        size_t dst_stride_Y;
                        uint16_t* dst_mem_Y = (uint16_t *)heif_image_get_plane2(*out_img, heif_channel_Y, &dst_stride_Y);
                        dst_stride_Y /= 2;

                        pitch = data->PitchHigh << 16 | data->Pitch;
                        for (int y = 0; y < h; y++) {
                            for (int x = 0; x < w; x++) {
                                dst_mem_Y[y * dst_stride_Y + x] = data->Y16[y * (pitch / 2) + x] >> 6;
                            }
                        }
                        // UV
                        h = (h + 1) / 2;
                        w = (w + 1) / 2;
                        err = heif_image_add_plane_safe(*out_img, heif_channel_Cb, w, h, 10, limits);
                        size_t dst_stride_Cb;
                        uint16_t* dst_mem_Cb = (uint16_t*)heif_image_get_plane2(*out_img, heif_channel_Cb, &dst_stride_Cb);
                        err = heif_image_add_plane_safe(*out_img, heif_channel_Cr, w, h, 10, limits);
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
                    default:
                        return err;
                    }
                }

                sts = decoder->images->decSurfaceOut->FrameInterface->Unmap(decoder->images->decSurfaceOut);
                if (sts != MFX_ERR_NONE) {
                    return err; //"mfxFrameSurfaceInterface->Unmap failed (%d)\n"
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
