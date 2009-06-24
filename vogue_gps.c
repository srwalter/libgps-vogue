
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include "gps.h"
#include "vogue_gps.h"

#define VOGUE_GPS_DEVICE "/dev/vogue_gps"

static GpsCallbacks vogue_callbacks;
static pthread_t gps_thread;
int gps_fd;

static pthread_mutex_t thread_mutex;
static int thread_running;
static pthread_cond_t thread_wq;
static int fix_freq = 60000;

static void send_status (GpsStatusValue sv)
{
    GpsStatus status;

    status.status = sv;
    vogue_callbacks.status_cb(&status);
}

static void send_signal_data (struct gps_state data)
{
    GpsSvStatus sv_info;
    int i;

    send_status(GPS_STATUS_SESSION_BEGIN);

    sv_info.num_svs = 0;
    for (i=0; i<MAX_SATELLITES; i++) {
        if (!data.sat_state[i].sat_no)
            break;

        sv_info.num_svs++;
        sv_info.sv_list[i].prn = data.sat_state[i].sat_no;
        sv_info.sv_list[i].snr
            = (double)data.sat_state[i].signal_strength / 256.0;
    }

    vogue_callbacks.sv_status_cb(&sv_info);
}

static void send_position_data (struct gps_state data)
{
    static int32_t last_lat, last_lng;
    GpsLocation location;
    char cmd[256];

    /* If the position hasn't changed, the kernel was probably just alerting us
     * to new signal data */
    if (data.lat == last_lat && data.lng == last_lng)
        return;

    last_lat = data.lat;
    last_lng = data.lng;

    memset(&location, 0, sizeof(location));
    location.flags |= GPS_LOCATION_HAS_LAT_LONG;
    location.latitude = ((double)data.lat) / 180000.0;
    location.latitude /= 1.035629;
    location.longitude = ((double)data.lng) / 180000.0;
    location.longitude /= 1.035629; /* correction factor? */

    system("echo lock >> /tmp/gps");
    snprintf(cmd, 256, "echo coords %10g %10g >> /tmp/gps", location.latitude,
            location.longitude);
    snprintf(cmd, 256, "echo coords %d %d >> /tmp/gps", last_lat, last_lng);
    system(cmd);

    vogue_callbacks.location_cb(&location);
}

static void *vogue_gps_thread (void *arg)
{
    (void)arg;

    system("echo thread 1 >> /tmp/gps");

    /* Wait until we're signalled to start */
    pthread_mutex_lock(&thread_mutex);
restart:
    while (!thread_running) {
        pthread_cond_wait(&thread_wq, &thread_mutex);
    }
    system("echo thread 2 >> /tmp/gps");

    /* 2 means we should quit */
    if (thread_running == 2) {
        pthread_mutex_unlock(&thread_mutex);
        return NULL;
    }
    pthread_mutex_unlock(&thread_mutex);

    system("echo thread 3 >> /tmp/gps");

    for (;;) {
        struct gps_state data;
        struct timeval select_tv, before_tv, after_tv;
        fd_set set, empty;
        int rc;
        int msec_to_next_fix = fix_freq;

        do {
            int msec_elapsed;

            select_tv.tv_sec = msec_to_next_fix / 1000;
            select_tv.tv_usec = (msec_to_next_fix % 1000) / 1000;

            FD_ZERO(&set);
            FD_ZERO(&empty);
            FD_SET(gps_fd, &set);

            gettimeofday(&before_tv, NULL);
            rc = select(gps_fd+1, &set, &empty, &empty, &select_tv);
            gettimeofday(&after_tv, NULL);

            /* If we got woken up early by a signal, we want to decrease our
            next timeout by the appropriate amount so we don't keep the
            framework waiting for a fix */
            msec_elapsed = (after_tv.tv_sec - before_tv.tv_sec) * 1000;
            msec_elapsed += (after_tv.tv_usec - before_tv.tv_usec) / 1000;
            msec_to_next_fix -= msec_elapsed;

            pthread_mutex_lock(&thread_mutex);
            if (thread_running != 1)
                goto restart;
            pthread_mutex_unlock(&thread_mutex);
        } while (rc < 0 && errno == EINTR);

        if (rc < 0) {
            system("echo select error >> /tmp/gps");
            perror("select");
            continue;
        }

        if (!rc) {
            /* fix_freq has elapsed with no data from the GPS.  better tell it
             * explicitly that we want a new fix */
            system("echo select timeout >> /tmp/gps");
            ioctl(gps_fd, VGPS_IOC_NEW_FIX);
            continue;
        }

        system("echo thread 6 >> /tmp/gps");

        do {
            rc = read(gps_fd, &data, sizeof(struct gps_state));
        } while (rc < 0 && errno == EINTR);

        if (rc < 0) {
            system("echo read error >> /tmp/gps");
            perror("read");
        }

        system("echo thread 7 >> /tmp/gps");

        send_signal_data(data);
        send_position_data(data);
    }
    
    return NULL;
}

static int vogue_gps_init (GpsCallbacks *callbacks)
{
    pthread_mutexattr_t attr;

    system("echo init >> /tmp/gps");
    memcpy(&vogue_callbacks, callbacks, sizeof(GpsCallbacks));
    gps_fd = open(VOGUE_GPS_DEVICE, O_RDWR);
    if (gps_fd < 0) {
        perror("open");
        return -errno;
    }

    pthread_mutexattr_init(&attr);
    pthread_mutex_init(&thread_mutex, &attr);
    pthread_cond_init(&thread_wq, NULL);
    pthread_create(&gps_thread, NULL, vogue_gps_thread, NULL);

    return 0;
}

static int vogue_gps_start (void)
{
    int rc;

    system("echo start >> /tmp/gps");
    if (!thread_running) {
        rc = ioctl(gps_fd, VGPS_IOC_ENABLE);
        if (rc < 0)
            return rc;

        send_status(GPS_STATUS_ENGINE_ON);

        system("echo start 1 >> /tmp/gps");
        pthread_mutex_lock(&thread_mutex);
        thread_running = 1;
        pthread_cond_broadcast(&thread_wq);
        pthread_mutex_unlock(&thread_mutex);
    }
    return 0;
}

static int vogue_gps_stop (void)
{
    if (thread_running) {
        pthread_mutex_lock(&thread_mutex);
        thread_running = 0;
        pthread_mutex_unlock(&thread_mutex);

        ioctl(gps_fd, VGPS_IOC_DISABLE);
        send_status(GPS_STATUS_ENGINE_OFF);
    }
    return 0;
}

static void vogue_gps_set_freq (int freq)
{
    if (freq < 10000)
        freq = 10000;
    fix_freq = freq;
}

static void vogue_gps_cleanup (void)
{
    vogue_gps_stop();
    pthread_mutex_lock(&thread_mutex);
    thread_running = 2;
    pthread_cond_broadcast(&thread_wq);
    pthread_mutex_unlock(&thread_mutex);
    close(gps_fd);
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
