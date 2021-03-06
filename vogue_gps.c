
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <math.h>
#include "gps.h"
#include "vogue_gps.h"

#define VOGUE_GPS_DEVICE "/dev/vogue_gps"

#define DEBUG

#ifdef DEBUG
# define GPS_LOG system
#else
# define GPS_LOG(...) do { } while (0);
#endif

static GpsCallbacks vogue_callbacks;
static pthread_t gps_thread;
int gps_fd;
double correction_factor;

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

    sv_info.num_svs = 0;
    for (i=0; i<MAX_SATELLITES; i++) {
        if (!data.sat_state[i].sat_no)
            break;

        sv_info.num_svs++;
        sv_info.sv_list[i].prn = data.sat_state[i].sat_no;
        sv_info.sv_list[i].snr
            = data.sat_state[i].signal_strength;
    }

    vogue_callbacks.sv_status_cb(&sv_info);
}

static int send_position_data (struct gps_state data)
{
    static uint32_t last_fix;
    static double last_lat, last_lon;
    float position_delta;
    uint32_t time_delta;
    GpsLocation location;
    char cmd[256];

    /* If the fix time hasn't changed, the kernel was probably just
     * alerting us to new signal data */
    if (data.time == last_fix)
        return 0;

    time_delta = data.time - last_fix;
    last_fix = data.time;

    memset(&location, 0, sizeof(location));
    location.flags |= GPS_LOCATION_HAS_LAT_LONG;
    location.flags |= GPS_LOCATION_HAS_ACCURACY;
    location.latitude = ((double)data.lat) / 180000.0;
    location.latitude /= correction_factor;
    location.longitude = ((double)data.lng) / 180000.0;
    location.longitude /= correction_factor;

    /* Compute speed and bearing */
    if (last_lat && last_lon) {
        position_delta = location.latitude - last_lat;
        position_delta *= position_delta;
        position_delta += (location.longitude - last_lon) *
            (location.longitude - last_lon);
        position_delta = sqrt(position_delta);
        /* Nautical miles per second */
        location.speed = 60 * position_delta / time_delta;
        /* Convert to meters per second (assuming near sea level) */
        location.speed *= 1853.0;
        location.flags |= GPS_LOCATION_HAS_SPEED;

        if (location.longitude != last_lon) {
            location.bearing = fabs(location.latitude - last_lat) / 
                fabs(location.longitude - last_lon);
            location.bearing = atan(location.bearing) * 360 / (6.282);
        } else {
            location.bearing = 0.0;
        }

        if ((location.latitude - last_lat) < 0) {
            if ((location.longitude - last_lon) < 0) {
                location.bearing += 180.0;
            } else {
                location.bearing += 90.0;
            }
        } else if ((location.longitude - last_lon) < 0) {
            location.bearing += 270.0;
        }
        if (location.bearing >= 360.0)
            location.bearing -= 360.0;
        location.flags |= GPS_LOCATION_HAS_BEARING;
#ifdef DEBUG
        snprintf(cmd, 256, "echo speed %10g bearing %10g >> /sdcard/gps",
                 location.speed, location.bearing);
        system(cmd);
#endif
    }

    last_lat = location.latitude;
    last_lon = location.longitude;
    location.accuracy = 3.0;
    location.timestamp = data.time;

    GPS_LOG("echo lock >> /sdcard/gps");
#ifdef DEBUG
    snprintf(cmd, 256, "echo coords %10g %10g >> /sdcard/gps", location.latitude,
            location.longitude);
    system(cmd);
#endif

    vogue_callbacks.location_cb(&location);
    return 1;
}

static int get_next_fix (void)
{
    int next_fix = fix_freq - 1000;
    if (next_fix < 2000)
        next_fix = 2000;
    return next_fix;
}

