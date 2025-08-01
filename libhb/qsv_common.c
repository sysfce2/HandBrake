/* qsv_common.c
 *
 * Copyright (c) 2003-2025 HandBrake Team
 * This file is part of the HandBrake source code.
 * Homepage: <http://handbrake.fr/>.
 * It may be used under the terms of the GNU General Public License v2.
 * For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

#include "handbrake/handbrake.h"
#include "handbrake/project.h"

#if HB_PROJECT_FEATURE_QSV

#include <stdio.h>
#include <string.h>

#include "vpl/mfxvideo.h"
#include "vpl/mfxdispatcher.h"

#include "handbrake/ports.h"
#include "handbrake/common.h"
#include "handbrake/hwaccel.h"
#include "handbrake/hb_dict.h"
#include "handbrake/qsv_common.h"
#include "handbrake/h264_common.h"
#include "handbrake/h265_common.h"
#include "handbrake/av1_common.h"
#include "handbrake/hbffmpeg.h"

#ifndef HB_QSV_PRINT_RET_MSG
#define HB_QSV_PRINT_RET_MSG(ERR)              { fprintf(stderr, "Error code %d,\t%s\t%d\n", ERR, __FUNCTION__, __LINE__); }
#endif

#ifndef HB_QSV_DEBUG_ASSERT
#define HB_QSV_DEBUG_ASSERT(x,y)               { if ((x)) { fprintf(stderr, "\nASSERT: %s\n", y); } }
#endif

#define HB_QSV_CHECK_RET(P, X, ERR)                {if ((X) > (P)) {HB_QSV_PRINT_RET_MSG(ERR); return;}}
#define HB_QSV_CHECK_RESULT(P, X, ERR)             {if ((X) > (P)) {HB_QSV_PRINT_RET_MSG(ERR); return ERR;}}
#define HB_QSV_CHECK_POINTER(P, ERR)               {if (!(P)) {HB_QSV_PRINT_RET_MSG(ERR); return ERR;}}
#define HB_QSV_IGNORE_MFX_STS(P, X)                {if ((X) == (P)) {P = MFX_ERR_NONE;}}

#define HB_QSV_ASYNC_DEPTH_DEFAULT     4

#define HB_QSV_AVC_DECODER_WIDTH_MAX   4096
#define HB_QSV_AVC_DECODER_HEIGHT_MAX  4096

typedef struct hb_qsv_adapter_details
{
    // DirectX index
    int index;
    mfxPlatform platform;
    char *impl_name;
    char *impl_path;
    // QSV info for each codec
    hb_qsv_info_t *hb_qsv_info_avc;
    hb_qsv_info_t *hb_qsv_info_hevc;
    hb_qsv_info_t *hb_qsv_info_av1;
    // API versions
    mfxVersion qsv_software_version;
    mfxVersion qsv_hardware_version;
    // AVC implementations
    hb_qsv_info_t qsv_software_info_avc;
    hb_qsv_info_t qsv_hardware_info_avc;
    // HEVC implementations
    hb_qsv_info_t qsv_software_info_hevc;
    hb_qsv_info_t qsv_hardware_info_hevc;
    // AV1 implementations
    hb_qsv_info_t qsv_hardware_info_av1;
    // Extended device information
    mfxExtendedDeviceId extended_device_id;
} hb_qsv_adapter_details_t;

static hb_list_t *g_qsv_adapters_list         = NULL;
static hb_list_t *g_qsv_adapters_details_list = NULL;
static int g_adapter_index = 0;
static int g_default_adapter_index = 0;
static int qsv_init_done = 0;
static int qsv_init_result = 0;

static void init_adapter_details(hb_qsv_adapter_details_t *adapter_details)
{
    adapter_details->index                                 = 0;
    adapter_details->platform.CodeName                     = MFX_PLATFORM_UNKNOWN;
    adapter_details->platform.MediaAdapterType             = MFX_MEDIA_UNKNOWN;
    // QSV info for each codec
    adapter_details->hb_qsv_info_avc                       = NULL;
    adapter_details->hb_qsv_info_hevc                      = NULL;
    adapter_details->hb_qsv_info_av1                       = NULL;
    // API versions
    adapter_details->qsv_software_version.Version          = 0;
    adapter_details->qsv_hardware_version.Version          = 0;
    // AVC implementations
    adapter_details->qsv_software_info_avc.available       = 0;
    adapter_details->qsv_software_info_avc.codec_id        = MFX_CODEC_AVC;
    adapter_details->qsv_software_info_avc.implementation  = MFX_IMPL_SOFTWARE;

    adapter_details->qsv_hardware_info_avc.available       = 0;
    adapter_details->qsv_hardware_info_avc.codec_id        = MFX_CODEC_AVC;
    adapter_details->qsv_hardware_info_avc.implementation  = MFX_IMPL_HARDWARE_ANY|MFX_IMPL_VIA_ANY;
    // HEVC implementations
    adapter_details->qsv_software_info_hevc.available      = 0;
    adapter_details->qsv_software_info_hevc.codec_id       = MFX_CODEC_HEVC;
    adapter_details->qsv_software_info_hevc.implementation = MFX_IMPL_SOFTWARE;

    adapter_details->qsv_hardware_info_hevc.available      = 0;
    adapter_details->qsv_hardware_info_hevc.codec_id       = MFX_CODEC_HEVC;
    adapter_details->qsv_hardware_info_hevc.implementation = MFX_IMPL_HARDWARE_ANY|MFX_IMPL_VIA_ANY;

    // AV1 implementations
    adapter_details->qsv_hardware_info_av1.available      = 0;
    adapter_details->qsv_hardware_info_av1.codec_id       = MFX_CODEC_AV1;
    adapter_details->qsv_hardware_info_av1.implementation = MFX_IMPL_HARDWARE_ANY|MFX_IMPL_VIA_ANY;
}

// QSV info about adapters
static const char* hb_qsv_get_adapter_type(const hb_qsv_adapter_details_t *details);
static int hb_qsv_make_adapters_list(hb_list_t **qsv_adapters_list, hb_list_t **qsv_adapters_details_list);
static int hb_qsv_collect_adapters_details(hb_list_t *hb_qsv_adapter_details_list);

// QSV-supported profile and level lists (not all exposed to the user)
static hb_triplet_t hb_qsv_h264_profiles[] =
{
    { "Baseline",             "baseline",       MFX_PROFILE_AVC_BASELINE,             },
    { "Main",                 "main",           MFX_PROFILE_AVC_MAIN,                 },
    { "Extended",             "extended",       MFX_PROFILE_AVC_EXTENDED,             },
    { "High",                 "high",           MFX_PROFILE_AVC_HIGH,                 },
    { "High 4:2:2",           "high422",        MFX_PROFILE_AVC_HIGH_422,             },
    { "Constrained Baseline", "baseline|set1",  MFX_PROFILE_AVC_CONSTRAINED_BASELINE, },
    { "Constrained High",     "high|set4|set5", MFX_PROFILE_AVC_CONSTRAINED_HIGH,     },
    { "Progressive High",     "high|set4",      MFX_PROFILE_AVC_PROGRESSIVE_HIGH,     },
    { NULL,                                                                           },
};
static hb_triplet_t hb_qsv_h265_profiles[] =
{
    { "Main",               "main",             MFX_PROFILE_HEVC_MAIN,   },
    { "Main 10",            "main10",           MFX_PROFILE_HEVC_MAIN10, },
    { "Main Still Picture", "mainstillpicture", MFX_PROFILE_HEVC_MAINSP, },
    { NULL,                                                              },
};
static const char * const hb_qsv_h265_profiles_names_10bit[] = {
    "auto",
    "main10",
    NULL,
};
static hb_triplet_t hb_qsv_av1_profiles[] =
{
    { "Main",               "main",             MFX_PROFILE_AV1_MAIN,    },
    { "High",               "high",             MFX_PROFILE_AV1_HIGH,    },
    { "Professional",       "professional",     MFX_PROFILE_AV1_PRO,     },
    { NULL,                                                              },
};
static const char * const hb_qsv_av1_profiles_names[] =
{
    "auto",
    "main",
    NULL,
};
static hb_triplet_t hb_qsv_vpp_scale_modes[] =
{
    { "auto",              "auto",              MFX_SCALING_MODE_DEFAULT,  },
    { "lowpower",          "low_power",         MFX_SCALING_MODE_LOWPOWER, },
    { "hq",                "hq",                MFX_SCALING_MODE_QUALITY,  },
    { "compute",           "compute",           3                          },
    { "vd",                "vd",                4                          },
    { "ve",                "ve",                5                          },
    { NULL,                                                                },
};

static hb_triplet_t hb_qsv_memory_types[] =
{
    { "System memory",         "system",        MFX_IOPATTERN_OUT_SYSTEM_MEMORY, },
    { "Video memory",          "video",         MFX_IOPATTERN_OUT_VIDEO_MEMORY,  },
    { NULL,                                                                      },
};

static hb_triplet_t hb_qsv_hyper_encode_modes[] =
{
    { "Hyper Encode off",      "off",           MFX_HYPERMODE_OFF,      },
    { "Hyper Encode on",       "on",            MFX_HYPERMODE_ON,       },
    { "Hyper Encode adaptive", "adaptive",      MFX_HYPERMODE_ADAPTIVE, },
    { NULL,                                                             },
};

static const enum AVPixelFormat hb_qsv_pix_fmts[] =
{
    AV_PIX_FMT_NV12, AV_PIX_FMT_NONE
};

static const enum AVPixelFormat hb_qsv_10bit_pix_fmts[] =
{
    AV_PIX_FMT_P010LE, AV_PIX_FMT_NONE
};

#define MFX_IMPL_VIA_MASK(impl) (0x0f00 & (impl))

// check available Intel Media SDK version against a minimum
#define HB_CHECK_MFX_VERSION(MFX_VERSION, MAJOR, MINOR) \
    ((MFX_VERSION.Major * 1000 + MFX_VERSION.Minor) >= (MAJOR * 1000 + MINOR))

int hb_qsv_get_adapter_index()
{
    return g_adapter_index;
}

static int hb_qsv_set_default_adapter_index(int adapter_index)
{
    g_default_adapter_index = adapter_index;
    return 0;
}

static int hb_qsv_get_default_adapter_index()
{
    return g_default_adapter_index;
}

hb_list_t* hb_qsv_adapters_list()
{
    return g_qsv_adapters_list;
}

static hb_qsv_adapter_details_t* hb_qsv_get_adapters_details_by_index(int adapter_index)
{
    for (int i = 0; i < hb_list_count(g_qsv_adapters_details_list); i++)
    {
        const hb_qsv_adapter_details_t *details = hb_list_item(g_qsv_adapters_details_list, i);
        if (details->index == adapter_index || adapter_index == -1)
        {
            return details;
        }
    }
    return NULL;
}

int hb_qsv_get_adapter_render_node(int adapter_index)
{
    const hb_qsv_adapter_details_t *details = hb_qsv_get_adapters_details_by_index(adapter_index);
    return details->extended_device_id.DRMRenderNodeNum;
}

int hb_qsv_set_adapter_index(int adapter_index)
{
    if (g_adapter_index == adapter_index)
        return 0;

    for (int i = 0; i < hb_list_count(g_qsv_adapters_details_list); i++)
    {
        const hb_qsv_adapter_details_t *details = hb_list_item(g_qsv_adapters_details_list, i);
        if (details && (details->index == adapter_index))
        {
            g_adapter_index = adapter_index;
            return 0;
        }
    }
    hb_error("hb_qsv_set_adapter_index: incorrect qsv device index %d", adapter_index);
    return -1;
}

static int qsv_impl_set_preferred(hb_qsv_adapter_details_t *details, const char *name)
{
    if (name == NULL || details == NULL)
    {
        return -1;
    }
    if (!strcasecmp(name, "software"))
    {
        if (details->qsv_software_info_avc.available)
        {
            details->hb_qsv_info_avc = &details->qsv_software_info_avc;
        }
        if (details->qsv_software_info_hevc.available)
        {
            details->hb_qsv_info_hevc = &details->qsv_software_info_hevc;
        }
        return 0;
    }
    if (!strcasecmp(name, "hardware"))
    {
        if (details->qsv_hardware_info_avc.available)
        {
            details->hb_qsv_info_avc = &details->qsv_hardware_info_avc;
        }
        if (details->qsv_hardware_info_hevc.available)
        {
            details->hb_qsv_info_hevc = &details->qsv_hardware_info_hevc;
        }
        if (details->qsv_hardware_info_av1.available)
        {
            details->hb_qsv_info_av1 = &details->qsv_hardware_info_av1;
        }
        return 0;
    }
    return -1;
}

int hb_qsv_impl_set_preferred(const char *name)
{
    hb_qsv_adapter_details_t* details = hb_qsv_get_adapters_details_by_index(hb_qsv_get_adapter_index());
    return qsv_impl_set_preferred(details, name);
}

int hb_qsv_hardware_generation(int cpu_platform)
{
    switch (cpu_platform)
    {
        case HB_CPU_PLATFORM_INTEL_BNL:
            return QSV_G0;
        case HB_CPU_PLATFORM_INTEL_SNB:
            return QSV_G1;
        case HB_CPU_PLATFORM_INTEL_IVB:
        case HB_CPU_PLATFORM_INTEL_SLM:
        case HB_CPU_PLATFORM_INTEL_CHT:
            return QSV_G2;
        case HB_CPU_PLATFORM_INTEL_HSW:
            return QSV_G3;
        case HB_CPU_PLATFORM_INTEL_BDW:
            return QSV_G4;
        case HB_CPU_PLATFORM_INTEL_SKL:
            return QSV_G5;
        case HB_CPU_PLATFORM_INTEL_KBL:
        case HB_CPU_PLATFORM_INTEL_CML:
            return QSV_G6;
        case HB_CPU_PLATFORM_INTEL_ICL:
            return QSV_G7;
        case HB_CPU_PLATFORM_INTEL_TGL:
        case HB_CPU_PLATFORM_INTEL_ADL:
            return QSV_G8;
        case HB_CPU_PLATFORM_INTEL_DG2:
            return QSV_G9;
        case HB_CPU_PLATFORM_INTEL_LNL:
            return QSV_G10;
        default:
            return QSV_FU;
    }
}

int qsv_map_mfx_platform_codename(int mfx_platform_codename)
{
    int platform = HB_CPU_PLATFORM_UNSPECIFIED;

    switch (mfx_platform_codename)
    {
    case MFX_PLATFORM_SANDYBRIDGE:
        platform = HB_CPU_PLATFORM_INTEL_SNB;
        break;
    case MFX_PLATFORM_IVYBRIDGE:
        platform = HB_CPU_PLATFORM_INTEL_IVB;
        break;
    case MFX_PLATFORM_HASWELL:
        platform = HB_CPU_PLATFORM_INTEL_HSW;
        break;
    case MFX_PLATFORM_BAYTRAIL:
    case MFX_PLATFORM_BROADWELL:
        platform = HB_CPU_PLATFORM_INTEL_BDW;
        break;
    case MFX_PLATFORM_CHERRYTRAIL:
        platform = HB_CPU_PLATFORM_INTEL_CHT;
        break;
    case MFX_PLATFORM_SKYLAKE:
        platform = HB_CPU_PLATFORM_INTEL_SKL;
        break;
    case MFX_PLATFORM_APOLLOLAKE:
    case MFX_PLATFORM_KABYLAKE:
        platform = HB_CPU_PLATFORM_INTEL_KBL;
        break;
#if (MFX_VERSION >= 1025)
    case MFX_PLATFORM_GEMINILAKE:
    case MFX_PLATFORM_COFFEELAKE:
    case MFX_PLATFORM_CANNONLAKE:
        platform = HB_CPU_PLATFORM_INTEL_KBL;
        break;
#endif
#if (MFX_VERSION >= 1027)
    case MFX_PLATFORM_ICELAKE:
        platform = HB_CPU_PLATFORM_INTEL_ICL;
        break;
#endif
    case MFX_PLATFORM_ELKHARTLAKE:
    case MFX_PLATFORM_JASPERLAKE:
    case MFX_PLATFORM_TIGERLAKE:
    case MFX_PLATFORM_ROCKETLAKE:
        platform = HB_CPU_PLATFORM_INTEL_TGL;
        break;
    case MFX_PLATFORM_ALDERLAKE_S:
    case MFX_PLATFORM_ALDERLAKE_P:
        platform = HB_CPU_PLATFORM_INTEL_ADL;
        break;
    case MFX_PLATFORM_ARCTICSOUND_P:
    case MFX_PLATFORM_DG2:
    case MFX_PLATFORM_ALDERLAKE_N:
    case MFX_PLATFORM_KEEMBAY:
    case MFX_PLATFORM_METEORLAKE:
    case MFX_PLATFORM_BATTLEMAGE:
    case MFX_PLATFORM_ARROWLAKE:
        platform = HB_CPU_PLATFORM_INTEL_DG2;
        break;
    case MFX_PLATFORM_LUNARLAKE:
        platform = HB_CPU_PLATFORM_INTEL_LNL;
        break;
    default:
        platform = HB_CPU_PLATFORM_UNSPECIFIED;
    }
    return platform;
}

static const char* hb_qsv_get_adapter_type(const hb_qsv_adapter_details_t *details)
{
    if (details)
    {
        return (details->platform.MediaAdapterType == MFX_MEDIA_INTEGRATED) ? "integrated" :
        (details->platform.MediaAdapterType == MFX_MEDIA_DISCRETE) ? "discrete" : "unknown";
    }
    return NULL;
}

/*
 * Determine whether a given mfxIMPL is hardware-accelerated.
 */
int hb_qsv_implementation_is_hardware(mfxIMPL implementation)
{
    return MFX_IMPL_BASETYPE(implementation) != MFX_IMPL_SOFTWARE;
}

int hb_qsv_info_init()
{
    int result;
    result = hb_qsv_make_adapters_list(&g_qsv_adapters_list, &g_qsv_adapters_details_list);
    if (result != 0)
    {
        hb_error("hb_qsv_info_init: hb_qsv_make_adapters_list failed");
        return result;
    }
    result = hb_qsv_collect_adapters_details(g_qsv_adapters_details_list);
    if (result != 0)
    {
        hb_error("hb_qsv_info_init: hb_qsv_collect_adapters_details failed");
        return result;
    }
    if (hb_list_count(g_qsv_adapters_details_list) == 0)
    {
        hb_deep_log(1, "hb_qsv_info_init: g_qsv_adapters_details_list has no adapters");
        return -1;
    }
    return 0;
}

