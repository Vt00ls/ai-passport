// main/app_dashboard/dashboard_logos.c
// Maps a provider id from the frame's `pv` field to its logo. The pixel data lives in
// the generated dashboard_logos_pi.c / dashboard_logos_dsh.c.
#include "app_dashboard/dashboard_logos.h"

#include <string.h>

const lv_image_dsc_t *dashboard_provider_logo(const char *provider)
{
    if (!provider) return NULL;
    if (strcmp(provider, "pi") == 0) return &dashboard_logo_pi;
    if (strcmp(provider, "dsh") == 0) return &dashboard_logo_dsh;
    // An unknown provider shows no logo rather than a misleading one.
    return NULL;
}
