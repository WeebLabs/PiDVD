#include "core/disc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_read.h>
#include <dvdread/ifo_types.h>

struct pidvd_disc {
    dvd_reader_t *dvd;
    char volume_id[33];
    uint8_t region_mask;
    int n_titles;
    pidvd_title_t *titles;
};

static int bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0f); }

static double dvd_time_to_seconds(const dvd_time_t *t)
{
    /* frame_u: bits 7-6 = frame rate code (1 = 25 fps, 3 = 29.97 fps),
     * bits 5-0 = BCD frame count */
    double fps = ((t->frame_u >> 6) & 3) == 1 ? 25.0 : 30000.0 / 1001.0;
    return bcd(t->hour) * 3600.0 + bcd(t->minute) * 60.0 + bcd(t->second)
         + bcd(t->frame_u & 0x3f) / fps;
}

static const char *audio_format_name(int code)
{
    switch (code) {
    case 0: return "AC-3";
    case 2: return "MPEG-1";
    case 3: return "MPEG-2";
    case 4: return "LPCM";
    case 6: return "DTS";
    case 7: return "SDDS";
    default: return "?";
    }
}

static void lang_str(uint16_t code, char out[4])
{
    if (code >= 0x0100) {
        out[0] = (char)(code >> 8);
        out[1] = (char)(code & 0xff);
        out[2] = '\0';
    } else {
        strcpy(out, "--");
    }
}

static void fill_video(pidvd_title_t *t, const video_attr_t *v,
                       pidvd_standard_t std)
{
    t->standard = std;
    t->aspect = (v->display_aspect_ratio == 3) ? PIDVD_ASPECT_16_9
                                               : PIDVD_ASPECT_4_3;
    int full_h = (std == PIDVD_STD_PAL) ? 576 : 480;
    switch (v->picture_size) {
    case 0: t->width = 720; t->height = full_h; break;
    case 1: t->width = 704; t->height = full_h; break;
    case 2: t->width = 352; t->height = full_h; break;
    case 3: t->width = 352; t->height = full_h / 2; break;
    default: t->width = 720; t->height = full_h; break;
    }
    t->letterboxed = v->letterboxed != 0;
}

