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

// Conditional compile-time defines
#if defined(ACC) && (defined(BARO) || defined(SONAR))
    #define NAV

    #if defined(GPS) && defined(MAG) && defined(NAV)
        #define NAV_3D
    #endif
#endif

// Log nav data to blackbox
#define NAV_BLACKBOX

// Maximum number of waypoints, special waypoint 0 = home, waypoint (NAV_MAX_WAYPOINTS + 1) = active WP
#define NAV_MAX_WAYPOINTS       15

// navigation mode
typedef enum {
    NAV_MODE_NONE       = 0,        // NAV is not used, pilot has full control. FC forced modes: none
    NAV_MODE_ALTHOLD    = 1 << 0,   // FC controls altitude. FC forced modes: LEVEL
    NAV_MODE_POSHOLD_2D = 1 << 1,   // FC controls everything execept throttle. Pilot controls yaw, throttle and velocity in 2D space. FC forced modes: LEVEL, MAG
    NAV_MODE_POSHOLD_3D = 1 << 2,   // FC controls everything. Pilot controls yaw rate and velocity in 3D space. FC forced modes: LEVEL, MAG
    NAV_MODE_WP         = 1 << 3,   // FC controls everything. Pilot has no control. FC forced modes: LEVEL, MAG
    NAV_MODE_RTH        = 1 << 4    // FC controls everything. Pilot has no control. FC forced modes: LEVEL, MAG
} navigationMode_e;

typedef struct navProfile_s {
    struct {
        unsigned use_midrc_for_althold : 1; // Don't remember throttle when althold was initiated, assume that throttle is at middle = zero climb rate
        unsigned throttle_tilt_comp : 1;    // Calculate and use automatic throttle tilt compensation
        unsigned lock_nav_until_takeoff : 1;// Easy mode, NAV won't mess up with controls on the ground
    } flags;

    uint16_t nav_wp_radius;                 // if we are within this distance to a waypoint then we consider it reached (distance is in cm)
    uint16_t nav_speed_min;                 // autonomous navigation speed cm/sec
    uint16_t nav_speed_max;                 // autonomous navigation speed cm/sec
    uint16_t nav_manual_speed_horizontal;   // manual velocity control max horizontal speed
    uint16_t nav_manual_speed_vertical;     // manual velocity control max vertical speed
    uint16_t nav_rc_deadband;               // Adds ability to adjust the Hold-position when moving the sticks (assisted mode)
    uint16_t nav_min_rth_distance;          // 0 Disables. Minimal distance for RTL in m, otherwise it will just autoland, prevent Failsafe jump in your face, when arming copter and turning off TX
    uint8_t  nav_lpf;                       // Low pass filter cut frequency for derivative calculation (default 20Hz)
    uint8_t  nav_expo;                      // 1 - 99 % defines the actual Expo applied for GPS
    float    gps_cf_vel;                    // GPS INS The LOWER the value the closer to gps speed // Dont go to high here
} navProfile_t;

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

void onNewGPSData(int32_t lat, int32_t lon);
void updateWaypointsAndNavigationMode(void);
void updateEstimatedAltitude(void);
void updateEstimatedHeading(void);
void updateEstimatedPositionFromIMU(void);
void applyWaypointNavigationAndAltitudeHold(void);
void resetHomePosition(void);
void setNextWaypointAndCalculateBearing(uint32_t lat, uint32_t lon, int32_t alt);
void updateHomePosition(void);

bool naivationRequiresAngleMode(void);
bool naivationControlsHeadingNow(void);
bool navigationControlsThrottleAngleCorrection(void);

extern float actualVerticalVelocity;
extern float actualHorizontalVelocity[];

//extern navigationMode_e navMode;
extern uint32_t distanceToHome;
extern int32_t directionToHome;
extern navPosition3D_t homePosition;
extern navPosition3D_t activeWpOrHoldPosition;    // Used for WP/ALTHOLD/PH/RTH
extern navPosition3D_t actualPosition;

#if defined(BLACKBOX)
extern int16_t navActualVelocity[3];
extern int16_t navDesiredVelocity[3];
extern int16_t navLatestPositionError[3];
extern int16_t navGPSVelocity[2];
extern int16_t navActualHeading;
extern int16_t navThrottleAngleCorrection;
extern int16_t navTargetAltitude;
#endif

#else

#define naivationRequiresAngleMode() (0)
#define naivationControlsHeadingNow() (0)

#endif

