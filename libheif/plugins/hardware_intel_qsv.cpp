#include "hardware_intel_qsv.h"
#include <shared_mutex>

static mfxLoader loader = NULL;
mfxSession intel_qsv_session = NULL;
int init_count = 0;

static bool init_intel_qsv(uint32_t codecId)
{
    // Initialize session
    if (!loader)
        loader = MFXLoad();
    if (!loader)
        return false;
}

static void deinit_intel_qsv()
{
    if (session) {
        if (video_decode_initialized) {
            MFXVideoDECODE_Close(session);
            video_decode_initialized = false;
        }
        if (!video_encode_initialized) {
            MFXClose(session);
            session = NULL;
        }
    }
    if (loader && !session) {
        MFXUnload(loader);
        loader = NULL;
    }
}