pidvd_disc_t *pidvd_disc_open(const char *path)
{
    dvd_reader_t *dvd = DVDOpen(path);
    if (!dvd) {
        fprintf(stderr, "pidvd: cannot open '%s' as a DVD image\n", path);
        return NULL;
    }

    ifo_handle_t *vmg = ifoOpen(dvd, 0);
    if (!vmg || !vmg->tt_srpt || !vmg->vmgi_mat) {
        fprintf(stderr, "pidvd: '%s' has no readable VMG IFO — not a "
                        "DVD-Video image, or still CSS-encrypted?\n", path);
        if (vmg) ifoClose(vmg);
        DVDClose(dvd);
        return NULL;
    }

    pidvd_disc_t *d = calloc(1, sizeof(*d));
    d->dvd = dvd;
    if (DVDUDFVolumeInfo(dvd, d->volume_id, sizeof(d->volume_id), NULL, 0) != 0
        && DVDISOVolumeInfo(dvd, d->volume_id, sizeof(d->volume_id), NULL, 0) != 0)
        strcpy(d->volume_id, "(unknown)");

    /* vmg_category bits 16-23 are the region *prohibition* mask
     * (set bit = playback prohibited in that region); invert to the
     * friendlier allowed-mask. */
    d->region_mask = (uint8_t)~(vmg->vmgi_mat->vmg_category >> 16);

    tt_srpt_t *tt = vmg->tt_srpt;
    d->n_titles = tt->nr_of_srpts;
    d->titles = calloc((size_t)d->n_titles, sizeof(pidvd_title_t));

    /* Cache one IFO handle per VTS — titles often share a title set. */
    int max_vts = vmg->vmgi_mat->vmg_nr_of_title_sets;
    ifo_handle_t **vts_ifo = calloc((size_t)max_vts + 1, sizeof(*vts_ifo));

    for (int i = 0; i < d->n_titles; i++) {
        const title_info_t *ti = &tt->title[i];
        pidvd_title_t *t = &d->titles[i];
        t->title_nr = i + 1;
        t->vts_nr   = ti->title_set_nr;
        t->chapters = ti->nr_of_ptts;
        t->angles   = ti->nr_of_angles;

        if (ti->title_set_nr < 1 || ti->title_set_nr > max_vts)
            continue;
        if (!vts_ifo[ti->title_set_nr])
            vts_ifo[ti->title_set_nr] = ifoOpen(dvd, ti->title_set_nr);
        ifo_handle_t *vts = vts_ifo[ti->title_set_nr];
        if (!vts || !vts->vtsi_mat)
            continue;

        const video_attr_t *v = &vts->vtsi_mat->vts_video_attr;
        fill_video(t, v, v->video_format == 1 ? PIDVD_STD_PAL
                                              : PIDVD_STD_NTSC);

        /* The title's first PGC: playback time, plus which of the VTS's
         * declared streams actually exist — IFOs often declare 8/32
         * slots and the PGC control words mask the real ones. */
        const pgc_t *pgc = NULL;
        if (vts->vts_ptt_srpt && vts->vts_pgcit
            && ti->vts_ttn >= 1
            && ti->vts_ttn <= vts->vts_ptt_srpt->nr_of_srpts) {
            const ttu_t *ttu = &vts->vts_ptt_srpt->title[ti->vts_ttn - 1];
            if (ttu->nr_of_ptts > 0) {
                int pgcn = ttu->ptt[0].pgcn;
                if (pgcn >= 1 && pgcn <= vts->vts_pgcit->nr_of_pgci_srp)
                    pgc = vts->vts_pgcit->pgci_srp[pgcn - 1].pgc;
            }
        }
        if (pgc)
            t->seconds = dvd_time_to_seconds(&pgc->playback_time);

        int n_audio = vts->vtsi_mat->nr_of_vts_audio_streams;
        if (n_audio > PIDVD_MAX_AUDIO) n_audio = PIDVD_MAX_AUDIO;
        for (int a = 0; a < n_audio; a++) {
            if (pgc && !(pgc->audio_control[a] & 0x8000))
                continue;
            const audio_attr_t *aa = &vts->vtsi_mat->vts_audio_attr[a];
            pidvd_audio_stream_t *as = &t->audio[t->n_audio++];
            snprintf(as->format, sizeof(as->format), "%s",
                     audio_format_name(aa->audio_format));
            lang_str(aa->lang_code, as->lang);
            as->channels = (uint8_t)(aa->channels + 1);
        }

        int n_subp = vts->vtsi_mat->nr_of_vts_subp_streams;
        if (n_subp > PIDVD_MAX_SUBP) n_subp = PIDVD_MAX_SUBP;
        for (int s = 0; s < n_subp; s++) {
            if (pgc && !(pgc->subp_control[s] & 0x80000000u))
                continue;
            /* subp_control packs the physical PES substream id per display
             * mode: bits 28-24 = 4:3, 20-16 = wide, 12-8 = letterbox,
             * 4-0 = pan-scan. Pick the field matching this title's aspect —
             * that is the substream the demux actually carries on screen.
             * No PGC (degenerate): assume physical == logical. */
            pidvd_subp_stream_t *sp = &t->subp[t->n_subp++];
            lang_str(vts->vtsi_mat->vts_subp_attr[s].lang_code, sp->lang);
            int phys = s;
            if (pgc) {
                uint32_t c = pgc->subp_control[s];
                phys = (t->aspect == PIDVD_ASPECT_16_9)
                     ? (int)((c >> 16) & 0x1f)
                     : (int)((c >> 24) & 0x1f);
            }
            sp->phys = (uint8_t)phys;
        }
    }

    for (int v = 1; v <= max_vts; v++)
        if (vts_ifo[v]) ifoClose(vts_ifo[v]);
    free(vts_ifo);
    ifoClose(vmg);
    return d;
}

void pidvd_disc_close(pidvd_disc_t *d)
{
    if (!d) return;
    free(d->titles);
    DVDClose(d->dvd);
    free(d);
}

const char *pidvd_disc_volume_id(const pidvd_disc_t *d) { return d->volume_id; }
uint8_t pidvd_disc_region_mask(const pidvd_disc_t *d) { return d->region_mask; }
void *pidvd_disc_reader(const pidvd_disc_t *d) { return d->dvd; }
int pidvd_disc_title_count(const pidvd_disc_t *d) { return d->n_titles; }

const pidvd_title_t *pidvd_disc_title(const pidvd_disc_t *d, int idx)
{
    if (idx < 0 || idx >= d->n_titles) return NULL;
    return &d->titles[idx];
}

pidvd_standard_t pidvd_disc_standard(const pidvd_disc_t *d, bool *mixed)
{
    /* Output mode follows the longest title (the feature), which is what
     * autoplay starts. Mixed-standard discs are flagged so the presenter
     * knows a mode switch may be needed at VTS boundaries. */
    pidvd_standard_t best = PIDVD_STD_NTSC;
    double best_len = -1.0;
    bool any_mix = false;
    for (int i = 0; i < d->n_titles; i++) {
        if (d->titles[i].seconds > best_len) {
            best_len = d->titles[i].seconds;
            best = d->titles[i].standard;
        }
        if (d->titles[i].standard != d->titles[0].standard)
            any_mix = true;
    }
    if (mixed) *mixed = any_mix;
    return best;
}

const char *pidvd_standard_name(pidvd_standard_t s)
{
    return s == PIDVD_STD_PAL ? "PAL 576i50" : "NTSC 480i59.94";
}