#if defined(_WIN32) || defined(__MINGW32__)
static void hb_qsv_free_adapters_details(hb_list_t *qsv_adapters_details_list)
{
    for (int i = 0; i < hb_list_count(qsv_adapters_details_list); i++)
    {
        hb_qsv_adapter_details_t *details = hb_list_item(qsv_adapters_details_list, i);
        if (details)
        {
            av_free(details);
        }
    }
}
#endif

void hb_qsv_info_close()
{
    if (g_qsv_adapters_details_list)
    {
#if defined(_WIN32) || defined(__MINGW32__)
        hb_qsv_free_adapters_details(g_qsv_adapters_details_list);
#endif
        hb_list_close(&g_qsv_adapters_details_list);
        g_qsv_adapters_details_list = NULL;
    }
    if (g_qsv_adapters_list)
    {
        hb_list_close(&g_qsv_adapters_list);
        g_qsv_adapters_list = NULL;
    }
}

static int hb_qsv_make_adapters_list(hb_list_t **qsv_adapters_list, hb_list_t **qsv_adapters_details_list)
{
    if (!qsv_adapters_list)
    {
        hb_error("hb_qsv_make_adapters_list: qsv_adapters_list destination pointer is NULL");
        return -1;
    }
    if (*qsv_adapters_list)
    {
        hb_error("hb_qsv_make_adapters_list: qsv_adapters_list is allocated already");
        return -1;
    }

    if (!qsv_adapters_details_list)
    {
        hb_error("hb_qsv_make_adapters_list: qsv_adapters_details_list destination pointer is NULL");
        return -1;
    }

    if (*qsv_adapters_details_list)
    {
        hb_error("hb_qsv_make_adapters_list: qsv_adapter_details_list is allocated already");
        return -1;
    }

    hb_list_t *list = hb_list_init();
    if (list == NULL)
    {
        hb_error("hb_qsv_make_adapters_list: hb_list_init() failed");
        return -1;
    }
    hb_list_t *list2 = hb_list_init();
    if (list == NULL)
    {
        hb_error("hb_qsv_make_adapters_list: hb_list_init() failed");
        return -1;
    }

    mfxLoader loader = MFXLoad();
    if (loader == NULL)
    {
        hb_error("hb_qsv_make_adapters_list: Error - MFXLoad() returned null - no libraries found\n");
        return -1;
    }

    mfxConfig config          = MFXCreateConfig(loader);
    int i                     = 0;
    mfxImplDescription *idesc = NULL;
    int max_generation        = QSV_G0;
    int default_adapter       = 0;
    mfxVariant var            = {};
    var.Version.Version       = MFX_VARIANT_VERSION;
    mfxStatus err             = MFX_ERR_NONE;

    var.Type     = MFX_VARIANT_TYPE_U32;
    var.Data.U32 = MFX_IMPL_TYPE_HARDWARE;
    err = MFXSetConfigFilterProperty(config, (const mfxU8 *)"mfxImplDescription.Impl", var);
    if (err != MFX_ERR_NONE)
        hb_error("hb_qsv_make_adapters_list: MFXSetConfigFilterProperty mfxImplDescription.Impl error=%d", err);

#if defined(_WIN32) || defined(__MINGW32__)
    var.Type     = MFX_VARIANT_TYPE_U32;
    var.Data.U32 = MFX_ACCEL_MODE_VIA_D3D11;
    err = MFXSetConfigFilterProperty(config, (const mfxU8 *)"mfxImplDescription.AccelerationMode", var);
    if (err != MFX_ERR_NONE)
        hb_error("hb_qsv_make_adapters_list: MFXSetConfigFilterProperty mfxImplDescription.AccelerationMode error=%d", err);
#endif

    var.Type     = MFX_VARIANT_TYPE_U32;
    var.Data.U32 = 0x8086;
    MFXSetConfigFilterProperty(config, (const mfxU8 *)"mfxImplDescription.VendorID", var);
    if (err != MFX_ERR_NONE)
        hb_error("hb_qsv_make_adapters_list: MFXSetConfigFilterProperty mfxImplDescription.VendorID error=%d", err);

    while (1)
    {
        err = MFXEnumImplementations(loader,
                                     i,
                                     MFX_IMPLCAPS_IMPLDESCSTRUCTURE,
                                     (mfxHDL *)(&idesc));
        if (err != MFX_ERR_NONE)
        {
            if (err != MFX_ERR_NOT_FOUND)
            {
                hb_error("hb_qsv_make_adapters_list: MFXEnumImplementations returns %d", err);
            }
            break;
        }

        mfxSession session = NULL;
        err = MFXCreateSession(loader, i, &session);
        if (err == MFX_ERR_NONE)
        {
            hb_qsv_adapter_details_t *adapter_details = av_mallocz(sizeof(hb_qsv_adapter_details_t));
            if (!adapter_details)
            {
                hb_error("hb_qsv_make_adapters_list: adapter_details allocation failed");
                return -1;
            }
            int *adapter_index = av_mallocz(sizeof(int));
            if (!adapter_index)
            {
                hb_error("hb_qsv_make_adapters_list: adapter_index allocation failed");
                return -1;
            }
            init_adapter_details(adapter_details);
            // On Linux VendorImplID number of the device starting from 0
            adapter_details->index = idesc->VendorImplID;
            adapter_details->impl_name = strdup( (const char *)idesc->ImplName);
            *adapter_index = idesc->VendorImplID;
            mfxHDL impl_path = NULL;
            err = MFXEnumImplementations(loader, i, MFX_IMPLCAPS_IMPLPATH, &impl_path);
            if (err == MFX_ERR_NONE)
            {
                if (impl_path)
                {
                    adapter_details->impl_path = strdup( (const char *)impl_path);
                    MFXDispReleaseImplDescription(loader, impl_path);
                }
            }
            else
            {
                hb_error("hb_qsv_make_adapters_list: MFXEnumImplementations MFX_IMPLCAPS_IMPLPATH failed impl=%d err=%d", i, err);
            }
            mfxExtendedDeviceId *idescDevice;
            err = MFXEnumImplementations(loader,
                                         i,
                                         MFX_IMPLCAPS_DEVICE_ID_EXTENDED,
                                         (mfxHDL *)(&idescDevice));
            if (err == MFX_ERR_NONE) {
                adapter_details->extended_device_id = *idescDevice;
                MFXDispReleaseImplDescription(loader, idescDevice);
            }
            hb_list_add(list, (void*)adapter_index);
            hb_list_add(list2, (void*)adapter_details);
            // On Linux, the handle to the VA display must be set.
            // This code is essentially a NOP other platforms.
            hb_display_t *display = hb_qsv_display_init(adapter_details->extended_device_id.DRMRenderNodeNum);
            if (display != NULL)
            {
                MFXVideoCORE_SetHandle(session, display->mfxType,
                                    (mfxHDL)display->handle);
            }
            mfxPlatform platform = { 0 };
            err = MFXVideoCORE_QueryPlatform(session, &platform);
            if (MFX_ERR_NONE == err)
            {
                int generation = hb_qsv_hardware_generation(qsv_map_mfx_platform_codename(platform.CodeName));
                // select default QSV adapter
                if (generation > max_generation)
                {
                    max_generation = generation;
                    default_adapter = idesc->VendorImplID;
                }
                adapter_details->platform = platform;
            }
            else
            {
                hb_error("hb_qsv_make_adapters_list: MFXVideoCORE_QueryPlatform failed impl=%d err=%d", i, err);
            }
            MFXClose(session);
            // display must be closed after MFXClose
            hb_display_close(&display);
        }
        else
        {
            hb_error("hb_qsv_make_adapters_list: MFXCreateSession failed impl=%d err=%d", i, err);
        }
        MFXDispReleaseImplDescription(loader, idesc);
        i++;
    }
    MFXUnload(loader);
    *qsv_adapters_list = list;
    *qsv_adapters_details_list = list2;
    hb_qsv_set_default_adapter_index(default_adapter);
    hb_qsv_set_adapter_index(default_adapter);
    return 0;
}

/**
 * Check the actual availability of QSV implementations on the system
 * and collect GPU adapters capabilities.
 *
 * @returns encoder codec mask supported by QSV implemenation,
 *      0 if QSV is not available, -1 if HB_PROJECT_FEATURE_QSV is not enabled
 */
int hb_qsv_available()
{
    if (hb_is_hardware_disabled())
    {
        return 0;
    }

    if (qsv_init_done != 0)
    {
        // This method gets called a lot. Don't probe hardware each time.
        return qsv_init_result;
    }

    qsv_init_done = 1;
    int result = hb_qsv_info_init();
    if (result != 0)
    {
        hb_log("qsv: not available on this system");
        qsv_init_result = 0;
        return qsv_init_result;
    }
    hb_log("qsv: is available on this system");

    // Return the codec capabilities for the highest platform generation
    qsv_init_result = ((hb_qsv_video_encoder_is_available(HB_VCODEC_FFMPEG_QSV_H264) ? HB_VCODEC_FFMPEG_QSV_H264 : 0) |
                       (hb_qsv_video_encoder_is_available(HB_VCODEC_FFMPEG_QSV_H265) ? HB_VCODEC_FFMPEG_QSV_H265 : 0) |
                       (hb_qsv_video_encoder_is_available(HB_VCODEC_FFMPEG_QSV_H265_10BIT) ? HB_VCODEC_FFMPEG_QSV_H265_10BIT : 0) |
                       (hb_qsv_video_encoder_is_available(HB_VCODEC_FFMPEG_QSV_AV1) ? HB_VCODEC_FFMPEG_QSV_AV1 : 0) |
                       (hb_qsv_video_encoder_is_available(HB_VCODEC_FFMPEG_QSV_AV1_10BIT) ? HB_VCODEC_FFMPEG_QSV_AV1_10BIT : 0));
    return qsv_init_result;
}

int hb_qsv_hyper_encode_available(int adapter_index)
{
    const hb_qsv_adapter_details_t *details = hb_qsv_get_adapters_details_by_index(adapter_index);

    if (details)
    {
        return (details->hb_qsv_info_avc != NULL && details->hb_qsv_info_avc->capabilities & HB_QSV_CAP_HYPERENCODE) ||
            (details->hb_qsv_info_hevc != NULL && details->hb_qsv_info_hevc->capabilities & HB_QSV_CAP_HYPERENCODE) ||
            (details->hb_qsv_info_av1 != NULL && details->hb_qsv_info_av1->capabilities & HB_QSV_CAP_HYPERENCODE);
    }
    else
    {
        return 0;
    }
}


int hb_qsv_is_ffmpeg_supported_codec(int vcodec)
{
    if (vcodec == HB_VCODEC_FFMPEG_QSV_H264 ||
        vcodec == HB_VCODEC_FFMPEG_QSV_H265 ||
        vcodec == HB_VCODEC_FFMPEG_QSV_H265_10BIT ||
        vcodec == HB_VCODEC_FFMPEG_QSV_AV1 ||
        vcodec == HB_VCODEC_FFMPEG_QSV_AV1_10BIT
        )
    {
        return 1;
    }
    return 0;
}

int hb_qsv_video_encoder_is_available(int encoder)
{
    for (int i = 0; i < hb_list_count(g_qsv_adapters_details_list); i++)
    {
        const hb_qsv_adapter_details_t *details = hb_list_item(g_qsv_adapters_details_list, i);
        if (hb_qsv_adapter_video_encoder_is_available(details->index, encoder))
        {
            return 1;
        }
    }
    return 0;
}

int hb_qsv_adapter_video_encoder_is_available(int adapter_index, int encoder)
{
    const hb_qsv_adapter_details_t *details = hb_qsv_get_adapters_details_by_index(adapter_index);

    if (hb_qsv_hardware_generation(hb_qsv_get_platform(adapter_index)) < QSV_G5)
    {
        return 0;
    }

    if (details)
    {
        switch (encoder)
        {
            case HB_VCODEC_FFMPEG_QSV_H264:
                return details->hb_qsv_info_avc != NULL && details->hb_qsv_info_avc->available;
            case HB_VCODEC_FFMPEG_QSV_H265_10BIT:
                if (hb_qsv_hardware_generation(hb_qsv_get_platform(adapter_index)) < QSV_G6)
                    return 0;
            case HB_VCODEC_FFMPEG_QSV_H265:
                return details->hb_qsv_info_hevc != NULL && details->hb_qsv_info_hevc->available;
            case HB_VCODEC_FFMPEG_QSV_AV1_10BIT:
            case HB_VCODEC_FFMPEG_QSV_AV1:
                return details->hb_qsv_info_av1 != NULL && details->hb_qsv_info_av1->available;
            default:
                return 0;
        }
    }
    else
    {
        return 0;
    }
}

static void init_video_param(mfxVideoParam *videoParam)
{
    if (videoParam == NULL)
    {
        return;
    }

    memset(videoParam, 0, sizeof(mfxVideoParam));
    videoParam->mfx.CodecId                 = MFX_CODEC_AVC;
    videoParam->mfx.CodecLevel              = MFX_LEVEL_UNKNOWN;
    videoParam->mfx.CodecProfile            = MFX_PROFILE_UNKNOWN;
    videoParam->mfx.RateControlMethod       = MFX_RATECONTROL_VBR;
    videoParam->mfx.TargetUsage             = MFX_TARGETUSAGE_BALANCED;
    videoParam->mfx.TargetKbps              = 5000;
    videoParam->mfx.GopOptFlag              = MFX_GOP_CLOSED;
    videoParam->mfx.FrameInfo.FourCC        = MFX_FOURCC_NV12;
    videoParam->mfx.FrameInfo.ChromaFormat  = MFX_CHROMAFORMAT_YUV420;
    videoParam->mfx.FrameInfo.PicStruct     = MFX_PICSTRUCT_PROGRESSIVE;
    videoParam->mfx.FrameInfo.FrameRateExtN = 25;
    videoParam->mfx.FrameInfo.FrameRateExtD = 1;
    videoParam->mfx.FrameInfo.Width         = 1920;
    videoParam->mfx.FrameInfo.CropW         = 1920;
    videoParam->mfx.FrameInfo.AspectRatioW  = 1;
    videoParam->mfx.FrameInfo.Height        = 1088;
    videoParam->mfx.FrameInfo.CropH         = 1080;
    videoParam->mfx.FrameInfo.AspectRatioH  = 1;
    videoParam->AsyncDepth                  = HB_QSV_ASYNC_DEPTH_DEFAULT;
    videoParam->IOPattern                   = MFX_IOPATTERN_IN_SYSTEM_MEMORY;
}

static void init_video_hyperencode_param(mfxVideoParam *videoParam, int codec_id)
{
    if (videoParam == NULL)
    {
        return;
    }

    // Both GPUs must support of the same encoder parameters
    if (codec_id == MFX_CODEC_HEVC)
    {
        videoParam->mfx.IdrInterval = 1;
    }
    else if (codec_id == MFX_CODEC_AVC)
    {
        videoParam->mfx.IdrInterval = 0;
        // Relax ARC Gfx encoding settings to align ADL Gfx capabilities
        videoParam->mfx.GopRefDist = 1;
    }
    videoParam->mfx.GopPicSize = 60;
    videoParam->AsyncDepth = 60;
}

static void init_ext_video_signal_info(mfxExtVideoSignalInfo *extVideoSignalInfo)
{
    if (extVideoSignalInfo == NULL)
    {
        return;
    }

    memset(extVideoSignalInfo, 0, sizeof(mfxExtVideoSignalInfo));
    extVideoSignalInfo->Header.BufferId          = MFX_EXTBUFF_VIDEO_SIGNAL_INFO;
    extVideoSignalInfo->Header.BufferSz          = sizeof(mfxExtVideoSignalInfo);
    extVideoSignalInfo->VideoFormat              = 5; // undefined
    extVideoSignalInfo->VideoFullRange           = 0; // TV range
    extVideoSignalInfo->ColourDescriptionPresent = 0; // don't write to bitstream
    extVideoSignalInfo->ColourPrimaries          = 2; // undefined
    extVideoSignalInfo->TransferCharacteristics  = 2; // undefined
    extVideoSignalInfo->MatrixCoefficients       = 2; // undefined
}

static void init_ext_chroma_loc_info(mfxExtChromaLocInfo *extChromaLocInfo)
{
    if (extChromaLocInfo == NULL)
    {
        return;
    }

    memset(extChromaLocInfo, 0, sizeof(mfxExtChromaLocInfo));
    extChromaLocInfo->Header.BufferId = MFX_EXTBUFF_CHROMA_LOC_INFO;
    extChromaLocInfo->Header.BufferSz = sizeof(mfxExtChromaLocInfo);
}

static void init_ext_mastering_display_colour_volume(mfxExtMasteringDisplayColourVolume *extMasteringDisplayColourVolume)
{
    if (extMasteringDisplayColourVolume == NULL)
    {
        return;
    }

    memset(extMasteringDisplayColourVolume, 0, sizeof(mfxExtMasteringDisplayColourVolume));
    extMasteringDisplayColourVolume->Header.BufferId     = MFX_EXTBUFF_MASTERING_DISPLAY_COLOUR_VOLUME;
    extMasteringDisplayColourVolume->Header.BufferSz     = sizeof(mfxExtMasteringDisplayColourVolume);
    extMasteringDisplayColourVolume->InsertPayloadToggle = MFX_PAYLOAD_OFF;
}

static void init_ext_content_light_level_info(mfxExtContentLightLevelInfo *extContentLightLevelInfo)
{
    if (extContentLightLevelInfo == NULL)
    {
        return;
    }

    memset(extContentLightLevelInfo, 0, sizeof(mfxExtContentLightLevelInfo));
    extContentLightLevelInfo->Header.BufferId     = MFX_EXTBUFF_CONTENT_LIGHT_LEVEL_INFO;
    extContentLightLevelInfo->Header.BufferSz     = sizeof(mfxExtContentLightLevelInfo);
    extContentLightLevelInfo->InsertPayloadToggle = MFX_PAYLOAD_OFF;
}

