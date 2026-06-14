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

/*
* Intel VPL acceleration. https://intel.github.io/libvpl/latest/index.html
*/
#ifndef LIBHEIF_HEIF_HARDWARE_INTEL_QSV_H
#define LIBHEIF_HEIF_HARDWARE_INTEL_QSV_H

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

#ifdef LIBVA_SUPPORT
#include "va/va.h"
#include "va/va_drm.h"
#endif

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

#define WAIT_100_MILLISECONDS 100

#define ALIGN16(value)           (((value + 15) >> 4) << 4)
#define VPLVERSION(major, minor) (major << 16 | minor)

extern mfxSession intel_qsv_session;

#endif
