#include "sync/av_sync.h"

#include <assert.h>
#include <stdint.h>

int main(void)
{
    pidvd_av_sync_t *sync = pidvd_av_sync_new();
    assert(sync);

    pidvd_av_sync_reset(sync, 7);
    assert(!pidvd_av_sync_wait_audio_ready(sync, 7, 1));
    pidvd_av_sync_audio_ready(sync, 7, true);
    assert(pidvd_av_sync_wait_audio_ready(sync, 7, 1));

    const int64_t t0 = INT64_C(1000000000);
    pidvd_av_sync_video_presented(sync, 7, 90000, t0);
    assert(pidvd_av_sync_video_is_started(sync, 7));

    int64_t error = 0;
    int correction = pidvd_av_sync_audio_correction(
        sync, 7, 90090, t0, &error);
    assert(error == 90);
    assert(correction > 0); /* audio ahead -> stretch it */

    pidvd_av_sync_reset(sync, 8);
    assert(!pidvd_av_sync_video_is_started(sync, 8));
    pidvd_av_sync_video_presented(sync, 7, 180000, t0 + 1000000000);
    correction = pidvd_av_sync_audio_correction(
        sync, 8, 180000, t0 + 1000000000, &error);
    assert(correction == 0); /* stale epoch cannot influence the new one */

    pidvd_av_sync_video_presented(sync, 8, 180000, t0 + 1000000000);
    correction = pidvd_av_sync_audio_correction(
        sync, 8, 179910, t0 + 1000000000, &error);
    assert(error == -90);
    assert(correction < 0); /* audio behind -> shorten it */

    pidvd_av_sync_free(sync);
    return 0;
}