static void init_ext_hyperencode_option(mfxExtHyperModeParam *extHyperEncodemParam)
{
    if (extHyperEncodemParam == NULL)
    {
        return;
    }

    memset(extHyperEncodemParam, 0, sizeof(mfxExtHyperModeParam));
    extHyperEncodemParam->Header.BufferId = MFX_EXTBUFF_HYPER_MODE_PARAM;
    extHyperEncodemParam->Header.BufferSz = sizeof(mfxExtHyperModeParam);
    extHyperEncodemParam->Mode = MFX_HYPERMODE_OFF;
}

static void init_ext_coding_option(mfxExtCodingOption *extCodingOption)
{
    if (extCodingOption == NULL)
    {
        return;
    }

    memset(extCodingOption, 0, sizeof(mfxExtCodingOption));
    extCodingOption->Header.BufferId = MFX_EXTBUFF_CODING_OPTION;
    extCodingOption->Header.BufferSz = sizeof(mfxExtCodingOption);
    extCodingOption->AUDelimiter     = MFX_CODINGOPTION_OFF;
    extCodingOption->PicTimingSEI    = MFX_CODINGOPTION_OFF;
    extCodingOption->CAVLC           = MFX_CODINGOPTION_OFF;
}

static void init_ext_coding_option2(mfxExtCodingOption2 *extCodingOption2)
{
    if (extCodingOption2 == NULL)
    {
        return;
    }

    memset(extCodingOption2, 0, sizeof(mfxExtCodingOption2));
    extCodingOption2->Header.BufferId = MFX_EXTBUFF_CODING_OPTION2;
    extCodingOption2->Header.BufferSz = sizeof(mfxExtCodingOption2);
    extCodingOption2->MBBRC           = MFX_CODINGOPTION_ON;
    extCodingOption2->ExtBRC          = MFX_CODINGOPTION_ON;
    extCodingOption2->Trellis         = MFX_TRELLIS_I|MFX_TRELLIS_P|MFX_TRELLIS_B;
    extCodingOption2->RepeatPPS       = MFX_CODINGOPTION_ON;
    extCodingOption2->BRefType        = MFX_B_REF_PYRAMID;
    extCodingOption2->AdaptiveI       = MFX_CODINGOPTION_ON;
    extCodingOption2->AdaptiveB       = MFX_CODINGOPTION_ON;
    extCodingOption2->LookAheadDS     = MFX_LOOKAHEAD_DS_4x;
    extCodingOption2->NumMbPerSlice   = 2040; // 1920x1088/4
}

static void init_ext_av1bitstream_option(mfxExtAV1BitstreamParam *extAV1BitstreamParam)
{
    if (extAV1BitstreamParam == NULL)
    {
        return;
    }

    memset(extAV1BitstreamParam, 0, sizeof(mfxExtAV1BitstreamParam));
    extAV1BitstreamParam->Header.BufferId = MFX_EXTBUFF_AV1_BITSTREAM_PARAM;
    extAV1BitstreamParam->Header.BufferSz = sizeof(mfxExtAV1BitstreamParam);
    extAV1BitstreamParam->WriteIVFHeaders = MFX_CODINGOPTION_OFF;
}

static void init_ext_av1screencontent_tools(mfxExtAV1ScreenContentTools *extScreenContentTools)
{
    if (extScreenContentTools == NULL)
    {
        return;
    }

    memset(extScreenContentTools, 0, sizeof(mfxExtAV1ScreenContentTools));
    extScreenContentTools->Header.BufferId = MFX_EXTBUFF_AV1_SCREEN_CONTENT_TOOLS;
    extScreenContentTools->Header.BufferSz = sizeof(mfxExtAV1ScreenContentTools);
    extScreenContentTools->IntraBlockCopy  = MFX_CODINGOPTION_OFF;
    extScreenContentTools->Palette         = MFX_CODINGOPTION_OFF;
}

static int query_capabilities(mfxSession session, int index, mfxVersion version, hb_qsv_info_t *info, int lowpower)
{
    /*
     * MFXVideoENCODE_Query(mfxSession, mfxVideoParam *in, mfxVideoParam *out);
     *
     * Mode 1:
     * - in is NULL
     * - out has the parameters we want to query set to 1
     * - out->mfx.CodecId field has to be set (mandatory)
     * - MFXVideoENCODE_Query should zero out all unsupported parameters
     *
     * Mode 2:
     * - the parameters we want to query are set for in
     * - in ->mfx.CodecId field has to be set (mandatory)
     * - out->mfx.CodecId field has to be set (mandatory)
     * - MFXVideoENCODE_Query should sanitize all unsupported parameters
     */
    mfxStatus     status;
    mfxExtBuffer *videoExtParam[1];
    mfxVideoParam videoParam, inputParam;
    mfxExtCodingOption    extCodingOption;
    mfxExtCodingOption2   extCodingOption2;
    mfxExtVideoSignalInfo extVideoSignalInfo;
    mfxExtChromaLocInfo   extChromaLocInfo;
    mfxExtMasteringDisplayColourVolume  extMasteringDisplayColourVolume;
    mfxExtContentLightLevelInfo         extContentLightLevelInfo;
    mfxExtAV1BitstreamParam extAV1BitstreamParam;
    mfxExtAV1ScreenContentTools extAV1ScreenContentToolsParam;
    mfxExtHyperModeParam extHyperEncodeParam;

    /* Reset capabilities before querying */
    info->capabilities = 0;

    /* Disable lowpower if the encoder is software */
    if (hb_qsv_implementation_is_hardware(info->implementation) == 0)
    {
         lowpower = 0;
    }

    /*
     * First of all, check availability of an encoder for
     * this combination of a codec ID and implementation.
     *
     * Note: can error out rather than sanitizing
     * unsupported codec IDs, so don't log errors.
     */
    if (HB_CHECK_MFX_VERSION(version, HB_QSV_MINVERSION_MAJOR, HB_QSV_MINVERSION_MINOR))
    {
        {
            mfxStatus mfxRes;
            init_video_param(&inputParam);
            inputParam.mfx.CodecId = info->codec_id;
            inputParam.mfx.LowPower = lowpower;
            memset(&videoParam, 0, sizeof(mfxVideoParam));
            videoParam.mfx.CodecId = inputParam.mfx.CodecId;

            mfxRes = MFXVideoENCODE_Query(session, &inputParam, &videoParam);
            if (mfxRes >= MFX_ERR_NONE &&
                videoParam.mfx.CodecId == info->codec_id)
            {
                /*
                 * MFXVideoENCODE_Query might tell you that an HEVC encoder is
                 * available on Haswell hardware, but it'll fail to initialize.
                 * So check encoder availability with MFXVideoENCODE_Init too.
                 */
                if ((status = MFXVideoENCODE_Init(session, &videoParam)) >= MFX_ERR_NONE)
                {
                    info->available = 1;
                }
                else if (info->codec_id == MFX_CODEC_AVC)
                {
                    /*
                     * This should not fail for AVC encoders, so we want to know
                     * about it - however, it may fail for other encoders (ignore)
                     */
                    fprintf(stderr,
                            "query_capabilities: MFXVideoENCODE_Init failed"
                            " (0x%"PRIX32", 0x%"PRIX32", %d)\n",
                            info->codec_id, info->implementation, status);
                }
                MFXVideoENCODE_Close(session);
            }
        }
    }
    if (!info->available)
    {
        /* Don't check capabilities for unavailable encoders */
        return 0;
    }

    {
        /* Implementation-specific features that can't be queried */
        if (info->codec_id == MFX_CODEC_AVC || info->codec_id == MFX_CODEC_HEVC || info->codec_id == MFX_CODEC_AV1)
        {
            if (hb_qsv_implementation_is_hardware(info->implementation))
            {
                if (hb_qsv_hardware_generation(hb_qsv_get_platform(index)) >= QSV_G3)
                {
                    info->capabilities |= HB_QSV_CAP_B_REF_PYRAMID;
                }
                if (info->codec_id == MFX_CODEC_AVC &&
                    (hb_qsv_hardware_generation(hb_qsv_get_platform(index)) >= QSV_G7))
                {
                    info->capabilities |= HB_QSV_CAP_LOWPOWER_ENCODE;
                }
                if (info->codec_id == MFX_CODEC_HEVC &&
                    (hb_qsv_hardware_generation(hb_qsv_get_platform(index)) >= QSV_G7))
                {
                    info->capabilities |= HB_QSV_CAP_LOWPOWER_ENCODE;
                }
                if (info->codec_id == MFX_CODEC_AV1 &&
                    (hb_qsv_hardware_generation(hb_qsv_get_platform(index)) > QSV_G8))
                {
                    info->capabilities |= HB_QSV_CAP_LOWPOWER_ENCODE;
                }
            }
            else
            {
                if (HB_CHECK_MFX_VERSION(version, 1, 6))
                {
                    info->capabilities |= HB_QSV_CAP_B_REF_PYRAMID;
                }
            }
        }

        /* API-specific features that can't be queried */
        if (HB_CHECK_MFX_VERSION(version, 1, 6))
        {
            // API >= 1.6 (mfxBitstream::DecodeTimeStamp, mfxExtCodingOption2)
            info->capabilities |= HB_QSV_CAP_MSDK_API_1_6;
        }

        /*
         * Check availability of optional rate control methods.
         *
         * Mode 2 tends to error out, but mode 1 gives false negatives, which
         * is worse. So use mode 2 and assume an error means it's unsupported.
         *
         * Also assume that LA and ICQ combined imply LA_ICQ is
         * supported, so we don't need to check the latter too.
         */
        if (HB_CHECK_MFX_VERSION(version, 1, 7))
        {
            init_video_param(&inputParam);
            inputParam.mfx.CodecId           = info->codec_id;
            inputParam.mfx.LowPower          = lowpower;
            inputParam.mfx.RateControlMethod = MFX_RATECONTROL_LA;
            inputParam.mfx.TargetKbps        = 5000;

            memset(&videoParam, 0, sizeof(mfxVideoParam));
            videoParam.mfx.CodecId = inputParam.mfx.CodecId;

            if (MFXVideoENCODE_Query(session, &inputParam, &videoParam) >= MFX_ERR_NONE &&
                videoParam.mfx.RateControlMethod == MFX_RATECONTROL_LA)
            {
                info->capabilities |= HB_QSV_CAP_RATECONTROL_LA;

                // also check for LA + interlaced support
                init_video_param(&inputParam);
                inputParam.mfx.CodecId             = info->codec_id;
                inputParam.mfx.LowPower            = lowpower;
                inputParam.mfx.RateControlMethod   = MFX_RATECONTROL_LA;
                inputParam.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_FIELD_TFF;
                inputParam.mfx.TargetKbps          = 5000;

                memset(&videoParam, 0, sizeof(mfxVideoParam));
                videoParam.mfx.CodecId = inputParam.mfx.CodecId;

                if (MFXVideoENCODE_Query(session, &inputParam, &videoParam) >= MFX_ERR_NONE &&
                    videoParam.mfx.FrameInfo.PicStruct == MFX_PICSTRUCT_FIELD_TFF           &&
                    videoParam.mfx.RateControlMethod   == MFX_RATECONTROL_LA)
                {
                    info->capabilities |= HB_QSV_CAP_RATECONTROL_LAi;
                }
            }
        }
        if (HB_CHECK_MFX_VERSION(version, 1, 8))
        {
            init_video_param(&inputParam);
            inputParam.mfx.CodecId           = info->codec_id;
            inputParam.mfx.LowPower          = lowpower;
            inputParam.mfx.RateControlMethod = MFX_RATECONTROL_ICQ;
            inputParam.mfx.ICQQuality        = 20;

            memset(&videoParam, 0, sizeof(mfxVideoParam));
            videoParam.mfx.CodecId = inputParam.mfx.CodecId;

            if (MFXVideoENCODE_Query(session, &inputParam, &videoParam) >= MFX_ERR_NONE &&
                videoParam.mfx.RateControlMethod == MFX_RATECONTROL_ICQ)
            {
                info->capabilities |= HB_QSV_CAP_RATECONTROL_ICQ;
            }
        }

        /*
         * Determine whether mfxExtVideoSignalInfo is supported.
         */
        if (HB_CHECK_MFX_VERSION(version, 1, 3))
        {
            init_video_param(&videoParam);
            videoParam.mfx.CodecId = info->codec_id;
            videoParam.mfx.LowPower = lowpower;
            init_ext_video_signal_info(&extVideoSignalInfo);
            videoParam.ExtParam    = videoExtParam;
            videoParam.ExtParam[0] = (mfxExtBuffer*)&extVideoSignalInfo;
            videoParam.NumExtParam = 1;

            status = MFXVideoENCODE_Query(session, NULL, &videoParam);
            if (status >= MFX_ERR_NONE)
            {
                /* Encoder can be configured via mfxExtVideoSignalInfo */
                info->capabilities |= HB_QSV_CAP_VUI_VSINFO;
            }
            else if (info->codec_id == MFX_CODEC_AVC)
            {
                /*
                 * This should not fail for AVC encoders, so we want to know
                 * about it - however, it may fail for other encoders (ignore)
                 */
                fprintf(stderr,
                        "query_capabilities: mfxExtVideoSignalInfo check"
                        " failed (0x%"PRIX32", 0x%"PRIX32", %d)\n",
                        info->codec_id, info->implementation, status);
            }
        }

        /*
         * Determine whether mfxExtCodingOption is supported.
         */
        if (HB_CHECK_MFX_VERSION(version, 1, 0))
        {
            init_video_param(&videoParam);
            videoParam.mfx.CodecId = info->codec_id;
            videoParam.mfx.LowPower = lowpower;
            init_ext_coding_option(&extCodingOption);
            videoParam.ExtParam    = videoExtParam;
            videoParam.ExtParam[0] = (mfxExtBuffer*)&extCodingOption;
            videoParam.NumExtParam = 1;

            status = MFXVideoENCODE_Query(session, NULL, &videoParam);
            if (status >= MFX_ERR_NONE)
            {
                /* Encoder can be configured via mfxExtCodingOption */
                info->capabilities |= HB_QSV_CAP_OPTION1;
            }
            else if (info->codec_id == MFX_CODEC_AVC)
            {
                /*
                 * This should not fail for AVC encoders, so we want to know
                 * about it - however, it may fail for other encoders (ignore)
                 */
                fprintf(stderr,
                        "query_capabilities: mfxExtCodingOption check"
                        " failed (0x%"PRIX32", 0x%"PRIX32", %d)\n",
                        info->codec_id, info->implementation, status);
            }
        }

        /*
         * Determine whether mfxExtCodingOption2 and its fields are supported.
         *
         * Mode 2 suffers from false negatives with some drivers, whereas mode 1
         * suffers from false positives instead. The latter is probably easier
         * and/or safer to sanitize for us, so use mode 1.
         */
        if (HB_CHECK_MFX_VERSION(version, 1, 6))
        {
            init_video_param(&videoParam);
            videoParam.mfx.CodecId = info->codec_id;
            videoParam.mfx.LowPower = lowpower;
            init_ext_coding_option2(&extCodingOption2);
            videoParam.ExtParam    = videoExtParam;
            videoParam.ExtParam[0] = (mfxExtBuffer*)&extCodingOption2;
            videoParam.NumExtParam = 1;

            status = MFXVideoENCODE_Query(session, NULL, &videoParam);
            if (status >= MFX_ERR_NONE)
            {
#if 0
                // testing code that could come in handy
                fprintf(stderr, "-------------------\n");
                fprintf(stderr, "MBBRC:         0x%02X\n",     extCodingOption2.MBBRC);
                fprintf(stderr, "ExtBRC:        0x%02X\n",     extCodingOption2.ExtBRC);
                fprintf(stderr, "Trellis:       0x%02X\n",     extCodingOption2.Trellis);
                fprintf(stderr, "RepeatPPS:     0x%02X\n",     extCodingOption2.RepeatPPS);
                fprintf(stderr, "BRefType:      %4"PRIu16"\n", extCodingOption2.BRefType);
                fprintf(stderr, "AdaptiveI:     0x%02X\n",     extCodingOption2.AdaptiveI);
                fprintf(stderr, "AdaptiveB:     0x%02X\n",     extCodingOption2.AdaptiveB);
                fprintf(stderr, "LookAheadDS:   %4"PRIu16"\n", extCodingOption2.LookAheadDS);
                fprintf(stderr, "-------------------\n");
#endif

                /* Encoder can be configured via mfxExtCodingOption2 */
                info->capabilities |= HB_QSV_CAP_OPTION2;

                /*
                 * Sanitize API 1.6 fields:
                 *
                 * - MBBRC  requires G3 hardware (Haswell or equivalent)
                 * - ExtBRC requires G2 hardware (Ivy Bridge or equivalent)
                 */
                if (hb_qsv_implementation_is_hardware(info->implementation) &&
                    hb_qsv_hardware_generation(hb_qsv_get_platform(index)) >= QSV_G3)
                {
                    if (extCodingOption2.MBBRC)
                    {
                        info->capabilities |= HB_QSV_CAP_OPTION2_MBBRC;
                    }
                }
                if (hb_qsv_implementation_is_hardware(info->implementation) &&
                    hb_qsv_hardware_generation(hb_qsv_get_platform(index)) >= QSV_G2)
                {
                    if (extCodingOption2.ExtBRC)
                    {
                        info->capabilities |= HB_QSV_CAP_OPTION2_EXTBRC;
                    }
                }

                /*
                 * Sanitize API 1.7 fields:
                 *
                 * - Trellis requires G3 hardware (Haswell or equivalent)
                 */
                if (HB_CHECK_MFX_VERSION(version, 1, 7))
                {
                    if (hb_qsv_implementation_is_hardware(info->implementation) &&
                        hb_qsv_hardware_generation(hb_qsv_get_platform(index)) >= QSV_G3)
                    {
                        if (extCodingOption2.Trellis)
                        {
                            info->capabilities |= HB_QSV_CAP_OPTION2_TRELLIS;
                        }
                    }
                }

                /*
                 * Sanitize API 1.8 fields:
                 *
                 * - BRefType    requires B-pyramid support
                 * - LookAheadDS requires lookahead support
                 * - AdaptiveI, AdaptiveB, NumMbPerSlice unknown (trust Query)
                 */
                if (HB_CHECK_MFX_VERSION(version, 1, 8))
                {
                    if (extCodingOption2.RepeatPPS)
                    {
                        info->capabilities |= HB_QSV_CAP_OPTION2_REPEATPPS;
                    }
                    if (info->capabilities & HB_QSV_CAP_B_REF_PYRAMID)
                    {
                        if (extCodingOption2.BRefType)
                        {
                            info->capabilities |= HB_QSV_CAP_OPTION2_BREFTYPE;
                        }
                    }
                    if (info->capabilities & HB_QSV_CAP_RATECONTROL_LA)
                    {
                        if (extCodingOption2.LookAheadDS)
                        {
                            info->capabilities |= HB_QSV_CAP_OPTION2_LA_DOWNS;
                        }
                    }
                    if (extCodingOption2.AdaptiveI && extCodingOption2.AdaptiveB)
                    {
                        info->capabilities |= HB_QSV_CAP_OPTION2_IB_ADAPT;
                    }
                    if (extCodingOption2.NumMbPerSlice)
                    {
                        info->capabilities |= HB_QSV_CAP_OPTION2_NMPSLICE;
                    }
                }
            }
            else
            {
                fprintf(stderr,
                        "query_capabilities: mfxExtCodingOption2 check failed (0x%"PRIX32", 0x%"PRIX32", %d)\n",
                        info->codec_id, info->implementation, status);
            }
        }
        if (HB_CHECK_MFX_VERSION(version, 1, 13) && info->codec_id == MFX_CODEC_AVC)
        {
            init_video_param(&videoParam);
            videoParam.mfx.CodecId = info->codec_id;
            videoParam.mfx.LowPower = lowpower;
            init_ext_chroma_loc_info(&extChromaLocInfo);
            videoParam.ExtParam    = videoExtParam;
            videoParam.ExtParam[0] = (mfxExtBuffer*)&extChromaLocInfo;
            videoParam.NumExtParam = 1;

            status = MFXVideoENCODE_Query(session, NULL, &videoParam);
            if (status >= MFX_ERR_NONE)
            {
                /* Encoder can be configured via mfxExtChromaLocInfo */
                info->capabilities |= HB_QSV_CAP_VUI_CHROMALOCINFO;
            }
        }

        if (HB_CHECK_MFX_VERSION(version, 1, 19))
        {
            if (hb_qsv_hardware_generation(hb_qsv_get_platform(index)) >= QSV_G7)
            {
                info->capabilities |= HB_QSV_CAP_VPP_SCALING;
            }
        }
        if (HB_CHECK_MFX_VERSION(version, 1, 25) && (info->codec_id == MFX_CODEC_HEVC || info->codec_id == MFX_CODEC_AV1))
        {
            init_video_param(&videoParam);
            videoParam.mfx.CodecId = info->codec_id;
            videoParam.mfx.LowPower = lowpower;
            init_ext_mastering_display_colour_volume(&extMasteringDisplayColourVolume);
            videoParam.ExtParam    = videoExtParam;
            videoParam.ExtParam[0] = (mfxExtBuffer*)&extMasteringDisplayColourVolume;
            videoParam.NumExtParam = 1;

            status = MFXVideoENCODE_Query(session, NULL, &videoParam);
            if (status >= MFX_ERR_NONE)
            {
                /* Encoder can be configured via mfxExtMasteringDisplayColourVolume */
                info->capabilities |= HB_QSV_CAP_VUI_MASTERINGINFO;
            }

            init_video_param(&videoParam);
            videoParam.mfx.CodecId = info->codec_id;
            videoParam.mfx.LowPower = lowpower;
            init_ext_content_light_level_info(&extContentLightLevelInfo);
            videoParam.ExtParam    = videoExtParam;
            videoParam.ExtParam[0] = (mfxExtBuffer*)&extContentLightLevelInfo;
            videoParam.NumExtParam = 1;

            status = MFXVideoENCODE_Query(session, NULL, &videoParam);
            if (status >= MFX_ERR_NONE)
            {
                /* Encoder can be configured via mfxExtContentLightLevelInfo */
                info->capabilities |= HB_QSV_CAP_VUI_CLLINFO;
            }
        }
        if (HB_CHECK_MFX_VERSION(version, 1, 33))
        {
            if (hb_qsv_hardware_generation(hb_qsv_get_platform(index)) >= QSV_G7)
            {
                info->capabilities |= HB_QSV_CAP_VPP_INTERPOLATION;
            }
        }
        if (lowpower == MFX_CODINGOPTION_ON)
        {
            init_video_param(&videoParam);
            videoParam.mfx.CodecId = info->codec_id;
            init_video_hyperencode_param(&videoParam, info->codec_id);
            videoParam.mfx.LowPower = lowpower;

            init_ext_hyperencode_option(&extHyperEncodeParam);
            videoParam.ExtParam    = videoExtParam;
            videoParam.ExtParam[0] = (mfxExtBuffer*)&extHyperEncodeParam;
            videoParam.NumExtParam = 1;

            status = MFXVideoENCODE_Query(session, NULL, &videoParam);
            if (status >= MFX_ERR_NONE && extHyperEncodeParam.Mode == MFX_HYPERMODE_ON)
            {
                info->capabilities |= HB_QSV_CAP_HYPERENCODE;
            }
        }
        if ((lowpower == MFX_CODINGOPTION_ON) && (info->codec_id == MFX_CODEC_AV1))
        {
            init_video_param(&videoParam);
            videoParam.mfx.CodecId = info->codec_id;
            videoParam.mfx.LowPower = lowpower;

            init_ext_av1bitstream_option(&extAV1BitstreamParam);
            videoParam.ExtParam    = videoExtParam;
            videoParam.ExtParam[0] = (mfxExtBuffer*)&extAV1BitstreamParam;
            videoParam.NumExtParam = 1;

            status = MFXVideoENCODE_Query(session, NULL, &videoParam);
            if (status >= MFX_ERR_NONE)
            {
                info->capabilities |= HB_QSV_CAP_AV1_BITSTREAM;
            }
        }
        if ((lowpower == MFX_CODINGOPTION_ON) && (info->codec_id == MFX_CODEC_AV1))
        {
            init_video_param(&videoParam);
            videoParam.mfx.CodecId = info->codec_id;
            videoParam.mfx.LowPower = lowpower;
            init_ext_av1screencontent_tools(&extAV1ScreenContentToolsParam);

            videoParam.ExtParam    = videoExtParam;
            videoParam.ExtParam[0] = (mfxExtBuffer*)&extAV1ScreenContentToolsParam;
            videoParam.NumExtParam = 1;

            status = MFXVideoENCODE_Query(session, NULL, &videoParam);
            if (status >= MFX_ERR_NONE && extAV1ScreenContentToolsParam.IntraBlockCopy != 0)
            {
                info->capabilities |= HB_QSV_CAP_AV1_SCREENCONTENT;
            }
        }
    }

    return 0;
}

