
#include "gps.h"

static GpsCallbacks vogue_callbacks;

static int vogue_gps_init (GpsCallbacks *callbacks)
{
    memcpy(&vogue_callbacks, callbacks, sizeof(GpsCallbacks));
    return 0;
}

static int vogue_gps_start (void)
{
    system("echo 1 > /sys/class/gps/enable");
    return 0;
}

static int vogue_gps_stop (void)
{
    system("echo 0 > /sys/class/gps/enable");
    return 0;
}

static void vogue_gps_set_freq (void)
{
}

static void vogue_gps_cleanup (void)
{
}

static int vogue_gps_inject (GpsUtcTime time, int64_t time_ref, int uncert)
{
    (void)time;
    (void)time_ref;
    (void)uncert;
    return 0;
}

static void vogue_gps_aids (GpsAidingData flags)
{
    (void)flags;
}

static int vogue_gps_set_mode (GpsPositionMode mode, int freq)
{
    return 0;
}

static const void * vogue_gps_get_extension (const char *name)
{
    (void)name;
    return NULL;
}

GpsInterface vogue_gps_iface = {
    .init       = vogue_gps_init,
    .start      = vogue_gps_start,
    .stop       = vogue_gps_stop,
    .set_fix_frequency  = vogue_gps_set_freq,
    .cleanup    = vogue_gps_cleanup,
    .inject_time        = vogue_gps_inject,
    .delete_aiding_data = vogue_gps_aids,
    .set_position_mode  = vogue_gps_set_mode,
    .get_extension      = vogue_gps_get_extension,
};

const GpsInterface* gps_get_hardware_interface()
{
    system("touch /tmp/ghi");
    return &vogue_gps_iface;
}
