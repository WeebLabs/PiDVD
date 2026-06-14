/* drmprops — dump the Composite connector's modes (with timings) and all
 * its properties (enum names+values+current). Used to ground the VEC
 * TV-norm switching: tells us the norm property's exact name and the
 * NTSC/PAL enum values so the player can set it correctly. Dev/diagnostic
 * tool; cross-compiled for the target. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

int main(void)
{
    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) { perror("open card0"); return 1; }
    drmModeRes *r = drmModeGetResources(fd);
    if (!r) { perror("resources"); return 1; }

    for (int i = 0; i < r->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, r->connectors[i]);
        if (!c) continue;
        if (c->connector_type != DRM_MODE_CONNECTOR_Composite) {
            drmModeFreeConnector(c);
            continue;
        }
        printf("Composite connector id=%u status=%d encoder=%u modes=%d\n",
               c->connector_id, c->connection, c->encoder_id, c->count_modes);
        for (int m = 0; m < c->count_modes; m++) {
            drmModeModeInfo *mi = &c->modes[m];
            printf("  MODE %-10s %dx%d clk=%d htot=%d vtot=%d vref=%d "
                   "flags=0x%x\n", mi->name, mi->hdisplay, mi->vdisplay,
                   mi->clock, mi->htotal, mi->vtotal, mi->vrefresh,
                   mi->flags);
        }
        drmModeObjectProperties *props = drmModeObjectGetProperties(
            fd, c->connector_id, DRM_MODE_OBJECT_CONNECTOR);
        if (props) {
            for (uint32_t p = 0; p < props->count_props; p++) {
                drmModePropertyRes *pr =
                    drmModeGetProperty(fd, props->props[p]);
                if (!pr) continue;
                printf("  PROP '%s' id=%u flags=0x%x cur=%llu", pr->name,
                       pr->prop_id, pr->flags,
                       (unsigned long long)props->prop_values[p]);
                if (pr->flags & DRM_MODE_PROP_ENUM) {
                    printf("  enums:");
                    for (int e = 0; e < pr->count_enums; e++)
                        printf(" [%s=%llu]", pr->enums[e].name,
                               (unsigned long long)pr->enums[e].value);
                }
                printf("\n");
                drmModeFreeProperty(pr);
            }
            drmModeFreeObjectProperties(props);
        }
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(r);
    close(fd);
    return 0;
}