const char * DRM_INTEL_DRIVER_NAME = "i915";
const char * VA_INTEL_DRIVER_NAMES[] = { "iHD", "i965", NULL};

hb_display_t * hb_qsv_display_init(const uint32_t dri_render_node)
{
    return hb_display_init(DRM_INTEL_DRIVER_NAME, dri_render_node, VA_INTEL_DRIVER_NAMES);
}

#if defined(_WIN32) || defined(__MINGW32__)
mfxIMPL hb_qsv_dx_index_to_impl(int dx_index)
{
    mfxIMPL impl;

    switch (dx_index)
    {
        {
        case 0:
            impl = MFX_IMPL_HARDWARE;
            break;
        case 1:
            impl = MFX_IMPL_HARDWARE2;
            break;
        case 2:
            impl = MFX_IMPL_HARDWARE3;
            break;
        case 3:
            impl = MFX_IMPL_HARDWARE4;
            break;

        default:
            // try searching on all display adapters
            impl = MFX_IMPL_HARDWARE_ANY;
            break;
        }
    }
    return impl;
}
#endif

// Adopted implementation of qsv_create_mfx_session() function for HandBrake
int hb_qsv_create_mfx_session(mfxIMPL implementation,
                              int drmRenderNodeNum,
                              mfxVersion *pver,
                              mfxSession *psession)
{
    mfxStatus sts;
    mfxLoader loader = NULL;
    mfxSession session = NULL;
    mfxConfig cfg;
    mfxVersion ver;
    mfxVariant impl_value;
    uint32_t adapter_idx = 0;
    uint32_t impl_idx = 0;

    // Get adapter from MediaSDK implementation value
    adapter_idx = hb_qsv_impl_get_num(implementation);

    *psession = NULL;
    loader = MFXLoad();

    if (!loader) {
        hb_error("hb_qsv_create_mfx_session: Error creating a MFX loader");
        goto fail;
    }

    /* Create configurations for implementation */
    cfg = MFXCreateConfig(loader);

    if (!cfg) {
        hb_error("hb_qsv_create_mfx_session: Error creating a MFX configuration");
        goto fail;
    }

    impl_value.Type = MFX_VARIANT_TYPE_U32;
    impl_value.Data.U32 = (implementation == MFX_IMPL_SOFTWARE) ?
        MFX_IMPL_TYPE_SOFTWARE : MFX_IMPL_TYPE_HARDWARE;
    sts = MFXSetConfigFilterProperty(cfg,
                                     (const mfxU8 *)"mfxImplDescription.Impl", impl_value);

    if (sts != MFX_ERR_NONE) {
        hb_error("hb_qsv_create_mfx_session: Error adding a MFX configuration "
               "property: %d.", sts);
        goto fail;
    }

    if (MFX_IMPL_VIA_D3D11 == MFX_IMPL_VIA_MASK(implementation))
    {
        impl_value.Type = MFX_VARIANT_TYPE_U32;
        impl_value.Data.U32 = MFX_ACCEL_MODE_VIA_D3D11;
        sts = MFXSetConfigFilterProperty(cfg,
                                        (const mfxU8 *)"mfxImplDescription.AccelerationMode", impl_value);

        if (sts != MFX_ERR_NONE) {
            hb_error("hb_qsv_create_mfx_session: Error adding a MFX configuration"
                "MFX_ACCEL_MODE_VIA_D3D11 property: %d.", sts);
            goto fail;
        }

        if (adapter_idx != -1) {
            impl_value.Type = MFX_VARIANT_TYPE_U32;
            impl_value.Data.U32 = adapter_idx;

            sts = MFXSetConfigFilterProperty(cfg,
                                            (const mfxU8 *)"mfxImplDescription.VendorImplID", impl_value);

            if (sts != MFX_ERR_NONE) {
                hb_error("hb_qsv_create_mfx_session: Error adding a MFX configuration"
                    "VendorImplID property: %d.", sts);
                goto fail;
            }
        }
    }
    else
    {
        impl_value.Type = MFX_VARIANT_TYPE_U32;
        impl_value.Data.U32 = drmRenderNodeNum;
        sts = MFXSetConfigFilterProperty(cfg,
                                        (const mfxU8 *)"mfxExtendedDeviceId.DRMRenderNodeNum", impl_value);

        if (sts != MFX_ERR_NONE) {
            hb_error("hb_qsv_create_mfx_session: Error adding a MFX configuration DRMRenderNodeNum property: %d.", sts);
            goto fail;
        }
    }

    impl_value.Type = MFX_VARIANT_TYPE_U32;
    impl_value.Data.U32 = 0x8086;
    sts = MFXSetConfigFilterProperty(cfg, (const mfxU8 *)"mfxImplDescription.VendorID", impl_value);
    if (sts != MFX_ERR_NONE) {
        hb_error("hb_qsv_create_mfx_session: MFXSetConfigFilterProperty mfxImplDescription.VendorID error=%d", sts);
        goto fail;
    }

    impl_value.Type = MFX_VARIANT_TYPE_U32;
    impl_value.Data.U32 = pver->Version;
    sts = MFXSetConfigFilterProperty(cfg,
                                     (const mfxU8 *)"mfxImplDescription.ApiVersion.Version",
                                     impl_value);

    if (sts != MFX_ERR_NONE) {
        hb_error("hb_qsv_create_mfx_session: Error adding a MFX configuration "
               "property: %d.", sts);
        goto fail;
    }

    while (1) {
        /* Enumerate all implementations */
        mfxImplDescription *impl_desc;

        sts = MFXEnumImplementations(loader, impl_idx,
                                     MFX_IMPLCAPS_IMPLDESCSTRUCTURE,
                                     (mfxHDL *)&impl_desc);

        /* Failed to find an available implementation */
        if (sts == MFX_ERR_NOT_FOUND)
            break;
        else if (sts != MFX_ERR_NONE) {
            impl_idx++;
            continue;
        }

        sts = MFXCreateSession(loader, impl_idx, &session);
        MFXDispReleaseImplDescription(loader, impl_desc);

        if (sts == MFX_ERR_NONE)
            break;

        impl_idx++;
    }

    if (sts != MFX_ERR_NONE) {
        hb_error("hb_qsv_create_mfx_session: Error creating a MFX session: %d.", sts);
        goto fail;
    }

    sts = MFXQueryVersion(session, &ver);

    if (sts != MFX_ERR_NONE) {
        hb_error("hb_qsv_create_mfx_session: Error querying a MFX session: %d.", sts);
        goto fail;
    }

    *psession = session;
    MFXUnload(loader);

    return 0;

fail:
    if (session)
        MFXClose(session);

    MFXUnload(loader);

    return AVERROR_UNKNOWN;
}

static int hb_qsv_collect_adapters_details(hb_list_t *hb_qsv_adapter_details_list)
{
    for (int i = 0; i < hb_list_count(hb_qsv_adapter_details_list); i++)
    {
        hb_qsv_adapter_details_t *details = hb_list_item(hb_qsv_adapter_details_list, i);
        /*
        * First, check for any MSDK version to determine whether one or
        * more implementations are present; then check if we can use them.
        *
        * I've had issues using a NULL version with some combinations of
        * hardware and driver, so use a low version number (1.0) instead.
        */
        mfxSession session;
        mfxVersion version = { .Major = 1, .Minor = 0, };

        // check for software fallback
        if (MFXInit(MFX_IMPL_SOFTWARE, &version, &session) == MFX_ERR_NONE)
        {
            // Media SDK software found, but check that our minimum is supported
            MFXQueryVersion(session, &details->qsv_software_version);
            if (HB_CHECK_MFX_VERSION(details->qsv_software_version,
                                    HB_QSV_MINVERSION_MAJOR,
                                    HB_QSV_MINVERSION_MINOR))
            {
                query_capabilities(session, details->index, details->qsv_software_version, &details->qsv_software_info_avc, MFX_CODINGOPTION_OFF);
                query_capabilities(session, details->index, details->qsv_software_version, &details->qsv_software_info_hevc, MFX_CODINGOPTION_OFF);
                // now that we know which hardware encoders are
                // available, we can set the preferred implementation
                qsv_impl_set_preferred(details, "software");
            }
            MFXClose(session);
        }
        // check for actual hardware support
#if defined(_WIN32) || defined(__MINGW32__)
        mfxIMPL hw_preference = MFX_IMPL_VIA_D3D11;
#else
        mfxIMPL hw_preference = MFX_IMPL_VIA_ANY;
#endif
        do{
#if defined(_WIN32) || defined(__MINGW32__)
            mfxIMPL hw_impl = hb_qsv_dx_index_to_impl(details->index);
#else
            mfxIMPL hw_impl = MFX_IMPL_HARDWARE_ANY;
#endif
            if (hb_qsv_create_mfx_session(hw_impl | hw_preference, details->extended_device_id.DRMRenderNodeNum, &version, &session) == MFX_ERR_NONE)
            {
                // On linux, the handle to the VA display must be set.
                // This code is essentially a NOP other platforms.
                hb_display_t * display = hb_qsv_display_init(details->extended_device_id.DRMRenderNodeNum);
                if (display != NULL)
                {
                    MFXVideoCORE_SetHandle(session, display->mfxType,
                                        (mfxHDL)display->handle);
                }
                // Media SDK hardware found, but check that our minimum is supported
                //
                // Note: this-party hardware (QSV_G0) is unsupported for the time being
                MFXQueryVersion(session, &details->qsv_hardware_version);
                if (hb_qsv_hardware_generation(hb_qsv_get_platform(details->index)) >= QSV_G1 &&
                    HB_CHECK_MFX_VERSION(details->qsv_hardware_version,
                                        HB_QSV_MINVERSION_MAJOR,
                                        HB_QSV_MINVERSION_MINOR))
                {
                    if (hb_qsv_hardware_generation(hb_qsv_get_platform(details->index)) >= QSV_G7)
                    {
                        query_capabilities(session, details->index, details->qsv_hardware_version, &details->qsv_hardware_info_avc, MFX_CODINGOPTION_ON);
                    }
                    if (details->qsv_hardware_info_avc.available == 0)
                    {
                        query_capabilities(session, details->index, details->qsv_hardware_version, &details->qsv_hardware_info_avc, MFX_CODINGOPTION_OFF);
                    }
                    details->qsv_hardware_info_avc.implementation = hw_impl | hw_preference;
                    if (hb_qsv_hardware_generation(hb_qsv_get_platform(details->index)) >= QSV_G7)
                    {
                        query_capabilities(session, details->index, details->qsv_hardware_version, &details->qsv_hardware_info_hevc, MFX_CODINGOPTION_ON);
                    }
                    if (details->qsv_hardware_info_hevc.available == 0)
                    {
                        query_capabilities(session, details->index, details->qsv_hardware_version, &details->qsv_hardware_info_hevc, MFX_CODINGOPTION_OFF);
                    }
                    details->qsv_hardware_info_hevc.implementation = hw_impl | hw_preference;
                    if (hb_qsv_hardware_generation(hb_qsv_get_platform(details->index)) > QSV_G8)
                    {
                        query_capabilities(session, details->index, details->qsv_hardware_version, &details->qsv_hardware_info_av1, MFX_CODINGOPTION_ON);
                        details->qsv_hardware_info_av1.implementation = hw_impl | hw_preference;
                    }
                    // now that we know which hardware encoders are
                    // available, we can set the preferred implementation
                    qsv_impl_set_preferred(details, "hardware");
                }
                MFXClose(session);
                // display must be closed after MFXClose
                hb_display_close(&display);
                hw_preference = 0;
            }
            else
            {
#if defined(_WIN32) || defined(__MINGW32__)
                // Windows only: After D3D11 we will try D3D9
                if (hw_preference == MFX_IMPL_VIA_D3D11)
                    hw_preference = MFX_IMPL_VIA_D3D9;
                else
#endif
                    hw_preference = 0;
            }
        }
        while(hw_preference != 0);
    }
    return 0;
}

