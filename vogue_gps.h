#ifndef _VOGUE_GPS_H_
#define _VOGUE_GPS_H_

#include <asm/ioctl.h>

/* API stuff */
struct gps_sat_state {
    int sat_no;
    int signal_strength;
};

#define MAX_SATELLITES 32

struct gps_state {
    uint32_t lat;
    uint32_t lng;
    uint32_t time;
    struct gps_sat_state sat_state[MAX_SATELLITES];
};

enum {
    VOGUE_GPS_ENABLE,
    VOGUE_GPS_DISABLE,
    VOGUE_GPS_NEW_FIX,
};

#define VGPS_IOC_ENABLE         _IO ('G', VOGUE_GPS_ENABLE)
#define VGPS_IOC_DISABLE        _IO ('G', VOGUE_GPS_DISABLE)
#define VGPS_IOC_NEW_FIX        _IO ('G', VOGUE_GPS_NEW_FIX)

#endif