static void *vogue_gps_thread (void *arg)
{
    (void)arg;
    int msec_to_next_fix = get_next_fix();
#ifdef DEBUG
    char cmd[256];

    snprintf(cmd, sizeof(cmd), "echo thread pid %d >> /sdcard/gps", getpid());
    system(cmd);
#endif

    GPS_LOG("echo thread 1 >> /sdcard/gps");

    /* Wait until we're signalled to start */
    pthread_mutex_lock(&thread_mutex);
restart:
    GPS_LOG("echo thread 1a >> /sdcard/gps");
    while (!thread_running) {
        pthread_cond_wait(&thread_wq, &thread_mutex);
    }
    GPS_LOG("echo thread 2 >> /sdcard/gps");

    /* 2 means we should quit */
    if (thread_running == 2) {
        pthread_mutex_unlock(&thread_mutex);
        return NULL;
    }
    pthread_mutex_unlock(&thread_mutex);

    GPS_LOG("echo thread 3 >> /sdcard/gps");

    for (;;) {
        struct gps_state data;
        struct timeval select_tv, before_tv, after_tv;
        fd_set set, empty;
        int rc;

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

            if (msec_to_next_fix < 0)
                msec_to_next_fix = 0;

            pthread_mutex_lock(&thread_mutex);
            if (thread_running != 1)
                goto restart;
            pthread_mutex_unlock(&thread_mutex);
        } while (rc < 0 && errno == EINTR);

        if (rc < 0) {
            GPS_LOG("echo select error >> /sdcard/gps");
            perror("select");
            continue;
        }

        if (!rc) {
            /* fix_freq has elapsed with no data from the GPS.  better tell it
             * explicitly that we want a new fix */
            GPS_LOG("echo select timeout >> /sdcard/gps");
            ioctl(gps_fd, VGPS_IOC_NEW_FIX);
            msec_to_next_fix = get_next_fix();
            continue;
        }

        GPS_LOG("echo thread 6 >> /sdcard/gps");

        do {
            rc = read(gps_fd, &data, sizeof(struct gps_state));
        } while (rc < 0 && errno == EINTR);

        if (rc < 0) {
            GPS_LOG("echo read error >> /sdcard/gps");
            perror("read");
        }

        GPS_LOG("echo thread 7 >> /sdcard/gps");

        send_signal_data(data);
        rc = send_position_data(data);

        if (rc) {
            /* We sent new position data, so reset the timer */
            msec_to_next_fix = get_next_fix();
        }

        /* fix frequency of zero means "one-shot mode" */
        if (!fix_freq) {
            thread_running=0;
            goto restart;
        }
    }
    
    return NULL;
}

static int need_init=1;

static int core_init()
{
    need_init = 0;
    int rc;
    struct gps_info info;
    pthread_mutexattr_t attr;

    gps_fd = open(VOGUE_GPS_DEVICE, O_RDWR);
    if (gps_fd < 0) {
        perror("open");
        return -errno;
    }

    GPS_LOG("echo ioctl >> /sdcard/gps");
    rc = ioctl(gps_fd, VGPS_IOC_INFO, &info);
    if (rc < 0) {
        perror("ioctl");
        return -errno;
    }

    GPS_LOG("echo version >> /sdcard/gps");
    if (info.version != GPS_VERSION) {
        fprintf(stderr, "wrong GPS version");
        return -1;
    }
    correction_factor = info.correction_factor;

    pthread_mutexattr_init(&attr);
    pthread_mutex_init(&thread_mutex, &attr);
    pthread_cond_init(&thread_wq, NULL);
    thread_running = 0;
    pthread_create(&gps_thread, NULL, vogue_gps_thread, NULL);

    return 0;
}

static int vogue_gps_init (GpsCallbacks *callbacks)
{
    int rc;
#ifdef DEBUG
    char cmd[256];
#endif

    if (need_init) {
        rc = core_init();
        if (rc)
            return rc;
    }

    GPS_LOG("echo init >> /sdcard/gps");
#ifdef DEBUG
    snprintf(cmd, sizeof(cmd), "echo %d >> /sdcard/gps", getpid());
    system(cmd);
#endif
    memcpy(&vogue_callbacks, callbacks, sizeof(GpsCallbacks));

    GPS_LOG("echo done >> /sdcard/gps");
    return 0;
}

static void start_thread(void)
{
    if (!thread_running) {
        GPS_LOG("echo thread not running >> /sdcard/gps");
        pthread_mutex_lock(&thread_mutex);
        thread_running = 1;
        pthread_cond_broadcast(&thread_wq);
        pthread_mutex_unlock(&thread_mutex);
    }
}

static int vogue_gps_start (void)
{
    int rc;

    if (need_init) {
        rc = core_init();
        if (rc)
            return rc;
    }

    GPS_LOG("echo start >> /sdcard/gps");
    if (!thread_running) {
        GPS_LOG("echo need start >> /sdcard/gps");
        rc = ioctl(gps_fd, VGPS_IOC_ENABLE);
        if (rc < 0)
            return rc;

        send_status(GPS_STATUS_SESSION_BEGIN);

        GPS_LOG("echo start 1 >> /sdcard/gps");
        start_thread();
    }
    return 0;
}

static int vogue_gps_stop (void)
{
    GPS_LOG("echo stop >> /sdcard/gps");
    if (thread_running) {
        pthread_mutex_lock(&thread_mutex);
        thread_running = 0;
        pthread_mutex_unlock(&thread_mutex);
    }
    ioctl(gps_fd, VGPS_IOC_DISABLE);
    send_status(GPS_STATUS_ENGINE_OFF);
    return 0;
}

static void vogue_gps_set_freq (int freq)
{
    GPS_LOG("echo set_freq >> /sdcard/gps");
    fix_freq = freq;
}

static void vogue_gps_cleanup (void)
{
    GPS_LOG("echo cleanup >> /sdcard/gps");
    vogue_gps_stop();
#if 0
    pthread_mutex_lock(&thread_mutex);
    thread_running = 2;
    pthread_cond_broadcast(&thread_wq);
    pthread_mutex_unlock(&thread_mutex);
    close(gps_fd);
#endif
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
    GPS_LOG("echo set_mode >> /sdcard/gps");
    fix_freq = freq;
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
    return &vogue_gps_iface;
}