static void log_decoder_capabilities(const int log_level, const hb_qsv_adapter_details_t *adapter_details, const char *prefix)
{
    char buffer[128] = "";

    if (hb_qsv_decode_h264_is_supported(adapter_details->index))
    {
        strcat(buffer, " h264");
    }

    if (hb_qsv_decode_h265_10_bit_is_supported(adapter_details->index))
    {
        strcat(buffer, " hevc (8bit: yes, 10bit: yes)");
    }
    else if (hb_qsv_decode_h265_is_supported(adapter_details->index))
    {
        strcat(buffer, " hevc (8bit: yes, 10bit: no)");
    }

    if (hb_qsv_decode_av1_is_supported(adapter_details->index))
    {
        strcat(buffer, " av1 (8bit: yes, 10bit: yes)");
    }

    if (hb_qsv_decode_vvc_is_supported(adapter_details->index))
    {
        strcat(buffer, " vvc (8bit: yes, 10bit: yes)");
    }

    hb_deep_log(log_level, "%s%s", prefix,
                strnlen(buffer, 1) ? buffer : " no decode support");
}

static void log_encoder_capabilities(const int log_level, const uint64_t caps, const char *prefix)
{
    /*
     * Note: keep the string short, as it may be logged by default.
     */
    char buffer[128] = "";

    if (caps & HB_QSV_CAP_LOWPOWER_ENCODE)
    {
        strcat(buffer, " lowpower");
    }
    /* B-Pyramid, with or without direct control (BRefType) */
    if (caps & HB_QSV_CAP_B_REF_PYRAMID)
    {
        if (caps & HB_QSV_CAP_OPTION2_BREFTYPE)
        {
            strcat(buffer, " breftype");
        }
        else
        {
            strcat(buffer, " bpyramid");
        }
    }
    /* Rate control: ICQ, lookahead (options: interlaced, downsampling) */
    if (caps & HB_QSV_CAP_RATECONTROL_LA)
    {
        if (caps & HB_QSV_CAP_RATECONTROL_ICQ)
        {
            strcat(buffer, " icq+la");
        }
        else
        {
            strcat(buffer, " la");
        }
        if (caps & HB_QSV_CAP_RATECONTROL_LAi)
        {
            strcat(buffer, "+i");
        }
        if (caps & HB_QSV_CAP_OPTION2_LA_DOWNS)
        {
            strcat(buffer, "+downs");
        }
    }
    else if (caps & HB_QSV_CAP_RATECONTROL_ICQ)
    {
        strcat(buffer, " icq");
    }
    if (caps & HB_QSV_CAP_VUI_VSINFO)
    {
        strcat(buffer, " vsinfo");
    }
    if (caps & HB_QSV_CAP_VUI_CHROMALOCINFO)
    {
        strcat(buffer, " chromalocinfo");
    }
    if (caps & HB_QSV_CAP_VUI_MASTERINGINFO)
    {
        strcat(buffer, " masteringinfo");
    }
    if (caps & HB_QSV_CAP_VUI_CLLINFO)
    {
        strcat(buffer, " cllinfo");
    }
    if (caps & HB_QSV_CAP_OPTION1)
    {
        strcat(buffer, " opt1");
    }
    if (caps & HB_QSV_CAP_OPTION2)
    {
        {
            strcat(buffer, " opt2");
        }
        if (caps & HB_QSV_CAP_OPTION2_MBBRC)
        {
            strcat(buffer, "+mbbrc");
        }
        if (caps & HB_QSV_CAP_OPTION2_EXTBRC)
        {
            strcat(buffer, "+extbrc");
        }
        if (caps & HB_QSV_CAP_OPTION2_TRELLIS)
        {
            strcat(buffer, "+trellis");
        }
        if (caps & HB_QSV_CAP_OPTION2_REPEATPPS)
        {
            strcat(buffer, "+repeatpps");
        }
        if (caps & HB_QSV_CAP_OPTION2_IB_ADAPT)
        {
            strcat(buffer, "+ib_adapt");
        }
        if (caps & HB_QSV_CAP_OPTION2_NMPSLICE)
        {
            strcat(buffer, "+nmpslice");
        }
    }
    if (caps & HB_QSV_CAP_AV1_SCREENCONTENT)
    {
        strcat(buffer, " av1screencontent");
    }
    if (caps & HB_QSV_CAP_HYPERENCODE)
    {
        strcat(buffer, " hyperencode");
    }
    if (caps & HB_QSV_CAP_AV1_BITSTREAM)
    {
        strcat(buffer, " av1bitstream");
    }

    hb_deep_log(log_level, "%s%s", prefix,
                strnlen(buffer, 1) ? buffer : " standard feature set");
}

static void hb_qsv_adapter_info_print(const hb_qsv_adapter_details_t *adapter_details)
{
    if (adapter_details->qsv_hardware_version.Version)
    {
        hb_log(" - Intel Media SDK hardware: API %"PRIu16".%"PRIu16" (minimum: %"PRIu16".%"PRIu16")",
                adapter_details->qsv_hardware_version.Major, adapter_details->qsv_hardware_version.Minor,
                HB_QSV_MINVERSION_MAJOR, HB_QSV_MINVERSION_MINOR);
    }

    if (adapter_details->qsv_software_version.Version)
    {
        hb_deep_log(3, " - Intel Media SDK software: API %"PRIu16".%"PRIu16" (minimum: %"PRIu16".%"PRIu16")",
                adapter_details->qsv_software_version.Major, adapter_details->qsv_software_version.Minor,
                HB_QSV_MINVERSION_MAJOR, HB_QSV_MINVERSION_MINOR);
    }

    log_decoder_capabilities(1, adapter_details, " - Decode support: ");

    if (adapter_details->hb_qsv_info_avc != NULL && adapter_details->hb_qsv_info_avc->available)
    {
        hb_log(" - H.264 encoder: yes");
        hb_log("    - preferred implementation: %s %s",
                hb_qsv_impl_get_name(adapter_details->hb_qsv_info_avc->implementation),
                hb_qsv_impl_get_via_name(adapter_details->hb_qsv_info_avc->implementation));
        if (adapter_details->qsv_hardware_info_avc.available)
        {
            log_encoder_capabilities(1, adapter_details->qsv_hardware_info_avc.capabilities,
                                "    - capabilities (hardware): ");
        }
        if (adapter_details->qsv_software_info_avc.available)
        {
            log_encoder_capabilities(3, adapter_details->qsv_software_info_avc.capabilities,
                                "    - capabilities (software): ");
        }
    }
    else
    {
        hb_log(" - H.264 encoder: no");
    }
    if (adapter_details->hb_qsv_info_hevc != NULL && adapter_details->hb_qsv_info_hevc->available)
    {
        hb_log(" - H.265 encoder: yes (8bit: yes, 10bit: %s)", (hb_qsv_hardware_generation(hb_qsv_get_platform(adapter_details->index)) < QSV_G6) ? "no" : "yes" );
        hb_log("    - preferred implementation: %s %s",
                hb_qsv_impl_get_name(adapter_details->hb_qsv_info_hevc->implementation),
                hb_qsv_impl_get_via_name(adapter_details->hb_qsv_info_hevc->implementation));
        if (adapter_details->qsv_hardware_info_hevc.available)
        {
            log_encoder_capabilities(1, adapter_details->qsv_hardware_info_hevc.capabilities,
                                "    - capabilities (hardware): ");
        }
        if (adapter_details->qsv_software_info_hevc.available)
        {
            log_encoder_capabilities(3, adapter_details->qsv_software_info_hevc.capabilities,
                                "    - capabilities (software): ");
        }
    }
    else
    {
        hb_log(" - H.265 encoder: no");
    }
    if (adapter_details->hb_qsv_info_av1 != NULL && adapter_details->hb_qsv_info_av1->available)
    {
        hb_log(" - AV1 encoder: yes (8bit: yes, 10bit: yes)");
        hb_log("    - preferred implementation: %s %s",
                hb_qsv_impl_get_name(adapter_details->hb_qsv_info_av1->implementation),
                hb_qsv_impl_get_via_name(adapter_details->hb_qsv_info_av1->implementation));
        if (adapter_details->qsv_hardware_info_av1.available)
        {
            log_encoder_capabilities(1, adapter_details->qsv_hardware_info_av1.capabilities,
                                "    - capabilities (hardware): ");
        }
    }
    else
    {
        hb_log(" - AV1 encoder: no");
    }
}

void hb_qsv_info_print()
{
    // is QSV available and usable?
    if (hb_qsv_available())
    {
#if defined(_WIN32) || defined(__MINGW32__)
        if (hb_list_count(g_qsv_adapters_details_list))
        {
            char gpu_list_str[256] = "";
            for (int i = 0; i < hb_list_count(g_qsv_adapters_details_list); i++)
            {
                const hb_qsv_adapter_details_t *details = hb_list_item(g_qsv_adapters_details_list, i);
                char value_str[256];
                snprintf(value_str, sizeof(value_str), "%d", details->index);
                if (i > 0)
                    strcat(gpu_list_str, ", ");
                strcat(gpu_list_str, value_str);
            }
            hb_log("Intel Quick Sync Video support: yes, gpu list: %s", gpu_list_str);
        }
        else
#endif
        {
            hb_log("Intel Quick Sync Video support: yes");
        }
        // also print the details about all QSV adapters
        for (int i = 0; i < hb_list_count(g_qsv_adapters_details_list); i++)
        {
            const hb_qsv_adapter_details_t *details = hb_list_item(g_qsv_adapters_details_list, i);
#if defined(_WIN32) || defined(__MINGW32__)
            hb_log("Intel Quick Sync Video %s adapter with index %d", hb_qsv_get_adapter_type(details), details->index);
#else
            hb_log("Intel Quick Sync Video %s adapter with index %d and renderD%d",
                hb_qsv_get_adapter_type(details), details->index, details->extended_device_id.DRMRenderNodeNum);
#endif
            hb_log("Impl %s library path: %s", details->impl_name, details->impl_path);
            hb_qsv_adapter_info_print(details);
        }
    }
    else
    {
        hb_log("Intel Quick Sync Video support: no");
    }
}

hb_qsv_info_t* hb_qsv_encoder_info_get(int adapter_index, int encoder)
{
    const hb_qsv_adapter_details_t *details = hb_qsv_get_adapters_details_by_index(adapter_index);

    if (details)
    {
        switch (encoder)
        {
            case HB_VCODEC_FFMPEG_QSV_H264:
                return details->hb_qsv_info_avc;
            case HB_VCODEC_FFMPEG_QSV_H265_10BIT:
            case HB_VCODEC_FFMPEG_QSV_H265:
                return details->hb_qsv_info_hevc;
            case HB_VCODEC_FFMPEG_QSV_AV1_10BIT:
            case HB_VCODEC_FFMPEG_QSV_AV1:
                return details->hb_qsv_info_av1;
            default:
                return NULL;
        }
    }
    return NULL;
}

const char* hb_qsv_decode_get_codec_name(enum AVCodecID codec_id)
{
    switch (codec_id)
    {
        case AV_CODEC_ID_H264:
            return "h264_qsv";

        case AV_CODEC_ID_HEVC:
            return "hevc_qsv";

        case AV_CODEC_ID_MPEG2VIDEO:
            return "mpeg2_qsv";

        case AV_CODEC_ID_AV1:
            return "av1_qsv";

        case AV_CODEC_ID_VVC:
            return "vvc_qsv";

        default:
            return NULL;
    }
}

int hb_qsv_decode_h264_is_supported(int adapter_index)
{
    return hb_qsv_hardware_generation(hb_qsv_get_platform(adapter_index)) >= QSV_G1;
}

int hb_qsv_decode_h265_is_supported(int adapter_index)
{
    return hb_qsv_hardware_generation(hb_qsv_get_platform(adapter_index)) >= QSV_G5;
}

int hb_qsv_decode_h265_10_bit_is_supported(int adapter_index)
{
    return hb_qsv_hardware_generation(hb_qsv_get_platform(adapter_index)) >= QSV_G6;
}

int hb_qsv_decode_av1_is_supported(int adapter_index)
{
    return hb_qsv_hardware_generation(hb_qsv_get_platform(adapter_index)) >= QSV_G8;
}

int hb_qsv_decode_vvc_is_supported(int adapter_index)
{
    return hb_qsv_hardware_generation(hb_qsv_get_platform(adapter_index)) > QSV_G9;
}

int hb_qsv_decode_is_codec_supported(int adapter_index, int video_codec_param, int pix_fmt, int width, int height)
{
    switch (video_codec_param)
    {
        case AV_CODEC_ID_H264:
            // QSV decode for AVC does not support higher video resolutions
            if (width > HB_QSV_AVC_DECODER_WIDTH_MAX || height > HB_QSV_AVC_DECODER_HEIGHT_MAX)
                return 0;

            if (pix_fmt == AV_PIX_FMT_NV12     ||
                pix_fmt == AV_PIX_FMT_YUV420P  ||
                pix_fmt == AV_PIX_FMT_YUVJ420P)
            {
                return hb_qsv_decode_h264_is_supported(adapter_index);
            }
            break;
        case AV_CODEC_ID_HEVC:
            if (pix_fmt == AV_PIX_FMT_NV12     ||
                pix_fmt == AV_PIX_FMT_YUV420P  ||
                pix_fmt == AV_PIX_FMT_YUVJ420P)
            {
                return hb_qsv_decode_h265_is_supported(adapter_index);
            }
            else if (pix_fmt == AV_PIX_FMT_P010LE ||
                pix_fmt == AV_PIX_FMT_YUV420P10)
            {
                return hb_qsv_decode_h265_10_bit_is_supported(adapter_index);
            }
            break;
        case AV_CODEC_ID_AV1:
            if (pix_fmt == AV_PIX_FMT_NV12     ||
                pix_fmt == AV_PIX_FMT_P010LE   ||
                pix_fmt == AV_PIX_FMT_YUV420P  ||
                pix_fmt == AV_PIX_FMT_YUVJ420P ||
                pix_fmt == AV_PIX_FMT_YUV420P10)
            {
                return hb_qsv_decode_av1_is_supported(adapter_index);
            }
            break;
        case AV_CODEC_ID_VVC:
            if (pix_fmt == AV_PIX_FMT_NV12     ||
                pix_fmt == AV_PIX_FMT_P010LE   ||
                pix_fmt == AV_PIX_FMT_YUV420P  ||
                pix_fmt == AV_PIX_FMT_YUVJ420P ||
                pix_fmt == AV_PIX_FMT_YUV420P10)
            {
                return hb_qsv_decode_vvc_is_supported(adapter_index);
            }
            break;
        default:
            return 0;
    }
    return 0;
}

static int hb_qsv_parse_options(hb_job_t *job)
{
    int err = 0;

    if (job->encoder_options != NULL && *job->encoder_options)
    {
        hb_dict_t *options_list;
        options_list = hb_encopts_to_dict(job->encoder_options, job->vcodec);
        hb_dict_iter_t iter;
        for (iter  = hb_dict_iter_init(options_list);
            iter != HB_DICT_ITER_DONE;
            iter  = hb_dict_iter_next(options_list, iter))
        {
            const char *key = hb_dict_iter_key(iter);
            hb_value_t *value = hb_dict_iter_value(iter);
            if (!strcasecmp(key, "gpu"))
            {
                char *str = hb_value_get_string_xform(value);
                int dx_index = hb_qsv_atoi(str, &err);
                free(str);
                if (!err)
                {
                    hb_log("hb_qsv_parse_options: gpu=%d", dx_index);
                    hb_qsv_param_parse_dx_index(job, dx_index);
                }
            }
            else if (!strcasecmp(key, "async-depth"))
            {
                char *str = hb_value_get_string_xform(value);
                int async_depth = hb_qsv_atoi(str, &err);
                free(str);
                if (!err)
                {
                    job->hw_device_async_depth = async_depth;
                }
            }
            else if (!strcasecmp(key, "memory-type"))
            {
                hb_triplet_t *mode = NULL;
                mode = hb_triplet4key(hb_qsv_memory_types, hb_value_get_string_xform(value));
                if (!mode)
                {
                    err = HB_QSV_PARAM_BAD_VALUE;
                }
                else
                {
                    job->qsv_ctx->memory_type = mode->value;
                }
            }
            else if (!strcasecmp(key, "scalingmode") ||
                     !strcasecmp(key, "vpp-sm"))
            {
                hb_triplet_t *mode = NULL;
                mode = hb_triplet4key(hb_qsv_vpp_scale_modes, hb_value_get_string_xform(value));
                if (!mode)
                {
                    err = HB_QSV_PARAM_BAD_VALUE;
                }
                else
                {
                    job->qsv_ctx->vpp_scale_mode = mode->name;
                }
            }
        }
        hb_dict_free(&options_list);
    }
    return 0;
}

int hb_qsv_setup_job(hb_job_t *job)
{
    if (job->qsv_ctx == NULL)
    {
        return 1;
    }

    // Parse the json parameter
    if (job->hw_device_index > -1)
    {
        hb_qsv_param_parse_dx_index(job, job->hw_device_index);
    }
    else
    {
        job->hw_device_index = hb_qsv_get_default_adapter_index();
    }

    // Parse the advanced options parameter
    hb_qsv_parse_options(job);

    int async_depth_default = hb_qsv_param_default_async_depth();
    if (job->hw_device_async_depth <= 0 || job->hw_device_async_depth > async_depth_default)
    {
        job->hw_device_async_depth = async_depth_default;
    }

    // Make sure QSV Decode is only True if the selected QSV adapter supports decode
    if (job->hw_decode & HB_DECODE_QSV)
    {
        int is_codec_supported = hb_qsv_decode_is_codec_supported(hb_qsv_get_adapter_index(),
            job->title->video_codec_param, job->input_pix_fmt,
            job->title->geometry.width, job->title->geometry.height);

        if (is_codec_supported == 0)
        {
            job->hw_decode &= ~HB_DECODE_QSV;
        }
    }

    return 0;
}

int hb_qsv_get_memory_type(hb_job_t *job)
{
    int qsv_full_path_is_enabled = hb_qsv_full_path_is_enabled(job);

    if (qsv_full_path_is_enabled)
    {
        if (job->qsv_ctx->memory_type == MFX_IOPATTERN_OUT_VIDEO_MEMORY)
            return MFX_IOPATTERN_OUT_VIDEO_MEMORY;
        else if (job->qsv_ctx->memory_type == MFX_IOPATTERN_OUT_SYSTEM_MEMORY)
            return MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
    }

    return qsv_full_path_is_enabled ? MFX_IOPATTERN_OUT_VIDEO_MEMORY : MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
}

