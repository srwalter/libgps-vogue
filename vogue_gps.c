
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/select.h>
#include "gps.h"

static GpsCallbacks vogue_callbacks;
static pthread_t gps_thread;
static int thread_running;

static void writesys(char *name, char *val) {
	FILE *fout;
	fout=fopen(name,"w");
	if(!fout) return;
	fprintf(fout,val);
	fclose(fout);
}

static void *vogue_gps_thread (void *arg)
{
    (void)arg;

    for (;;) {
        struct timeval tv;
        GpsLocation location;
        char buf[256];
        FILE *pos;
        uint32_t lat, lng;

        tv.tv_sec = 5;
        select(0, NULL, NULL, NULL, &tv);
        if (!thread_running)
            continue;

        system("echo thread >> /tmp/gps");

        writesys("/sys/class/gps/enable", "2");

        pos = fopen("/sys/class/gps/position", "r");
        fscanf(pos, "%d %d", &lat, &lng);
        fclose(pos);

        if (lat == 0 && lng == 0)
            continue;

        system("echo lock > /tmp/gps");

        memset(&location, 0, sizeof(location));
        location.flags |= GPS_LOCATION_HAS_LAT_LONG;
        location.latitude = (double)lat / (double)180000;
        location.longitude = (double)lng / (double)180000;

        vogue_callbacks.location_cb(&location);
    }
    
    return NULL;
}

static int vogue_gps_init (GpsCallbacks *callbacks)
{
    system("echo init >> /tmp/gps");
    memcpy(&vogue_callbacks, callbacks, sizeof(GpsCallbacks));
    pthread_create(&gps_thread, NULL, vogue_gps_thread, NULL);
    return 0;
}

static int vogue_gps_start (void)
{
    writesys("/sys/class/gps/enable", "1");
    system("echo start >> /tmp/gps");
    thread_running = 1;
    return 0;
}

static int vogue_gps_stop (void)
{
    if (thread_running) {
        thread_running = 0;
    }
    writesys("/sys/class/gps/enable", "0");
    return 0;
}

static void vogue_gps_set_freq (int freq)
{
    (void)freq;
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
    .init               = vogue_gps_init,
    .start              = vogue_gps_start,
    .stop               = vogue_gps_stop,
    .set_fix_frequency  = vogue_gps_set_freq,
    .cleanup            = vogue_gps_cleanup,
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
