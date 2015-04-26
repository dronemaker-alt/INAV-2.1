/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once


#include "flight/pid.h"

#include "sensors/barometer.h"

#include "io/rc_controls.h"
#include "io/escservo.h"

#define NAV

// Log nav data to blackbox
#define NAV_BLACKBOX

// Maximum number of waypoints, special waypoint 0 = home, waypoint (NAV_MAX_WAYPOINTS + 1) = active WP
#define NAV_MAX_WAYPOINTS       15

// navigation mode
typedef enum navigationMode_e {
    NAV_MODE_NONE       = 0,
    NAV_MODE_ALTHOLD    = 1 << 0,
    NAV_MODE_POSHOLD_2D = 1 << 1,
    NAV_MODE_POSHOLD_3D = 1 << 2,
    NAV_MODE_WP         = 1 << 3,
    NAV_MODE_RTH        = 1 << 4,
    NAV_MODE_RTH_2D     = 1 << 5,
} navigationMode_t;

enum {
    NAV_GPS_ATTI    = 0,
    NAV_GPS_CRUISE  = 1
};

enum {
    NAV_RTH_NO_ALT          = 0,            // Maintain current altitude
    NAX_RTH_EXTRA_ALT       = 1,            // Maintain current altitude + predefined safety margin
    NAV_RTH_CONST_ALT       = 2,            // Climb to predefined altitude
    NAV_RTH_MAX_ALT         = 3,            // Track maximum altitude and climb to it when RTH
};

typedef struct navProfile_s {
    struct {
        uint8_t use_midrc_for_althold;      // Don't remember throttle when althold was initiated, assume that throttle is at middle = zero climb rate
        uint8_t throttle_tilt_comp;         // Calculate and use automatic throttle tilt compensation
        uint8_t lock_nav_until_takeoff;     // Easy mode, NAV won't mess up with controls on the ground
        uint8_t user_control_mode;          // NAV_GPS_ATTI or NAV_GPS_CRUISE
        uint8_t rth_alt_control_style;      // Controls how RTH controls altitude
    } flags;

    uint16_t nav_wp_radius;                 // if we are within this distance to a waypoint then we consider it reached (distance is in cm)
    uint16_t nav_speed_max;                 // autonomous navigation speed cm/sec
    uint16_t nav_manual_speed_horizontal;   // manual velocity control max horizontal speed
    uint16_t nav_manual_speed_vertical;     // manual velocity control max vertical speed
    uint8_t  nav_rc_deadband;               // Adds ability to adjust the Hold-position when moving the sticks (assisted mode)
    uint16_t nav_rth_altitude;              // altitude to maintain when RTH is active (depends on rth_alt_control_style) (cm)
    uint16_t nav_min_rth_distance;          // 0 Disables. Minimal distance for RTL in cm, otherwise it will just autoland
    uint8_t  nav_pterm_cut_hz;              // Low pass filter cut frequency for P-term calculation (default 20Hz)
    uint8_t  nav_dterm_cut_hz;              // Low pass filter cut frequency for D-term calculation (default 5Hz)
    float    nav_gps_cf;                    // GPS INS The LOWER the value the closer to gps speed // Dont go to high here
} navProfile_t;

// LLH Location in NEU axis system
typedef struct navLocation_s {
    int32_t alt;    // Altitude in centimeters (meters * 100)
    int32_t lat;    // Lattitude * 1e+7
    int32_t lon;    // Longitude * 1e+7
} navLocation_t;

// Define a position in 3D space (coordinates are in GPS points)
typedef struct navPosition3D_s {
    int32_t altitude;
    int32_t coordinates[2];
    int32_t heading;
} navPosition3D_t;

#if defined(NAV) 

void navigationUsePIDs(pidProfile_t *pidProfile);
void navigationUseProfile(navProfile_t *navProfileToUse);
void navigationUseBarometerConfig(barometerConfig_t * intialBarometerConfig);
void navigationUseRcControlsConfig(rcControlsConfig_t *initialRcControlsConfig);
void navigationInit(navProfile_t *initialNavProfile, 
                    pidProfile_t *initialPidProfile, 
                    barometerConfig_t *intialBarometerConfig,
                    rcControlsConfig_t *initialRcControlsConfig);

void onNewGPSData(int32_t lat, int32_t lon, int32_t alt);

void updateWaypointsAndNavigationMode(void);
void updateAltitudeAndClimbRate(void);
void updateEstimatedHeading(void);
void applyWaypointNavigationAndAltitudeHold(void);
void resetHomePosition(void);
void updateHomePosition(void);

bool naivationRequiresAngleMode(void);
bool naivationControlsHeadingNow(void);
bool navigationControlsThrottleAngleCorrection(void);

float getEstimatedActualVelocity(int axis);
float getEstimatedActualPosition(int axis);

extern uint32_t distanceToHome;
extern int32_t directionToHome;
extern navPosition3D_t homePosition;

#if defined(BLACKBOX)
extern int16_t navCurrentMode;
extern int32_t navLatestActualPosition[3];
extern int16_t navActualVelocity[3];
extern int16_t navDesiredVelocity[3];
extern int16_t navLatestPositionError[3];
extern int16_t navGPSVelocity[3];
extern int16_t navBaroVelocity;
extern int16_t navActualHeading;
extern int16_t navDesiredHeading;
extern int16_t navThrottleAngleCorrection;
extern int16_t navTargetAltitude;
extern int16_t navWindCompensation[2];
extern int16_t navDebug[4];
#endif

#else

#define naivationRequiresAngleMode() (0)
#define naivationControlsHeadingNow() (0)
#define navigationControlsThrottleAngleCorrection() (0)

#endif