static int are_filters_supported(hb_list_t *filters)
{
#if defined(_WIN32) || defined(__MINGW32__)
    int num_sw_filters = 0;
    for (int i = 0; i < hb_list_count(filters); i++)
    {
        hb_filter_object_t *filter = hb_list_item(filters, i);
        switch (filter->id)
        {
            // pixel format conversion is done via VPP filter
            case HB_FILTER_FORMAT:
            // cropping and scaling always done via VPP filter
            case HB_FILTER_CROP_SCALE:
            case HB_FILTER_ROTATE:
            case HB_FILTER_AVFILTER:
                break;
            case HB_FILTER_VFR:
            {
                // Mode 0 doesn't require access to the frame data
                int mode = hb_dict_get_int(filter->settings, "mode");
                if (mode == 0)
                {
                    break;
                }
            }
            default:
                // count only filters with access to frame data
                num_sw_filters++;
                break;
        }
    }
    return num_sw_filters == 0;
#else // other OS
    return 0;
#endif
}

int hb_qsv_full_path_is_enabled(hb_job_t *job)
{
    int qsv_full_path_is_enabled = 0;
    if (!job || !job->qsv_ctx)
    {
        return 0;
    }
#if defined(_WIN32) || defined(__MINGW32__)
    hb_qsv_info_t *info = hb_qsv_encoder_info_get(hb_qsv_get_adapter_index(), job->vcodec);

    qsv_full_path_is_enabled = (job->hw_decode & HB_DECODE_QSV &&
        info && hb_qsv_implementation_is_hardware(info->implementation) &&
        job->qsv_ctx && are_filters_supported(job->list_filter));
#endif
    return qsv_full_path_is_enabled;
}

int hb_qsv_atoindex(const char* const *arr, const char *str, int *err)
{
    int i;
    for (i = 0; arr[i] != NULL; i++)
    {
        if (!strcasecmp(arr[i], str))
        {
            break;
        }
    }
    *err = (arr[i] == NULL);
    return i;
}

// adapted from libx264
int hb_qsv_atobool(const char *str, int *err)
{
    if (!strcasecmp(str,    "1") ||
        !strcasecmp(str,  "yes") ||
        !strcasecmp(str, "true"))
    {
        return 1;
    }
    if (!strcasecmp(str,     "0") ||
        !strcasecmp(str,    "no") ||
        !strcasecmp(str, "false"))
    {
        return 0;
    }
    *err = 1;
    return 0;
}

// adapted from libx264
int hb_qsv_atoi(const char *str, int *err)
{
    char *end;
    int v = strtol(str, &end, 0);
    if (end == str || end[0] != '\0')
    {
        *err = 1;
    }
    return v;
}

// adapted from libx264
float hb_qsv_atof(const char *str, int *err)
{
    char *end;
    float v = strtod(str, &end);
    if (end == str || end[0] != '\0')
    {
        *err = 1;
    }
    return v;
}

static void add_qsv_param(AVDictionary** av_opts, const char *key, const char *value)
{
    // qsv_params=KEY=VALUE:NEXTKEY=NEXTVALUE
    char c[100] = {0};

    if(av_dict_get(*av_opts, "qsv_params", NULL, 0) != NULL)
    {
        strcat(c, ":"); // add separator if already exists
    }

    strcat(c, key);
    strcat(c, "=");
    strcat(c, value);

    av_dict_set(av_opts, "qsv_params", c, AV_DICT_APPEND);
}

static void add_qsv_param_u32(AVDictionary** av_opts, const char *key, mfxU32 value)
{
    if(value == 0) return; // Default == 0; skip

    int len = snprintf(NULL, 0, "%"PRIu32"", value); // u16 == u32
    char c[100] = {0};
    snprintf(c, len + 1, "%"PRIu32"", value);

    add_qsv_param(av_opts, key, c);
}

int hb_qsv_select_ffmpeg_options(qsv_data_t * qsv_data, hb_job_t *job, AVDictionary **av_opts)
{
#define MFX_STRUCT_TO_AV_OPTS(MEMBER_NAME) add_qsv_param_u32(av_opts, #MEMBER_NAME, param->videoParam->mfx.MEMBER_NAME);

    hb_qsv_info_t *qsv_info = qsv_data->qsv_info;
    hb_qsv_param_t *param = &qsv_data->param;

    int hw_generation = hb_qsv_hardware_generation(hb_qsv_get_platform(hb_qsv_get_adapter_index()));
    // sanitize ICQ
    // workaround for MediaSDK platforms below TGL to disable ICQ if incorrectly detected
    if (!(qsv_info->capabilities & HB_QSV_CAP_RATECONTROL_ICQ) ||
        ((param->videoParam->mfx.LowPower == MFX_CODINGOPTION_ON) && (hw_generation < QSV_G8)))
    {
        // ICQ not supported
        param->rc.icq = 0;
    }
    else
    {
        param->rc.icq = param->rc.icq && job->vquality > HB_INVALID_VIDEO_QUALITY;
    }

    // sanitize lookahead
    if (!(qsv_info->capabilities & HB_QSV_CAP_RATECONTROL_LA))
    {
        // lookahead not supported
        param->rc.lookahead = 0;
    }
    else if ((param->rc.lookahead)                                      &&
             (qsv_info->capabilities & HB_QSV_CAP_RATECONTROL_LAi) == 0 &&
             (param->videoParam->mfx.FrameInfo.PicStruct != MFX_PICSTRUCT_PROGRESSIVE))
    {
        // lookahead enabled but we can't use it
        hb_log("encqsvInit: LookAhead not used (LookAhead is progressive-only)");
        param->rc.lookahead = 0;
    }
    else
    {
        param->rc.lookahead = param->rc.lookahead && (param->rc.icq || job->vquality <= HB_INVALID_VIDEO_QUALITY);
    }

    if (job->qsv_ctx != NULL)
    {
        job->qsv_ctx->la_is_enabled = param->rc.lookahead ? 1 : 0;
    }

    // libmfx BRC parameters are 16 bits thus maybe overflow, then BRCParamMultiplier is needed
    // Comparison vbitrate in Kbps (kilobit) with vbv_max_bitrate, vbv_buffer_size, vbv_buffer_init in KB (kilobyte)
    int brc_param_multiplier = (FFMAX(FFMAX3(job->vbitrate, param->rc.vbv_max_bitrate, param->rc.vbv_buffer_size),
                            param->rc.vbv_buffer_init) + 0x10000) / 0x10000;
    // set VBV here (this will be overridden for CQP and ignored for LA)
    // only set BufferSizeInKB, InitialDelayInKB and MaxKbps if we have
    // them - otherwise Media SDK will pick values for us automatically
    if (param->rc.vbv_buffer_size > 0)
    {
        if (param->rc.vbv_buffer_init > 1.0)
        {
            param->videoParam->mfx.InitialDelayInKB = (param->rc.vbv_buffer_init / 8) / brc_param_multiplier;
            MFX_STRUCT_TO_AV_OPTS(InitialDelayInKB)
        }
        else if (param->rc.vbv_buffer_init > 0.0)
        {
            param->videoParam->mfx.InitialDelayInKB = (param->rc.vbv_buffer_size *
                                                          param->rc.vbv_buffer_init / 8) / brc_param_multiplier;
            MFX_STRUCT_TO_AV_OPTS(InitialDelayInKB)
        }
        param->videoParam->mfx.BufferSizeInKB       = (param->rc.vbv_buffer_size / 8) / brc_param_multiplier;
        MFX_STRUCT_TO_AV_OPTS(BufferSizeInKB)
        param->videoParam->mfx.BRCParamMultiplier   = brc_param_multiplier;
    }
    if (param->rc.vbv_max_bitrate > 0)
    {
        param->videoParam->mfx.MaxKbps              = param->rc.vbv_max_bitrate / brc_param_multiplier;
        MFX_STRUCT_TO_AV_OPTS(MaxKbps)
        param->videoParam->mfx.BRCParamMultiplier   = brc_param_multiplier;
    }

    // set rate control parameters
    if (job->vquality > HB_INVALID_VIDEO_QUALITY)
    {
        unsigned int upper_limit = 51;

        if (param->rc.icq)
        {
            // introduced in API 1.8
            if (param->rc.lookahead)
            {
                param->videoParam->mfx.RateControlMethod = MFX_RATECONTROL_LA_ICQ;
            }
            else
            {
                param->videoParam->mfx.RateControlMethod = MFX_RATECONTROL_ICQ;
            }
            param->videoParam->mfx.ICQQuality = HB_QSV_CLIP3(1, upper_limit, job->vquality);
            MFX_STRUCT_TO_AV_OPTS(ICQQuality)
        }
        else
        {
            // introduced in API 1.1
            // HEVC 10b has QP range as [-12;51]
            // with shift +12 needed to be in QSV's U16 range
            if (param->videoParam->mfx.CodecProfile == MFX_PROFILE_HEVC_MAIN10)
            {
                upper_limit = 63;
            }
            if (param->videoParam->mfx.CodecId == MFX_CODEC_AV1)
            {
                upper_limit = 255;
            }

            param->videoParam->mfx.RateControlMethod = MFX_RATECONTROL_CQP;
            param->videoParam->mfx.QPI = HB_QSV_CLIP3(0, upper_limit, job->vquality + param->rc.cqp_offsets[0]);
            param->videoParam->mfx.QPP = HB_QSV_CLIP3(0, upper_limit, job->vquality + param->rc.cqp_offsets[1]);
            param->videoParam->mfx.QPB = HB_QSV_CLIP3(0, upper_limit, job->vquality + param->rc.cqp_offsets[2]);

            MFX_STRUCT_TO_AV_OPTS(QPI)
            MFX_STRUCT_TO_AV_OPTS(QPP)
            MFX_STRUCT_TO_AV_OPTS(QPB)

            // CQP + ExtBRC can cause bad output
            param->codingOption2.ExtBRC = MFX_CODINGOPTION_OFF;
            av_dict_set(av_opts, "extbrc", "0", 0); // MFX_CODINGOPTION_OFF
        }
    }
    else if (job->vbitrate > 0)
    {
        if (param->rc.lookahead)
        {
            // introduced in API 1.7
            param->videoParam->mfx.RateControlMethod  = MFX_RATECONTROL_LA;
            param->videoParam->mfx.TargetKbps         = job->vbitrate / brc_param_multiplier;
            MFX_STRUCT_TO_AV_OPTS(TargetKbps)
            param->videoParam->mfx.BRCParamMultiplier = brc_param_multiplier;
            // ignored, but some drivers will change AsyncDepth because of it
            param->codingOption2.ExtBRC = MFX_CODINGOPTION_OFF;
            av_dict_set(av_opts, "extbrc", "0", 0); // MFX_CODINGOPTION_OFF
        }
        else
        {
            // introduced in API 1.0
            if (job->vbitrate == param->rc.vbv_max_bitrate)
            {
                param->videoParam->mfx.RateControlMethod = MFX_RATECONTROL_CBR;
            }
            else
            {
                param->videoParam->mfx.RateControlMethod = MFX_RATECONTROL_VBR;
            }
            param->videoParam->mfx.TargetKbps            = job->vbitrate / brc_param_multiplier;
            MFX_STRUCT_TO_AV_OPTS(TargetKbps)
            param->videoParam->mfx.BRCParamMultiplier    = brc_param_multiplier;
        }
    }
    else
    {
        hb_error("encqsvInit: invalid rate control (%f, %d)",
                 job->vquality, job->vbitrate);
        return -1;
    }

    MFX_STRUCT_TO_AV_OPTS(RateControlMethod);

    // if VBV is enabled but ignored, log it
    if (param->rc.vbv_max_bitrate > 0 || param->rc.vbv_buffer_size > 0)
    {
        switch (param->videoParam->mfx.RateControlMethod)
        {
            case MFX_RATECONTROL_LA:
            case MFX_RATECONTROL_LA_ICQ:
                hb_log("encqsvInit: LookAhead enabled, ignoring VBV");
                break;
            case MFX_RATECONTROL_ICQ:
                hb_log("encqsvInit: ICQ rate control, ignoring VBV");
                break;
            default:
                break;
        }
    }

    // set the GOP structure
    if (param->gop.gop_ref_dist < 0)
    {
        if ((hw_generation >= QSV_G8) &&
            (param->videoParam->mfx.CodecId == MFX_CODEC_HEVC ||
            param->videoParam->mfx.CodecId == MFX_CODEC_AV1))
        {
            param->gop.gop_ref_dist = 8;
        }
        else
        {
            param->gop.gop_ref_dist = 4;
        }
    }
    param->videoParam->mfx.GopRefDist = param->gop.gop_ref_dist;

    // set the keyframe interval
    if (param->gop.gop_pic_size < 0)
    {
        double rate = (double)job->orig_vrate.num / job->orig_vrate.den + 0.5;
        // set the keyframe interval based on the framerate
        param->gop.gop_pic_size = (int)(FFMIN(rate * 2, 120));
    }
    param->videoParam->mfx.GopPicSize = param->gop.gop_pic_size;

    // set the Hyper Encode structure
    if (param->hyperEncodeParam->value != MFX_HYPERMODE_OFF)
    {
        if (param->videoParam->mfx.CodecId == MFX_CODEC_HEVC)
        {
            param->videoParam->mfx.IdrInterval = 1;
        }
        else if (param->videoParam->mfx.CodecId == MFX_CODEC_AVC)
        {
            param->videoParam->mfx.IdrInterval = 0;
        }

        MFX_STRUCT_TO_AV_OPTS(IdrInterval)
        // sanitize some of the encoding parameters
        param->videoParam->mfx.GopPicSize = (int)(FFMIN(param->gop.gop_pic_size, 60));
        param->videoParam->AsyncDepth = (int)(FFMAX(param->videoParam->AsyncDepth, 60));
        av_dict_set_int(av_opts, "async_depth", param->videoParam->AsyncDepth, 0);

        char hyperencode[16];
        snprintf(hyperencode, sizeof(hyperencode), "%s", qsv_data->param.hyperEncodeParam->key);
        av_dict_set(av_opts, "dual_gfx", hyperencode, 0);
        hb_log("encavcodec: Hyper Encoding mode: %s", hyperencode);
    }
    MFX_STRUCT_TO_AV_OPTS(GopPicSize)

    // sanitize some settings that affect memory consumption
    if (!qsv_data->is_sys_mem)
    {
        // limit these to avoid running out of resources (causes hang)
        param->videoParam->mfx.GopRefDist   = FFMIN(param->videoParam->mfx.GopRefDist,
                                                       param->rc.lookahead ? 8 : 16);
        param->codingOption2.LookAheadDepth = FFMIN(param->codingOption2.LookAheadDepth,
                                                       param->rc.lookahead ? (48 - param->videoParam->mfx.GopRefDist -
                                                                                 3 * !param->videoParam->mfx.GopRefDist) : 0);
    }
    else
    {
        // encode-only is a bit less sensitive to memory issues
        param->videoParam->mfx.GopRefDist   = FFMIN(param->videoParam->mfx.GopRefDist, 16);
        param->codingOption2.LookAheadDepth = FFMIN(param->codingOption2.LookAheadDepth,
                                                       param->rc.lookahead ? 100 : 0);
    }
    MFX_STRUCT_TO_AV_OPTS(GopRefDist)

    if (param->rc.lookahead)
    {
        // LookAheadDepth 10 will cause a hang with some driver versions
        param->codingOption2.LookAheadDepth = FFMAX(param->codingOption2.LookAheadDepth, 11);
    }
    av_dict_set_int(av_opts, "look_ahead_depth", param->codingOption2.LookAheadDepth, 0);

    if(qsv_data->qsv_info->capabilities & HB_QSV_CAP_LOWPOWER_ENCODE)
    {
        char low_power[7];
        snprintf(low_power, sizeof(low_power), "%d", qsv_data->param.low_power);
        av_dict_set(av_opts, "low_power", low_power, 0);
        if(qsv_data->param.low_power)
            hb_log("encavcodec: using Low Power mode");
    }

    if((qsv_data->qsv_info->capabilities & HB_QSV_CAP_AV1_SCREENCONTENT) &&
        qsv_data->param.av1ScreenContentToolsParam.IntraBlockCopy)
    {
        char intrabc[7];
        snprintf(intrabc, sizeof(intrabc), "%d", qsv_data->param.av1ScreenContentToolsParam.IntraBlockCopy);
        av_dict_set(av_opts, "intrabc", intrabc, 0);
        hb_log("encavcodec: ScreenContentCoding is enabled IBC %s",
            qsv_data->param.av1ScreenContentToolsParam.IntraBlockCopy ? "on" : "off");
    }

    if((qsv_data->qsv_info->capabilities & HB_QSV_CAP_AV1_SCREENCONTENT) &&
        qsv_data->param.av1ScreenContentToolsParam.Palette)
    {
        char palette_mode[7];
        snprintf(palette_mode, sizeof(palette_mode), "%d", qsv_data->param.av1ScreenContentToolsParam.Palette);
        av_dict_set(av_opts, "palette_mode", palette_mode, 0);
        hb_log("encavcodec: ScreenContentCoding is enabled Palette %s",
            qsv_data->param.av1ScreenContentToolsParam.Palette ? "on" : "off");
    }

  // Transcoding Info
  MFX_STRUCT_TO_AV_OPTS(BRCParamMultiplier)
  // scenecut
  MFX_STRUCT_TO_AV_OPTS(GopOptFlag)

#undef MFX_STRUCT_TO_AV_OPTS
  return 0;
}

int hb_qsv_apply_encoder_options(qsv_data_t * qsv_data, hb_job_t *job, AVDictionary **av_opts)
{
    if (qsv_data == NULL)
    {
        hb_error("hb_qsv_apply_encoder_options: invalid pointer qsv_data=%p", qsv_data);
        return -1;
    }

    qsv_data->qsv_info = hb_qsv_encoder_info_get(hb_qsv_get_adapter_index(), job->vcodec);

    if (qsv_data->qsv_info == NULL)
    {
        hb_error("hb_qsv_apply_encoder_options: invalid pointer qsv_data->qsv_info=%p", qsv_data->qsv_info);
        return -1;
    }

    mfxVideoParam videoParam = {0};
    qsv_data->param.videoParam = &videoParam;
    qsv_data->is_sys_mem = (hb_qsv_get_memory_type(job) == MFX_IOPATTERN_OUT_SYSTEM_MEMORY);

    int ret = hb_qsv_param_default(&qsv_data->param, qsv_data->qsv_info);
    if (ret)
    {
        return ret;
    }

    if (job->encoder_options != NULL && *job->encoder_options)
    {
        hb_dict_t *options_list;
        options_list = hb_encopts_to_dict(job->encoder_options, job->vcodec);

        hb_dict_iter_t iter;
        for (iter  = hb_dict_iter_init(options_list);
             iter != HB_DICT_ITER_DONE;
             iter  = hb_dict_iter_next(options_list, iter))
        {
            const char *key = hb_dict_iter_key(iter);
            hb_value_t *value = hb_dict_iter_value(iter);
            char *str = hb_value_get_string_xform(value);

            switch (hb_qsv_param_parse(av_opts, &qsv_data->param, qsv_data->qsv_info, job, key, str))
            {
                case HB_QSV_PARAM_OK:
                    break;

                case HB_QSV_PARAM_BAD_NAME:
                    hb_log("qsv_encavcodecInit: hb_qsv_param_parse: bad key %s", key);
                    break;
                case HB_QSV_PARAM_BAD_VALUE:
                    hb_log("qsv_encavcodecInit: hb_qsv_param_parse: bad value %s for key %s",
                           str, key);
                    break;
                case HB_QSV_PARAM_UNSUPPORTED:
                    hb_log("qsv_encavcodecInit: hb_qsv_param_parse: unsupported option %s",
                           key);
                    break;

                case HB_QSV_PARAM_ERROR:
                default:
                    hb_log("qsv_encavcodecInit: hb_qsv_param_parse: unknown error");
                    break;
            }
            free(str);
        }
        hb_dict_free(&options_list);
    }

    ret = hb_qsv_select_ffmpeg_options(qsv_data, job, av_opts);
    if(ret)
    {
        hb_log("encavcodec: parsing ffmpeg options failed");
        return ret;
    }

    hb_log("encavcodec: using%s%s path",
           hb_qsv_full_path_is_enabled(job) ? " full QSV" : " encode-only",
           hb_qsv_get_memory_type(job) == MFX_IOPATTERN_OUT_VIDEO_MEMORY ? " via video memory" : " via system memory");

    return 0;
}

int hb_qsv_param_parse(AVDictionary** av_opts, hb_qsv_param_t *param, hb_qsv_info_t *info, hb_job_t *job,
                       const char *key, const char *value)
{
    float fvalue;
    int ivalue, error = 0;
    if (param == NULL || info == NULL)
    {
        return HB_QSV_PARAM_ERROR;
    }
    if (value == NULL || value[0] == '\0')
    {
        value = "true";
    }
    else if (value[0] == '=')
    {
        value++;
    }
    if (key == NULL || key[0] == '\0')
    {
        return HB_QSV_PARAM_BAD_NAME;
    }
    else if (!strncasecmp(key, "no-", 3))
    {
        key  += 3;
        value = hb_qsv_atobool(value, &error) ? "false" : "true";
        if (error)
        {
            return HB_QSV_PARAM_BAD_VALUE;
        }
    }
    if (!strcasecmp(key, "target-usage") ||
        !strcasecmp(key, "tu"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            int target_usage = HB_QSV_CLIP3(MFX_TARGETUSAGE_1, MFX_TARGETUSAGE_7, ivalue);
            switch (target_usage)
            {
                case MFX_TARGETUSAGE_1:
                    av_dict_set(av_opts, "preset", "veryslow", 0);
                    break;
                case MFX_TARGETUSAGE_2:
                    av_dict_set(av_opts, "preset", "slower", 0);
                    break;
                case MFX_TARGETUSAGE_3:
                    av_dict_set(av_opts, "preset", "slow", 0);
                    break;
                case MFX_TARGETUSAGE_4:
                    av_dict_set(av_opts, "preset", "medium", 0);
                    break;
                case MFX_TARGETUSAGE_5:
                    av_dict_set(av_opts, "preset", "fast", 0);
                    break;
                case MFX_TARGETUSAGE_6:
                    av_dict_set(av_opts, "preset", "faster", 0);
                    break;
                case MFX_TARGETUSAGE_7:
                    av_dict_set(av_opts, "preset", "veryfast", 0);
                    break;
                default:
                    break;
            }
        }
    }
    else if (!strcasecmp(key, "num-ref-frame") ||
             !strcasecmp(key, "ref"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            av_dict_set_int(av_opts, "refs", HB_QSV_CLIP3(0, 16, ivalue), 0);
        }
    }
    else if (!strcasecmp(key, "gop-ref-dist"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            av_dict_set_int(av_opts, "bf", HB_QSV_CLIP3(-1, 32, ivalue), 0);
        }
    }
    else if (!strcasecmp(key, "gop-pic-size") ||
             !strcasecmp(key, "keyint"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            av_dict_set_int(av_opts, "g", HB_QSV_CLIP3(-1, UINT16_MAX, ivalue), 0);
        }
    }
    else if (!strcasecmp(key, "b-pyramid"))
    {
        if (info->capabilities & HB_QSV_CAP_B_REF_PYRAMID)
        {
            ivalue = hb_qsv_atoi(value, &error);
            if (!error)
            {
                av_dict_set_int(av_opts, "b_strategy", HB_QSV_CLIP3(-1, 1, ivalue), 0);
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
    }
    else if (!strcasecmp(key, "scenecut"))
    {
        ivalue = hb_qsv_atobool(value, &error);
        if (!error)
        {
            if (!ivalue)
            {
                param->videoParam->mfx.GopOptFlag |= MFX_GOP_STRICT;
            }
            else
            {
                param->videoParam->mfx.GopOptFlag &= ~MFX_GOP_STRICT;
            }
        }
    }
    else if (!strcasecmp(key, "adaptive-i") ||
             !strcasecmp(key, "i-adapt"))
    {
        if (info->capabilities & HB_QSV_CAP_OPTION2_IB_ADAPT)
        {
            ivalue = hb_qsv_atobool(value, &error);
            if (!error)
            {
                av_dict_set_int(av_opts, "adaptive_i", ivalue, 0);
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
    }
    else if (!strcasecmp(key, "adaptive-b") ||
             !strcasecmp(key, "b-adapt"))
    {
        if (info->capabilities & HB_QSV_CAP_OPTION2_IB_ADAPT)
        {
            ivalue = hb_qsv_atobool(value, &error);
            if (!error)
            {
                av_dict_set_int(av_opts, "adaptive_b", ivalue, 0);
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
    }
    else if (!strcasecmp(key, "force-cqp"))
    {
        ivalue = hb_qsv_atobool(value, &error);
        if (!error)
        {
            param->rc.icq = !ivalue;
        }
    }
    else if (!strcasecmp(key, "cqp-offset-i"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            param->rc.cqp_offsets[0] = HB_QSV_CLIP3(INT16_MIN, INT16_MAX, ivalue);
        }
    }
    else if (!strcasecmp(key, "cqp-offset-p"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            param->rc.cqp_offsets[1] = HB_QSV_CLIP3(INT16_MIN, INT16_MAX, ivalue);
        }
    }
    else if (!strcasecmp(key, "cqp-offset-b"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            param->rc.cqp_offsets[2] = HB_QSV_CLIP3(INT16_MIN, INT16_MAX, ivalue);
        }
    }
    else if (!strcasecmp(key, "vbv-init"))
    {
        fvalue = hb_qsv_atof(value, &error);
        if (!error)
        {
            param->rc.vbv_buffer_init = HB_QSV_CLIP3(0, INT32_MAX, fvalue);
        }
    }
    else if (!strcasecmp(key, "vbv-bufsize"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            param->rc.vbv_buffer_size = HB_QSV_CLIP3(0, INT32_MAX, ivalue);
        }
    }
    else if (!strcasecmp(key, "vbv-maxrate"))
    {
        ivalue = hb_qsv_atoi(value, &error);
        if (!error)
        {
            param->rc.vbv_max_bitrate = HB_QSV_CLIP3(0, INT32_MAX, ivalue);
        }
    }
    else if (!strcasecmp(key, "cavlc") || !strcasecmp(key, "cabac"))
    {
        if (info->capabilities & HB_QSV_CAP_OPTION1)
        {
            switch (info->codec_id)
            {
                case MFX_CODEC_AVC:
                    ivalue = hb_qsv_atobool(value, &error);
                    break;
                default:
                    return HB_QSV_PARAM_UNSUPPORTED;
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
        if (!error)
        {
            if (!strcasecmp(key, "cabac"))
            {
                ivalue = !ivalue;
            }
            av_dict_set_int(av_opts, "cavlc", ivalue, 0);
        }
    }
    else if (!strcasecmp(key, "colorprim"))
    {
        if (info->capabilities & HB_QSV_CAP_VUI_VSINFO)
        {
            switch (info->codec_id)
            {
                case MFX_CODEC_AVC:
                    ivalue = hb_qsv_atoindex(hb_h264_colorprim_names, value, &error);
                    break;
                case MFX_CODEC_HEVC:
                    ivalue = hb_qsv_atoindex(hb_h265_colorprim_names, value, &error);
                    break;
                default:
                    return HB_QSV_PARAM_UNSUPPORTED;
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
        if (!error)
        {
            av_dict_set_int(av_opts, "color_primaries", ivalue, 0);
        }
    }
    else if (!strcasecmp(key, "transfer"))
    {
        if (info->capabilities & HB_QSV_CAP_VUI_VSINFO)
        {
            switch (info->codec_id)
            {
                case MFX_CODEC_AVC:
                    ivalue = hb_qsv_atoindex(hb_h264_transfer_names, value, &error);
                    break;
                case MFX_CODEC_HEVC:
                    ivalue = hb_qsv_atoindex(hb_h265_transfer_names, value, &error);
                    break;
                default:
                    return HB_QSV_PARAM_UNSUPPORTED;
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
        if (!error)
        {
            av_dict_set_int(av_opts, "color_trc", ivalue, 0);
        }
    }
    else if (!strcasecmp(key, "colormatrix"))
    {
        if (info->capabilities & HB_QSV_CAP_VUI_VSINFO)
        {
            switch (info->codec_id)
            {
                case MFX_CODEC_AVC:
                    ivalue = hb_qsv_atoindex(hb_h264_colmatrix_names, value, &error);
                    break;
                case MFX_CODEC_HEVC:
                    ivalue = hb_qsv_atoindex(hb_h265_colmatrix_names, value, &error);
                    break;
                default:
                    return HB_QSV_PARAM_UNSUPPORTED;
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
        if (!error)
        {
            av_dict_set_int(av_opts, "colorspace", ivalue, 0);
        }
    }
    else if (!strcasecmp(key, "tff") ||
             !strcasecmp(key, "interlaced"))
    {
        switch (info->codec_id)
        {
            case MFX_CODEC_AVC:
                ivalue = hb_qsv_atobool(value, &error);
                break;
            default:
                return HB_QSV_PARAM_UNSUPPORTED;
        }
        if (!error)
        {
            ivalue = ivalue ? MFX_PICSTRUCT_FIELD_TFF : MFX_PICSTRUCT_PROGRESSIVE;
            av_dict_set_int(av_opts, "flags", ivalue, AV_DICT_APPEND);
        }
    }
    else if (!strcasecmp(key, "bff"))
    {
        switch (info->codec_id)
        {
            case MFX_CODEC_AVC:
                ivalue = hb_qsv_atobool(value, &error);
                break;
            default:
                return HB_QSV_PARAM_UNSUPPORTED;
        }
        if (!error)
        {
            ivalue = ivalue ? MFX_PICSTRUCT_FIELD_BFF : MFX_PICSTRUCT_PROGRESSIVE;
            av_dict_set_int(av_opts, "flags", ivalue, AV_DICT_APPEND);
        }
    }
    else if (!strcasecmp(key, "mbbrc"))
    {
        if (info->capabilities & HB_QSV_CAP_OPTION2_MBBRC)
        {
            ivalue = hb_qsv_atobool(value, &error);
            if (!error)
            {
                av_dict_set_int(av_opts, "mbbrc", ivalue, 0);
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
    }
    else if (!strcasecmp(key, "extbrc"))
    {
        if (info->capabilities & HB_QSV_CAP_OPTION2_EXTBRC)
        {
            ivalue = hb_qsv_atobool(value, &error);
            if (!error)
            {
                av_dict_set_int(av_opts, "extbrc", ivalue, 0);
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
    }
    else if (!strcasecmp(key, "lookahead") ||
             !strcasecmp(key, "la"))
    {
        if (info->capabilities & HB_QSV_CAP_RATECONTROL_LA)
        {
            ivalue = hb_qsv_atobool(value, &error);
            if (!error)
            {
                param->rc.lookahead = ivalue;
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
    }
    else if (!strcasecmp(key, "lookahead-depth") ||
             !strcasecmp(key, "la-depth"))
    {
        if (info->capabilities & HB_QSV_CAP_RATECONTROL_LA)
        {
            ivalue = hb_qsv_atoi(value, &error);
            if (!error)
            {
                av_dict_set_int(av_opts, "look_ahead_depth", HB_QSV_CLIP3(10, 100, ivalue), 0);
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
    }
    else if (!strcasecmp(key, "lookahead-ds") ||
             !strcasecmp(key, "la-ds"))
    {
        if (info->capabilities & HB_QSV_CAP_OPTION2_LA_DOWNS)
        {
            ivalue = hb_qsv_atoi(value, &error);
            if (!error)
            {
                ivalue = HB_QSV_CLIP3(MFX_LOOKAHEAD_DS_UNKNOWN, MFX_LOOKAHEAD_DS_4x, ivalue);
                av_dict_set_int(av_opts, "look_ahead_downsampling", ivalue, 0);
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
    }
    else if (!strcasecmp(key, "trellis"))
    {
        if (info->capabilities & HB_QSV_CAP_OPTION2_TRELLIS)
        {
            ivalue = hb_qsv_atoi(value, &error);
            if (!error)
            {
                av_dict_set_int(av_opts, "trellis", ivalue, 0);
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
    }
    else if (!strcasecmp(key, "repeatpps"))
    {
        if (info->capabilities & HB_QSV_CAP_OPTION2_REPEATPPS)
        {
            ivalue = hb_qsv_atobool(value, &error);
            if (!error)
            {
                av_dict_set_int(av_opts, "repeat_pps", ivalue, 0);
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
    }
    else if (!strcasecmp(key, "lowpower"))
    {
        if (info->capabilities & HB_QSV_CAP_LOWPOWER_ENCODE)
        {
            ivalue = hb_qsv_atobool(value, &error);
            if (!error)
            {
                param->low_power = ivalue;
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
    }
    else if (!strcasecmp(key, "gpu"))
    {
        // Already parsed in QSV initialization
    }
    else if (!strcasecmp(key, "memory-type"))
    {
        // Check if was parsed already in decoder initialization
        if (job->qsv_ctx && !job->qsv_ctx->memory_type)
        {
            hb_triplet_t* mode = NULL;
            mode = hb_triplet4key(hb_qsv_memory_types, value);
            if (!mode)
                error = HB_QSV_PARAM_BAD_VALUE;
            else
                job->qsv_ctx->memory_type = mode->value;
        }
    }
    else if (!strcasecmp(key, "scalingmode") ||
             !strcasecmp(key, "vpp-sm"))
    {
        // Already parsed it in decoder but need to check support
        if (info->capabilities & HB_QSV_CAP_VPP_SCALING)
        {
            hb_triplet_t *mode = NULL;
            mode = hb_triplet4key(hb_qsv_vpp_scale_modes, value);
            if (!mode)
            {
                error = HB_QSV_PARAM_BAD_VALUE;
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
    }
    else if (!strcasecmp(key, "hyperencode"))
    {
        hb_triplet_t *mode = NULL;
        if (info->capabilities & HB_QSV_CAP_HYPERENCODE)
        {
            mode = hb_triplet4key(hb_qsv_hyper_encode_modes, value);
            if (!mode)
            {
                error = HB_QSV_PARAM_BAD_VALUE;
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
        if (mode)
        {
            param->hyperEncodeParam = mode;
        }
    }
    else if (!strcasecmp(key, "palette"))
    {
        if (info->capabilities & HB_QSV_CAP_AV1_SCREENCONTENT)
        {
            ivalue = hb_qsv_atobool(value, &error);
            if (!error)
            {
                param->av1ScreenContentToolsParam.Palette = ivalue;
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
    }
    else if (!strcasecmp(key, "intrabc"))
    {
        if (info->capabilities & HB_QSV_CAP_AV1_SCREENCONTENT)
        {
            ivalue = hb_qsv_atobool(value, &error);
            if (!error)
            {
                param->av1ScreenContentToolsParam.IntraBlockCopy = ivalue;
            }
        }
        else
        {
            return HB_QSV_PARAM_UNSUPPORTED;
        }
    }
    else if (!strcasecmp(key, "async-depth"))
    {
        int async_depth = hb_qsv_atoi(value, &error);
        if (!error)
        {
            av_dict_set_int(av_opts, "async_depth", async_depth, 0);
            param->videoParam->AsyncDepth = async_depth;
        }
    }
    else
    {
        /*
         * TODO:
         * - slice count (num-slice/slices, num-mb-per-slice/slice-max-mbs)
         * - open-gop
         * - fake-interlaced (mfxExtCodingOption.FramePicture???)
         * - intra-refresh
         */
        return HB_QSV_PARAM_BAD_NAME;
    }
    return error ? HB_QSV_PARAM_BAD_VALUE : HB_QSV_PARAM_OK;
}

int hb_qsv_profile_parse(hb_qsv_param_t *param, hb_qsv_info_t *info, const char *profile_key, const int codec)
{
    hb_triplet_t *profile = NULL;
    if (profile_key != NULL && *profile_key && strcasecmp(profile_key, "auto"))
    {
        switch (param->videoParam->mfx.CodecId)
        {
            case MFX_CODEC_AVC:
                profile = hb_triplet4key(hb_qsv_h264_profiles, profile_key);
                break;

            case MFX_CODEC_HEVC:
                profile = hb_triplet4key(hb_qsv_h265_profiles, profile_key);

                /* HEVC10 supported starting from KBL/G6 */
                if (profile->value == MFX_PROFILE_HEVC_MAIN10 &&
                    hb_qsv_hardware_generation(hb_qsv_get_platform(hb_qsv_get_adapter_index())) < QSV_G6)
                {
                    hb_log("qsv: HEVC Main10 is not supported on this platform");
                    profile = NULL;
                }
                break;

            case MFX_CODEC_AV1:
                profile = hb_triplet4key(hb_qsv_av1_profiles, profile_key);

                if (hb_qsv_hardware_generation(hb_qsv_get_platform(hb_qsv_get_adapter_index())) <= QSV_G8)
                {
                    hb_log("qsv: AV1 is not supported on this platform");
                    profile = NULL;
                }
                break;

            default:
                break;
        }
        if (profile == NULL)
        {
            return -1;
        }
        param->videoParam->mfx.CodecProfile = profile->value;
    }
    /* HEVC 10 bits defaults to Main 10 */
    else if (((profile_key != NULL && !strcasecmp(profile_key, "auto")) || profile_key == NULL) &&
              codec == HB_VCODEC_FFMPEG_QSV_H265_10BIT &&
              param->videoParam->mfx.CodecId == MFX_CODEC_HEVC &&
              hb_qsv_hardware_generation(hb_qsv_get_platform(hb_qsv_get_adapter_index())) >= QSV_G6)
    {
         profile = &hb_qsv_h265_profiles[1];
         param->videoParam->mfx.CodecProfile = profile->value;
    }
    /* AV1 10 bits defaults to Main */
    else if (((profile_key != NULL && !strcasecmp(profile_key, "auto")) || profile_key == NULL) &&
              codec == HB_VCODEC_FFMPEG_QSV_AV1_10BIT &&
              param->videoParam->mfx.CodecId == MFX_CODEC_AV1 &&
              hb_qsv_hardware_generation(hb_qsv_get_platform(hb_qsv_get_adapter_index())) > QSV_G8)
    {
        profile = &hb_qsv_av1_profiles[0];
        param->videoParam->mfx.CodecProfile = profile->value;
    }
    return 0;
}

const char* const* hb_qsv_preset_get_names()
{
    if (hb_qsv_hardware_generation(hb_qsv_get_platform(hb_qsv_get_adapter_index())) >= QSV_G3)
    {
        return hb_qsv_preset_names2;
    }
    else
    {
        return hb_qsv_preset_names1;
    }
}

const char* const* hb_qsv_profile_get_names(int encoder)
{
    switch (encoder)
    {
        case HB_VCODEC_FFMPEG_QSV_H264:
            return hb_h264_profile_names_8bit;
        case HB_VCODEC_FFMPEG_QSV_H265_8BIT:
            return hb_h265_profile_names_8bit;
        case HB_VCODEC_FFMPEG_QSV_H265_10BIT:
            return hb_qsv_h265_profiles_names_10bit;
        case HB_VCODEC_FFMPEG_QSV_AV1_10BIT:
        case HB_VCODEC_FFMPEG_QSV_AV1:
            return hb_qsv_av1_profiles_names;
        default:
            return NULL;
    }
}

const char* const* hb_qsv_level_get_names(int encoder)
{
    switch (encoder)
    {
        case HB_VCODEC_FFMPEG_QSV_H264:
            return hb_qsv_h264_level_names;
        case HB_VCODEC_FFMPEG_QSV_H265_10BIT:
        case HB_VCODEC_FFMPEG_QSV_H265:
            return hb_qsv_h265_level_names;
        case HB_VCODEC_FFMPEG_QSV_AV1_10BIT:
        case HB_VCODEC_FFMPEG_QSV_AV1:
            return hb_qsv_av1_level_names;
        default:
            return NULL;
    }
}

const int* hb_qsv_get_pix_fmts(int encoder)
{
    switch (encoder)
    {
    case HB_VCODEC_FFMPEG_QSV_H264:
    case HB_VCODEC_FFMPEG_QSV_H265:
    case HB_VCODEC_FFMPEG_QSV_AV1:
        return hb_qsv_pix_fmts;
    case HB_VCODEC_FFMPEG_QSV_H265_10BIT:
    case HB_VCODEC_FFMPEG_QSV_AV1_10BIT:
        return hb_qsv_10bit_pix_fmts;

    default:
        return hb_qsv_pix_fmts;
    }
}

const char* hb_qsv_video_quality_get_name(uint32_t codec)
{
    uint64_t caps = 0;
    const hb_qsv_adapter_details_t *details = hb_qsv_get_adapters_details_by_index(hb_qsv_get_adapter_index());
    if (details)
    {
        switch (codec)
        {
            case HB_VCODEC_FFMPEG_QSV_H264:
                if (details->hb_qsv_info_avc != NULL) caps = details->hb_qsv_info_avc->capabilities;
                break;

            case HB_VCODEC_FFMPEG_QSV_H265_10BIT:
            case HB_VCODEC_FFMPEG_QSV_H265:
                if (details->hb_qsv_info_hevc != NULL) caps = details->hb_qsv_info_hevc->capabilities;
                break;

            case HB_VCODEC_FFMPEG_QSV_AV1_10BIT:
            case HB_VCODEC_FFMPEG_QSV_AV1:
                if (details->hb_qsv_info_av1 != NULL) caps = details->hb_qsv_info_av1->capabilities;
                break;

            default:
                break;
        }
    }
    return (caps & HB_QSV_CAP_RATECONTROL_ICQ) ? "ICQ" : "QP";
}

void hb_qsv_video_quality_get_limits(uint32_t codec, float *low, float *high,
                                     float *granularity, int *direction)
{
    uint64_t caps = 0;
    const hb_qsv_adapter_details_t *details = hb_qsv_get_adapters_details_by_index(hb_qsv_get_adapter_index());
    if (details)
    {
        switch (codec)
        {
            case HB_VCODEC_FFMPEG_QSV_H265_10BIT:
            case HB_VCODEC_FFMPEG_QSV_H265:
                if (details->hb_qsv_info_hevc != NULL) caps = details->hb_qsv_info_hevc->capabilities;
                *direction   = 1;
                *granularity = 1.;
                *low         = (caps & HB_QSV_CAP_RATECONTROL_ICQ) ? 1. : 0.;
                *high        = 51.;
                break;

            case HB_VCODEC_FFMPEG_QSV_AV1_10BIT:
            case HB_VCODEC_FFMPEG_QSV_AV1:
                if (details->hb_qsv_info_av1 != NULL) caps = details->hb_qsv_info_av1->capabilities;
                *direction   = 1;
                *granularity = 1.;
                *low         = (caps & HB_QSV_CAP_RATECONTROL_ICQ) ? 1. : 0.;
                *high        = 51.;
                break;

            case HB_VCODEC_FFMPEG_QSV_H264:
            default:
                if (details->hb_qsv_info_avc != NULL) caps = details->hb_qsv_info_avc->capabilities;
                *direction   = 1;
                *granularity = 1.;
                *low         = (caps & HB_QSV_CAP_RATECONTROL_ICQ) ? 1. : 0.;
                *high        = 51.;
                break;
        }
    }
}

const char * hb_map_qsv_preset_name(const char * preset)
{
    if (preset == NULL)
    {
        return "medium";
    }

    if (strcmp(preset, "speed") == 0) {
      return "veryfast";
    } else if (strcmp(preset, "balanced") == 0) {
      return "medium";
    } else if (strcmp(preset, "quality") == 0) {
      return "veryslow";
    }

    return "medium";
}

int hb_qsv_param_default_async_depth()
{
    return hb_qsv_hardware_generation(hb_qsv_get_platform(hb_qsv_get_adapter_index())) >= QSV_G7 ? 6 : HB_QSV_ASYNC_DEPTH_DEFAULT;
}

int hb_qsv_param_default(hb_qsv_param_t *param, hb_qsv_info_t *info)
{
    if (param != NULL && info != NULL)
    {
        // introduced in API 1.6
        memset(&param->codingOption2, 0, sizeof(mfxExtCodingOption2));
        param->codingOption2.Header.BufferId = MFX_EXTBUFF_CODING_OPTION2;
        param->codingOption2.Header.BufferSz = sizeof(mfxExtCodingOption2);
        param->codingOption2.IntRefType      = 0;
        param->codingOption2.IntRefCycleSize = 2;
        param->codingOption2.IntRefQPDelta   = 0;
        param->codingOption2.MaxFrameSize    = 0;
        param->codingOption2.BitrateLimit    = MFX_CODINGOPTION_ON;
        param->codingOption2.MBBRC           = MFX_CODINGOPTION_OFF;
        param->codingOption2.ExtBRC          = MFX_CODINGOPTION_OFF;
        // introduced in API 1.7
        param->codingOption2.LookAheadDepth  = 40;
        param->codingOption2.Trellis         = MFX_TRELLIS_OFF;
        // introduced in API 1.8
        param->codingOption2.RepeatPPS       = MFX_CODINGOPTION_OFF;
        param->codingOption2.BRefType        = MFX_B_REF_UNKNOWN; // controlled via gop.b_pyramid
        param->codingOption2.AdaptiveI       = MFX_CODINGOPTION_OFF;
        param->codingOption2.AdaptiveB       = MFX_CODINGOPTION_OFF;
        param->codingOption2.LookAheadDS     = MFX_LOOKAHEAD_DS_OFF;
        param->codingOption2.NumMbPerSlice   = 0;
        // introduced in API 2.5
        param->hyperEncodeParam              = hb_triplet4key(hb_qsv_hyper_encode_modes, "off");

        // introduced in API 2.11
        memset(&param->av1ScreenContentToolsParam, 0, sizeof(mfxExtAV1ScreenContentTools));
        param->av1ScreenContentToolsParam.Header.BufferId = MFX_EXTBUFF_AV1_SCREEN_CONTENT_TOOLS;
        param->av1ScreenContentToolsParam.Header.BufferSz = sizeof(mfxExtAV1ScreenContentTools);
        param->av1ScreenContentToolsParam.IntraBlockCopy  = 0;
        param->av1ScreenContentToolsParam.Palette         = 0;

        // GOP & rate control
        param->gop.b_pyramid          =  1; // enabled by default (if supported)
        param->gop.gop_pic_size       = -1; // set automatically
        param->gop.gop_ref_dist       = -1; // set automatically
        param->gop.int_ref_cycle_size = -1; // set automatically
        param->rc.icq                 =  1; // enabled by default (if supported)
        param->rc.lookahead           =  0; // disabled by default
        param->rc.cqp_offsets[0]      =  0;
        param->rc.cqp_offsets[1]      =  2;
        param->rc.cqp_offsets[2]      =  4;
        param->rc.vbv_max_bitrate     =  0; // set automatically
        param->rc.vbv_buffer_size     =  0; // set automatically
        param->rc.vbv_buffer_init     = .0; // set automatically

        param->low_power              = 0;

#if defined(_WIN32) || defined(__MINGW32__)
        if (info->capabilities & HB_QSV_CAP_LOWPOWER_ENCODE)
        {
            param->low_power          = 1;
        }
#endif
    }
    else
    {
        hb_error("hb_qsv_param_default: invalid pointer(s)");
        return -1;
    }
    return 0;
}

hb_triplet_t* hb_triplet4value(hb_triplet_t *triplets, const int value)
{
    for (int i = 0; triplets[i].name != NULL; i++)
    {
        if (triplets[i].value == value)
        {
            return &triplets[i];
        }
    }
    return NULL;
}

hb_triplet_t* hb_triplet4name(hb_triplet_t *triplets, const char *name)
{
    for (int i = 0; triplets[i].name != NULL; i++)
    {
        if (!strcasecmp(triplets[i].name, name))
        {
            return &triplets[i];
        }
    }
    return NULL;
}

hb_triplet_t* hb_triplet4key(hb_triplet_t *triplets, const char *key)
{
    for (int i = 0; triplets[i].name != NULL; i++)
    {
        if (!strcasecmp(triplets[i].key, key))
        {
            return &triplets[i];
        }
    }
    return NULL;
}

const char* hb_qsv_codec_name(uint32_t codec_id)
{
    switch (codec_id)
    {
        case MFX_CODEC_AVC:
            return "H.264/AVC";

        case MFX_CODEC_HEVC:
            return "H.265/HEVC";

        case MFX_CODEC_AV1:
            return "AV1";

        default:
            return NULL;
    }
}

const char* hb_qsv_profile_name(uint32_t codec_id, uint16_t profile_id)
{
    hb_triplet_t *profile = NULL;
    switch (codec_id)
    {
        case MFX_CODEC_AVC:
            profile = hb_triplet4value(hb_qsv_h264_profiles, profile_id);
            break;

        case MFX_CODEC_HEVC:
            profile = hb_triplet4value(hb_qsv_h265_profiles, profile_id);
            break;

        case MFX_CODEC_AV1:
            profile = hb_triplet4value(hb_qsv_av1_profiles, profile_id);
            break;

        default:
            break;
    }
    return profile != NULL ? profile->name : NULL;
}

const char* hb_qsv_impl_get_name(int impl)
{
    switch (MFX_IMPL_BASETYPE(impl))
    {
        case MFX_IMPL_SOFTWARE:
            return "software";

        case MFX_IMPL_HARDWARE:
            return "hardware (1)";
        case MFX_IMPL_HARDWARE2:
            return "hardware (2)";
        case MFX_IMPL_HARDWARE3:
            return "hardware (3)";
        case MFX_IMPL_HARDWARE4:
            return "hardware (4)";
        case MFX_IMPL_HARDWARE_ANY:
            return "hardware (any)";

        case MFX_IMPL_AUTO:
            return "automatic";
        case MFX_IMPL_AUTO_ANY:
            return "automatic (any)";

        default:
            return NULL;
    }
}

int hb_qsv_impl_get_num(int impl)
{
    switch (MFX_IMPL_BASETYPE(impl))
    {
        case MFX_IMPL_HARDWARE:
            return 0;
        case MFX_IMPL_HARDWARE2:
            return 1;
        case MFX_IMPL_HARDWARE3:
            return 2;
        case MFX_IMPL_HARDWARE4:
            return 3;
        case MFX_IMPL_SOFTWARE:
        case MFX_IMPL_AUTO:
        case MFX_IMPL_AUTO_ANY:
        case MFX_IMPL_HARDWARE_ANY:
        default:
            return -1;
    }
}

const char* hb_qsv_impl_get_via_name(int impl)
{
    if      ((impl & 0xF00) == MFX_IMPL_VIA_VAAPI)
        return "via VAAPI";
    else if ((impl & 0xF00) == MFX_IMPL_VIA_D3D11)
        return "via D3D11";
    else if ((impl & 0xF00) == MFX_IMPL_VIA_D3D9)
        return "via D3D9";
    else if ((impl & 0xF00) == MFX_IMPL_VIA_ANY)
        return "via ANY";
    else return NULL;
}

int hb_qsv_get_platform(int adapter_index)
{
    for (int i = 0; i < hb_list_count(g_qsv_adapters_details_list); i++)
    {
        const hb_qsv_adapter_details_t *details = hb_list_item(g_qsv_adapters_details_list, i);
        // find DirectX adapter with given index in list of QSV adapters
        if (details && (details->index == adapter_index))
        {
            return qsv_map_mfx_platform_codename(details->platform.CodeName);
        }
    }
    return HB_CPU_PLATFORM_UNSPECIFIED;
}

int hb_qsv_param_parse_dx_index(hb_job_t *job, const int dx_index)
{
    for (int i = 0; i < hb_list_count(g_qsv_adapters_details_list); i++)
    {
        const hb_qsv_adapter_details_t *details = hb_list_item(g_qsv_adapters_details_list, i);
        // find DirectX adapter with given index in list of QSV adapters
        if (details && (details->index == dx_index))
        {
            job->hw_device_index = details->index;
            hb_log("qsv: %s qsv adapter with index %u has been selected", hb_qsv_get_adapter_type(details), details->index);
            hb_qsv_set_adapter_index(details->index);
            return 0;
        }
    }
    job->hw_device_index = hb_qsv_get_adapter_index();
    hb_log("qsv: default qsv adapter has been selected");
    return -1;
}

hb_qsv_context_t * hb_qsv_context_init()
{
    if (!hb_qsv_available())
    {
        return 0;
    }
  
    hb_qsv_context_t *ctx = av_mallocz(sizeof(hb_qsv_context_t));
    if (!ctx)
    {
        hb_error("hb_qsv_context_init: qsv ctx alloc failed");
        return NULL;
    }
    return ctx;
}

hb_qsv_context_t * hb_qsv_context_dup(const hb_qsv_context_t *src)
{
    if (src == NULL)
    {
        return NULL;
    }

    hb_qsv_context_t *ctx = hb_qsv_context_init();
    if (ctx)
    {
        memcpy(ctx, src, sizeof(hb_qsv_context_t));
    }
    return ctx;
}

void hb_qsv_context_close(hb_qsv_context_t **_ctx)
{
    hb_qsv_context_t *ctx = *_ctx;
    if (ctx == NULL)
    {
        return;
    }

    av_freep(_ctx);

    // restore adapter index after user preferences
    g_adapter_index = hb_qsv_get_default_adapter_index();
}

static void * find_decoder(int codec_param)
{
    const char *codec_name = hb_qsv_decode_get_codec_name(codec_param);
    return (void *)avcodec_find_decoder_by_name(codec_name);
}

static const int qsv_encoders[] =
{
    HB_VCODEC_FFMPEG_QSV_H264,
    HB_VCODEC_FFMPEG_QSV_H265,
    HB_VCODEC_FFMPEG_QSV_H265_10BIT,
    HB_VCODEC_FFMPEG_QSV_AV1,
    HB_VCODEC_FFMPEG_QSV_AV1_10BIT,
    HB_VCODEC_INVALID
};

hb_hwaccel_t hb_hwaccel_qsv =
{
    .id           = HB_DECODE_QSV,
    .name         = "qsv",
    .encoders     = qsv_encoders,
    .type         = AV_HWDEVICE_TYPE_QSV,
    .hw_pix_fmt   = AV_PIX_FMT_QSV,
    .can_filter   = are_filters_supported,
    .find_decoder = find_decoder,
    .caps         = HB_HWACCEL_CAP_ROTATE
};

#else // HB_PROJECT_FEATURE_QSV

int hb_qsv_available()
{
    return -1;
}

#endif // HB_PROJECT_FEATURE_QSV
