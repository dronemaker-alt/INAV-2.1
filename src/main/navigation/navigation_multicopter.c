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

#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#include "platform.h"

#include "build/build_config.h"
#include "build/debug.h"

#include "drivers/time.h"

#include "common/axis.h"
#include "common/maths.h"
#include "common/filter.h"
#include "common/utils.h"

#include "sensors/sensors.h"
#include "sensors/acceleration.h"
#include "sensors/boardalignment.h"
#include "sensors/gyro.h"

#include "fc/config.h"
#include "fc/rc_controls.h"
#include "fc/rc_curves.h"
#include "fc/rc_modes.h"
#include "fc/runtime_config.h"

#include "flight/pid.h"
#include "flight/imu.h"
#include "flight/failsafe.h"
#include "flight/mixer.h"

#include "navigation/navigation_pos_estimator_private.h"
#include "navigation/navigation.h"
#include "navigation/navigation_private.h"
#include "navigation/sqrt_controller.h"

#include "sensors/battery.h"

/*-----------------------------------------------------------
 * Altitude controller for multicopter aircraft
 *-----------------------------------------------------------*/

static sqrt_controller_t pos_z_sqrt_controller;
static sqrt_controller_t shape_pos_z_sqrt_controller;
static sqrt_controller_t shape_vel_z_sqrt_controller;

static pt1Filter_t altholdThrottleFilterState;

static bool prepareForTakeoffOnReset = false;

static int16_t rcCommandAdjustedThrottle;
static int16_t altHoldThrottleRCZero = 1500;

float _vel_max_down_cms = 0.0f; // Max descent rate in cm/s used for kinematic shaping
float _vel_max_up_cms = 0.0f;   // Max climb rate in cm/s used for kinematic shaping
float _accel_max_z_cmss = 0.0f; // Max vertical acceleration in cm/s/s used for kinematic shaping
float _jerk_max_z_cmsss = 0.0f; // Jerk limit of the z kinematic path generation in cm/s^3 used to determine how quickly the aircraft varies the acceleration target

fpVector3_t _pos_target;
fpVector3_t _vel_desired;
fpVector3_t _vel_target;
fpVector3_t _accel_target;
fpVector3_t _accel_desired;
fpVector3_t _limit_vector;
float _pos_offset_z;
float _vel_offset_z;
float _accel_offset_z;
float _pos_offset_target_z = 0.0f;
float _vel_z_control_ratio = 1.0f;    // Confidence that we have control in the vertical axis

// update_vel_accel - single axis projection of velocity, vel, forwards in time based on a time step of dt and acceleration of accel.
// the velocity is not moved in the direction of limit if limit is not set to zero.
// limit - specifies if the system is unable to continue to accelerate.
// vel_error - specifies the direction of the velocity error used in limit handling.
void update_vel_accel(float *vel, float accel, float dt, float limit, float vel_error)
{
    float delta_vel = accel * dt;

    // do not add delta_vel if it will increase the velocity error in the direction of limit
    // unless adding delta_vel will reduce vel towards zero
    if ((delta_vel * limit > 0.0f) && (vel_error * limit > 0.0f)) {
        if (*vel * limit < 0.0f) {
            delta_vel = constrainf(delta_vel, -fabsf(*vel), fabsf(*vel));
        } else {
            delta_vel = 0.0f;
        }
    }

    *vel += delta_vel;
}

// update_pos_vel_accel - single axis projection of position and velocity forward in time based on a time step of dt and acceleration of accel.
// the position and velocity is not moved in the direction of limit if limit is not set to zero.
// limit - specifies if the system is unable to continue to accelerate.
// pos_error and vel_error - specifies the direction of the velocity error used in limit handling.
void update_pos_vel_accel(float *pos, float *vel, float accel, float dt, float limit, float pos_error, float vel_error)
{
    // move position and velocity forward by dt if it does not increase error when limited.
    float delta_pos = *vel * dt + accel * 0.5f * sq(dt);

    // do not add delta_pos if it will increase the velocity error in the direction of limit
    if ((delta_pos * limit > 0.0f) && (pos_error * limit > 0.0f)) {
        delta_pos = 0.0f;
    }

    *pos += delta_pos;

    update_vel_accel(vel, accel, dt, limit, vel_error);
}

/* shape_accel calculates a jerk limited path from the current acceleration to an input acceleration.
 The function takes the current acceleration and calculates the required jerk limited adjustment to the acceleration for the next time dt.
 The kinematic path is constrained by :
    maximum jerk - jerk_max (must be positive).
 The function alters the variable accel to follow a jerk limited kinematic path to accel_input.
*/
void shape_accel(float accel_input, float *accel, float jerk_max, float dt)
{
    // Sanity check jerk_max
    if (jerk_max < 0.0f) {
        return;
    }

    // Jerk limit acceleration change
    if (dt > 0.0f) {
        float accel_delta = accel_input - *accel;
        accel_delta = constrainf(accel_delta, -jerk_max * dt, jerk_max * dt);
        *accel += accel_delta;
    }
}

/* shape_vel_accel and shape_vel_xy calculate a jerk limited path from the current position, velocity and acceleration to an input velocity.
 The function takes the current position, velocity, and acceleration and calculates the required jerk limited adjustment to the acceleration for the next time dt.
 The kinematic path is constrained by :
    minimum acceleration - accel_min (must be negative),
    maximum acceleration - accel_max (must be positive),
    maximum jerk - jerk_max (must be positive).
 The function alters the variable accel to follow a jerk limited kinematic path to vel_input and accel_input.
 The correction acceleration is limited from accel_min to accel_max. If limit_total is true the target acceleration is limited from accel_min to accel_max.
*/
void shape_vel_accel(float vel_input, float accel_input,
                     float vel, float *accel,
                     float accel_min, float accel_max,
                     float jerk_max, float dt, bool limit_total_accel)
{
    // sanity check accel_min, accel_max and jerk_max.
    if ((accel_min > 0.0f) || (accel_max < 0.0f) || (jerk_max < 0.0f)) {
        return;
    }

    // velocity error to be corrected
    float vel_error = vel_input - vel;

    // Calculate time constants and limits to ensure stable operation
    // The direction of acceleration limit is the same as the velocity error.
    // This is because the velocity error is negative when slowing down while
    // closing a positive position error.
    float KPa;

    if (vel_error > 0.0f) {
        KPa = jerk_max / accel_max;
    } else {
        KPa = jerk_max / (-accel_min);
    }

    // acceleration to correct velocity
    shape_vel_z_sqrt_controller.kp = KPa;
    shape_vel_z_sqrt_controller.error = vel_error;
    shape_vel_z_sqrt_controller.derivative_max = jerk_max;
    float accel_target = sqrtControllerApply(&shape_vel_z_sqrt_controller, 0.0f, 0.0f, SQRT_CONTROLLER_NORMAL, dt);

    // constrain correction acceleration from accel_min to accel_max
    accel_target = constrainf(accel_target, accel_min, accel_max);

    // velocity correction with input velocity
    accel_target += accel_input;

    // constrain total acceleration from accel_min to accel_max
    if (limit_total_accel) {
        accel_target = constrainf(accel_target, accel_min, accel_max);
    }

    shape_accel(accel_target, accel, jerk_max, dt);
}

/* shape_pos_vel_accel calculate a jerk limited path from the current position, velocity and acceleration to an input position and velocity.
 The function takes the current position, velocity, and acceleration and calculates the required jerk limited adjustment to the acceleration for the next time dt.
 The kinematic path is constrained by :
    minimum velocity - vel_min (must not be positive),
    maximum velocity - vel_max (must not be negative),
    minimum acceleration - accel_min (must be negative),
    maximum acceleration - accel_max (must be positive),
    maximum jerk - jerk_max (must be positive).
 The function alters the variable accel to follow a jerk limited kinematic path to pos_input, vel_input and accel_input.
 The correction velocity is limited to vel_max to vel_min. If limit_total is true the target velocity is limited to vel_max to vel_min.
 The correction acceleration is limited from accel_min to accel_max. If limit_total is true the target acceleration is limited from accel_min to accel_max.
*/
void shape_pos_vel_accel(float pos_input, float vel_input, float accel_input,
                         float pos, float vel, float *accel,
                         float vel_min, float vel_max,
                         float accel_min, float accel_max,
                         float jerk_max, float dt, bool limit_total)
{
    // sanity check vel_min, vel_max, accel_min, accel_max and jerk_max.
    if ((vel_min > 0.0f) || (vel_max < 0.0f) || (accel_min > 0.0f) || (accel_max < 0.0f) || (jerk_max < 0.0f)) {
        return;
    }

    // position error to be corrected
    float pos_error = pos_input - pos;

    // Calculate time constants and limits to ensure stable operation
    // The negative acceleration limit is used here because the square root controller
    // manages the approach to the setpoint. Therefore the acceleration is in the opposite
    // direction to the position error.
    float accel_tc_max;
    float KPv;

    if ((pos_error > 0.0f)) {
        accel_tc_max = -0.5f * accel_min;
        KPv = 0.5f * jerk_max / (-accel_min);
    } else {
        accel_tc_max = 0.5 * accel_max;
        KPv = 0.5f * jerk_max / accel_max;
    }

    // velocity to correct position    
    shape_pos_z_sqrt_controller.kp = KPv;
    shape_pos_z_sqrt_controller.error = pos_error;
    shape_pos_z_sqrt_controller.derivative_max = accel_tc_max;
    float vel_target = sqrtControllerApply(&shape_pos_z_sqrt_controller, 0.0f, 0.0f, SQRT_CONTROLLER_NORMAL, dt);

    // limit velocity between vel_min and vel_max
    if ((vel_min < 0.0f) || (vel_max > 0.0f)) {
        vel_target = constrainf(vel_target, vel_min, vel_max);
    }

    // velocity correction with input velocity
    vel_target += vel_input;

    // limit velocity between vel_min and vel_max
    if (limit_total) {
        vel_target = constrainf(vel_target, vel_min, vel_max);
    }

    shape_vel_accel(vel_target, accel_input, vel, accel, accel_min, accel_max, jerk_max, dt, limit_total);
}

/// update_pos_offset_z - updates the vertical offsets used by terrain following
void update_pos_offset_z(float pos_offset_z, float dt)
{
    float p_offset_z = _pos_offset_z;
    update_pos_vel_accel(&p_offset_z, &_vel_offset_z, _accel_offset_z, dt, MIN(_limit_vector.z, 0.0f), posControl.pids.pos[Z].error, posControl.pids.vel[Z].error);
    _pos_offset_z = p_offset_z;

    // input shape the terrain offset
    shape_pos_vel_accel(pos_offset_z, 0.0f, 0.0f,
        _pos_offset_z, _vel_offset_z, &_accel_offset_z,
        _vel_max_down_cms, _vel_max_up_cms,
        -_accel_max_z_cmss, _accel_max_z_cmss,
        _jerk_max_z_cmsss, dt, false);
}

// calculate_overspeed_gain - calculated increased maximum acceleration and jerk if over speed condition is detected
float calculate_overspeed_gain(void)
{
    if (_vel_desired.z < _vel_max_down_cms && _vel_max_down_cms != 0.0f) {
        return NAV_MC_OVERSPEED_GAIN_Z * _vel_desired.z / _vel_max_down_cms;
    }

    if (_vel_desired.z > _vel_max_up_cms && _vel_max_up_cms != 0.0f) {
        return NAV_MC_OVERSPEED_GAIN_Z * _vel_desired.z / _vel_max_up_cms;
    }

    return 1.0f;
}

/// input_accel_z - calculate a jerk limited path from the current position, velocity and acceleration to an input acceleration.
///     The function takes the current position, velocity, and acceleration and calculates the required jerk limited adjustment to the acceleration for the next time dt.
///     The kinematic path is constrained by the maximum acceleration and jerk set using the function set_max_speed_accel_z.
///     The parameter limit_output specifies if the velocity and acceleration limits are applied to the sum of commanded and correction values or just correction.
void input_vel_accel_z(float *vel, float accel, bool limit_output, float dt)
{
    // calculated increased maximum acceleration and jerk if over speed
    const float overspeed_gain = calculate_overspeed_gain();
    const float accel_max_z_cmss = _accel_max_z_cmss * overspeed_gain;
    const float jerk_max_z_cmsss = _jerk_max_z_cmsss * overspeed_gain;

    // adjust desired alt if motors have not hit their limits
    update_pos_vel_accel(&_pos_target.z, &_vel_desired.z, _accel_desired.z, dt, _limit_vector.z, posControl.pids.pos[Z].error, posControl.pids.vel[Z].error);

    shape_vel_accel(*vel, accel,
                    _vel_desired.z, &_accel_desired.z,
                    -constrainf(accel_max_z_cmss, 0.0f, 750.0f), accel_max_z_cmss,
                    jerk_max_z_cmsss, dt, limit_output);

    update_vel_accel(vel, accel, dt, 0.0f, 0.0f);
}

void set_max_speed_accel_z(float speed_down, float speed_up, float accel_cmss)
{
    // Ensure speed_down is always negative
    speed_down = -fabsf(speed_down);

    // Sanity check and update
    if (speed_down < 0.0f) {
        _vel_max_down_cms = speed_down;
    }

    if (speed_up > 0.0f) {
        _vel_max_up_cms = speed_up;
    }

    if (accel_cmss > 0.0f) {
        _accel_max_z_cmss = accel_cmss;
    }
    
    // Ensure the vertical Jerk is not limited by the filters in the Z accel PID object
    _jerk_max_z_cmsss = navConfig()->general.shaping_jerk_z * 100.0f;

    if (posControl.pids.acceleration_z.errorLpfHz > 0.0f) {
        _jerk_max_z_cmsss = MIN(_jerk_max_z_cmsss, MIN(GRAVITY_CMSS, _accel_max_z_cmss) * ((M_PI * 2.0f) * posControl.pids.acceleration_z.errorLpfHz) / 5.0f);
    }
}

/// set_pos_target_z_from_climb_rate_cm - adjusts target up or down using a commanded climb rate in cm/s
///     using the default position control kinematic path.
///     The zero target altitude is varied to follow pos_offset_z
void set_pos_target_z_from_climb_rate_cm(float vel, float dt)
{
    // remove terrain offsets for flat earth assumption
    _pos_target.z -= _pos_offset_z;
    _vel_desired.z -= _vel_offset_z;
    _accel_desired.z -= _accel_offset_z;

    float vel_temp = vel;
    input_vel_accel_z(&vel_temp, 0.0f, true, dt);

    // update the vertical position, velocity and acceleration offsets
    update_pos_offset_z(_pos_offset_target_z, dt);

    // add terrain offsets
    _pos_target.z += _pos_offset_z;
    _vel_desired.z += _vel_offset_z;
    _accel_desired.z += _accel_offset_z;
}

// relaxZController - initialise the position controller to the current position and velocity with decaying acceleration.
///     This function decays the output acceleration by 95% every half second to achieve a smooth transition to zero requested acceleration.
void relaxZController(float dt)
{
    _pos_target.z = navGetCurrentActualPositionAndVelocity()->pos.z;
    _vel_desired.z = navGetCurrentActualPositionAndVelocity()->vel.z;
    // With zero position error _vel_target = _vel_desired
    _vel_target.z = navGetCurrentActualPositionAndVelocity()->vel.z;
    
    pt1FilterReset(&altholdThrottleFilterState, 0.0f);

    // Reset I term of velocity PID
    pt1FilterReset(&posControl.pids.vel[Z].error_filter_state, 0.0f);
    pt1FilterReset(&posControl.pids.vel[Z].dterm_filter_state, 0.0f);
    posControl.pids.vel[Z].integrator = 0.0f;

    _accel_desired.z = constrainf(posEstimator.imu.accelNEU.z, -_accel_max_z_cmss, _accel_max_z_cmss);
    // With zero position error _accel_target = _accel_desired
    _accel_target.z = _accel_desired.z;
    pt1FilterReset(&posControl.pids.acceleration_z.error_filter_state, 0.0f);

    // initialise vertical offsets
    _pos_offset_target_z = 0.0f;
    _pos_offset_z = 0.0f;
    _vel_offset_z = 0.0f;
    _accel_offset_z = 0.0f;

    // Set accel PID I term based on the current throttle
    // Remove the expected P term due to _accel_desired.z being constrained to _accel_max_z_cmss
    posControl.pids.acceleration_z.integrator = (rcCommand[THROTTLE] - currentBatteryProfile->nav.mc.hover_throttle) - posControl.pids.acceleration_z.param.kP * (_accel_target.z - posEstimator.imu.accelNEU.z);

    // init_z_controller has set the accel PID I term to generate the current throttle set point
    // Use relax_integrator to decay the throttle set point to throttle_setting
    navPidRelaxIntegrator(&posControl.pids.acceleration_z, currentBatteryProfile->nav.mc.hover_throttle - 1000U, dt, NAV_MC_INTEGRAL_RELAX_TC_Z);
}

// get_pilot_desired_climb_rate - transform pilot's throttle input to climb rate in cm/s
static float get_pilot_desired_climb_rate(void)
{
    const int16_t rcThrottleAdjustment = applyDeadbandRescaled(rcCommand[THROTTLE] - altHoldThrottleRCZero, rcControlsConfig()->alt_hold_deadband, -500, 500);
    float desiredRate = 0.0f;

    if (rcThrottleAdjustment) {
        // Make sure we can satisfy max_manual_climb_rate in both up and down directions
        if (rcThrottleAdjustment > 0) {
            // Scaling from altHoldThrottleRCZero to maxthrottle
            desiredRate = rcThrottleAdjustment * navConfig()->general.max_manual_climb_rate / (float)(motorConfig()->maxthrottle - altHoldThrottleRCZero - rcControlsConfig()->alt_hold_deadband);
        } else {
            // Scaling from minthrottle to altHoldThrottleRCZero
            desiredRate = rcThrottleAdjustment * navConfig()->general.max_manual_climb_rate / (float)(altHoldThrottleRCZero - getThrottleIdleValue() - rcControlsConfig()->alt_hold_deadband);
        }
    }

   return desiredRate;
}

static void updateZController(timeDelta_t deltaMicros)
{
    set_pos_target_z_from_climb_rate_cm(get_pilot_desired_climb_rate(), US2S(deltaMicros));

    // Calculate the target velocity correction
    _vel_target.z = sqrtControllerApply(&pos_z_sqrt_controller, _pos_target.z, navGetCurrentActualPositionAndVelocity()->pos.z, SQRT_CONTROLLER_POS_VEL_Z, US2S(deltaMicros));
    
    // Add feed forward component
    _vel_target.z += _vel_desired.z;

    // Hard limit desired target velocity to max_climb_rate
    float vel_max_z = 0.0f;

    if (posControl.flags.isAdjustingAltitude) {
        vel_max_z = navConfig()->general.max_manual_climb_rate;
    } else {
        vel_max_z = navConfig()->general.max_auto_climb_rate;
    }

    _vel_target.z = constrainf(_vel_target.z, -vel_max_z, vel_max_z);

    posControl.pids.pos[Z].output_constrained = _vel_target.z;

    /***********************
    Velocity Controller
    ***********************/

    // Calculate min and max throttle boundaries (to compensate for integral windup)
    const int16_t thrCorrectionMin = getThrottleIdleValue() - currentBatteryProfile->nav.mc.hover_throttle;
    const int16_t thrCorrectionMax = motorConfig()->maxthrottle - currentBatteryProfile->nav.mc.hover_throttle;
    _accel_target.z = navPidApply2(&posControl.pids.vel[Z], _vel_target.z, navGetCurrentActualPositionAndVelocity()->vel.z, US2S(deltaMicros), thrCorrectionMin, thrCorrectionMax, 0);
    
    // Add feed forward component
    _accel_target.z += _accel_desired.z;

    /***********************
    Acceleration Controller
    ***********************/

    // Calculate vertical acceleration
    float thr_out;
    float correctionMax = NAV_MC_ACC_Z_IMAX;

    // Ensure imax is always large enough to overpower hover throttle
    if (thrCorrectionMax > NAV_MC_ACC_Z_IMAX) {
        correctionMax = thrCorrectionMax;
    }

    thr_out = navPidApply2(&posControl.pids.acceleration_z, _accel_target.z, posEstimator.imu.accelNEU.z, US2S(deltaMicros), thrCorrectionMin, correctionMax, PID_LIMIT_INTEGRATOR);

    int16_t rcThrottleCorrection = pt1FilterApply4(&altholdThrottleFilterState, thr_out, NAV_THROTTLE_CUTOFF_FREQUENCY_HZ, US2S(deltaMicros));
    rcThrottleCorrection = constrain(rcThrottleCorrection, thrCorrectionMin, thrCorrectionMax);

    // _vel_max_down_cms is checked to be non-zero when set
    float error_ratio = posControl.pids.vel[Z].error / _vel_max_down_cms;
    _vel_z_control_ratio += US2S(deltaMicros) * 0.1f * (0.5f - error_ratio);
    _vel_z_control_ratio = constrainf(_vel_z_control_ratio, 0.0f, 1.0f);
    
    if ((navGetCurrentStateFlags() & NAV_CTL_LAND) && !STATE(LANDING_DETECTED)) {
        rcThrottleCorrection = (1.0f - _vel_z_control_ratio) * getThrottleIdleValue() + _vel_z_control_ratio * motorConfig()->maxthrottle;
    } else {
        rcThrottleCorrection += currentBatteryProfile->nav.mc.hover_throttle;
    }

    posControl.rcAdjustment[THROTTLE] = setDesiredThrottle(rcThrottleCorrection, false);

    navDesiredVelocity[Z] = constrain(lrintf(posControl.desiredState.vel.z), -32678, 32767);
}

// To use with RangeFinder
void set_pos_offset_target_z_cm(float pos_offset_target_z) 
{
    _pos_offset_target_z = pos_offset_target_z; 
}

bool adjustMulticopterAltitudeFromRCInput(void)
{
    if (posControl.flags.isTerrainFollowEnabled) {
        const float altTarget = scaleRangef(rcCommand[THROTTLE], getThrottleIdleValue(), motorConfig()->maxthrottle, 0.0f, navConfig()->general.max_terrain_follow_altitude);

        // In terrain follow mode we apply different logic for terrain control
        if (posControl.flags.estAglStatus == EST_TRUSTED && altTarget > 10.0f) {
            // We have solid terrain sensor signal - directly map throttle to altitude
            updateClimbRateToAltitudeController(0, 0, ROC_TO_ALT_RESET);
            set_pos_offset_target_z_cm(altTarget);
        }
        else {
            updateClimbRateToAltitudeController(-50.0f, 0, ROC_TO_ALT_CONSTANT);
        }

        // In surface tracking we always indicate that we're adjusting altitude
        return true;
    } else {
        const int16_t rcThrottleAdjustment = applyDeadbandRescaled(rcCommand[THROTTLE] - altHoldThrottleRCZero, rcControlsConfig()->alt_hold_deadband, -500, 500);
        if (rcThrottleAdjustment) {
            // set velocity proportional to stick movement
            float rcClimbRate;

            // Make sure we can satisfy max_manual_climb_rate in both up and down directions
            if (rcThrottleAdjustment > 0) {
                // Scaling from altHoldThrottleRCZero to maxthrottle
                rcClimbRate = rcThrottleAdjustment * navConfig()->general.max_manual_climb_rate / (float)(motorConfig()->maxthrottle - altHoldThrottleRCZero - rcControlsConfig()->alt_hold_deadband);
            }
            else {
                // Scaling from minthrottle to altHoldThrottleRCZero
                rcClimbRate = rcThrottleAdjustment * navConfig()->general.max_manual_climb_rate / (float)(altHoldThrottleRCZero - getThrottleIdleValue() - rcControlsConfig()->alt_hold_deadband);
            }

            updateClimbRateToAltitudeController(rcClimbRate, 0, ROC_TO_ALT_CONSTANT);

            return true;
        } else {
            // Adjusting finished - reset desired position to stay exactly where pilot released the stick
            if (posControl.flags.isAdjustingAltitude) {
                updateClimbRateToAltitudeController(0, 0, ROC_TO_ALT_RESET);
            }

            return false;
        }
    }
}

void setupMulticopterAltitudeController(void)
{
    const bool throttleIsLow = throttleStickIsLow();
    const uint8_t throttleType = navConfig()->mc.althold_throttle_type;

    if (throttleType == MC_ALT_HOLD_STICK && !throttleIsLow) {
        // Only use current throttle if not LOW - use Thr Mid otherwise
        altHoldThrottleRCZero = rcCommand[THROTTLE];
    } else if (throttleType == MC_ALT_HOLD_HOVER) {
        altHoldThrottleRCZero = currentBatteryProfile->nav.mc.hover_throttle;
    } else {
        altHoldThrottleRCZero = rcLookupThrottleMid();
    }

    // Make sure we are able to satisfy the deadband
    altHoldThrottleRCZero = constrain(altHoldThrottleRCZero,
                                      getThrottleIdleValue() + rcControlsConfig()->alt_hold_deadband + 10,
                                      motorConfig()->maxthrottle - rcControlsConfig()->alt_hold_deadband - 10);

    // Force AH controller to initialize althold integral for pending takeoff on reset
    // Signal for that is low throttle _and_ low actual altitude
    if (throttleIsLow && fabsf(navGetCurrentActualPositionAndVelocity()->pos.z) <= 50.0f) {
        prepareForTakeoffOnReset = true;
    }
}

void resetMulticopterAltitudeController(void)
{
    navPidReset(&posControl.pids.vel[Z]);
    navPidReset(&posControl.pids.surface);

    if (FLIGHT_MODE(FAILSAFE_MODE) || FLIGHT_MODE(NAV_RTH_MODE) || FLIGHT_MODE(NAV_WP_MODE) || navigationIsExecutingAnEmergencyLanding()) {
        set_max_speed_accel_z(navConfig()->general.max_auto_climb_rate, navConfig()->general.max_auto_climb_rate, navConfig()->general.max_auto_acceleration);
    } else {
        set_max_speed_accel_z(navConfig()->general.max_manual_climb_rate, navConfig()->general.max_manual_climb_rate, navConfig()->general.max_manual_acceleration);
    }

    sqrtControllerInit(&pos_z_sqrt_controller, posControl.pids.pos[Z].param.kP, _vel_max_down_cms, _vel_max_up_cms, _accel_max_z_cmss);

    posControl.rcAdjustment[THROTTLE] = currentBatteryProfile->nav.mc.hover_throttle;

    posControl.desiredState.vel.z = navGetCurrentActualPositionAndVelocity()->vel.z;   // Gradually transition from current climb
    _pos_target.z = navGetCurrentActualPositionAndVelocity()->pos.z;
    _vel_desired.z = navGetCurrentActualPositionAndVelocity()->vel.z;
    _vel_target.z = navGetCurrentActualPositionAndVelocity()->vel.z;
    _accel_desired.z = constrainf(posEstimator.imu.accelNEU.z, -_accel_max_z_cmss, _accel_max_z_cmss);
    _accel_target.z = _accel_desired.z;
    _pos_offset_z = 0.0f;
    _vel_offset_z = 0.0f;
    _accel_offset_z = 0.0f;

    pt1FilterReset(&altholdThrottleFilterState, 0.0f);
    pt1FilterReset(&posControl.pids.vel[Z].error_filter_state, 0.0f);
    pt1FilterReset(&posControl.pids.vel[Z].dterm_filter_state, 0.0f);
    pt1FilterReset(&posControl.pids.acceleration_z.error_filter_state, 0.0f);

    // Reset I term of velocity PID
    posControl.pids.vel[Z].integrator = 0.0f;

    // Set accel PID I term based on the current throttle
    // Remove the expected P term due to _accel_desired.z being constrained to _accel_max_z_cmss
    posControl.pids.acceleration_z.integrator = (rcCommand[THROTTLE] - currentBatteryProfile->nav.mc.hover_throttle) - posControl.pids.acceleration_z.param.kP * (_accel_target.z - posEstimator.imu.accelNEU.z);
}

static void applyMulticopterAltitudeController(timeUs_t currentTimeUs)
{
    static timeUs_t previousTimePositionUpdate = 0;     // Occurs @ altitude sensor update rate (max MAX_ALTITUDE_UPDATE_RATE_HZ)

    // If we have an update on vertical position data - update velocity and accel targets
    if (posControl.flags.verticalPositionDataNew) {
        const timeDeltaLarge_t deltaMicrosPositionUpdate = currentTimeUs - previousTimePositionUpdate;
        previousTimePositionUpdate = currentTimeUs;

        // Check if last correction was not too long ago
        if (deltaMicrosPositionUpdate < MAX_POSITION_UPDATE_INTERVAL_US) {
            if (prepareForTakeoffOnReset) {
                // If we are preparing for takeoff - start with lowset possible climb rate, adjust alt target and make sure throttle doesn't jump
                //posControl.desiredState.vel.z = -navConfig()->general.max_manual_climb_rate;
                //posControl.desiredState.pos.z = navGetCurrentActualPositionAndVelocity()->pos.z - (navConfig()->general.max_manual_climb_rate / posControl.pids.pos[Z].param.kP);
                //posControl.pids.vel[Z].integrator = -500.0f;
                //pt1FilterReset(&altholdThrottleFilterState, -500.0f);

                relaxZController(US2S(deltaMicrosPositionUpdate));

                prepareForTakeoffOnReset = false;
            }

            updateZController(deltaMicrosPositionUpdate);
        }
        else {
            // Position update has not occurred in time (first start or glitch), reset altitude controller
            resetMulticopterAltitudeController();
        }

        // Indicate that information is no longer usable
        posControl.flags.verticalPositionDataConsumed = true;
    }

    // Update throttle controller
    rcCommand[THROTTLE] = posControl.rcAdjustment[THROTTLE];

    // Save processed throttle for future use
    rcCommandAdjustedThrottle = rcCommand[THROTTLE];
}

/*-----------------------------------------------------------
 * Adjusts desired heading from pilot's input
 *-----------------------------------------------------------*/
bool adjustMulticopterHeadingFromRCInput(void)
{
    if (ABS(rcCommand[YAW]) > rcControlsConfig()->pos_hold_deadband) {
        // Heading during Cruise Hold mode set by Nav function so no adjustment required here
        if (!FLIGHT_MODE(NAV_COURSE_HOLD_MODE)) {
            posControl.desiredState.yaw = posControl.actualState.yaw;
        }

        return true;
    }

    return false;
}

/*-----------------------------------------------------------
 * XY-position controller for multicopter aircraft
 *-----------------------------------------------------------*/
static float lastAccelTargetX = 0.0f, lastAccelTargetY = 0.0f;

void resetMulticopterBrakingMode(void)
{
    DISABLE_STATE(NAV_CRUISE_BRAKING);
    DISABLE_STATE(NAV_CRUISE_BRAKING_BOOST);
    DISABLE_STATE(NAV_CRUISE_BRAKING_LOCKED);
}

static void processMulticopterBrakingMode(const bool isAdjusting)
{
#ifdef USE_MR_BRAKING_MODE
    static uint32_t brakingModeDisengageAt = 0;
    static uint32_t brakingBoostModeDisengageAt = 0;

    if (!(NAV_Status.state == MW_NAV_STATE_NONE || NAV_Status.state == MW_NAV_STATE_HOLD_INFINIT)) {
        resetMulticopterBrakingMode();
        return;
    }

    const bool brakingEntryAllowed =
        IS_RC_MODE_ACTIVE(BOXBRAKING) &&
        !STATE(NAV_CRUISE_BRAKING_LOCKED) &&
        posControl.actualState.velXY > navConfig()->mc.braking_speed_threshold &&
        !isAdjusting &&
        navConfig()->general.flags.user_control_mode == NAV_GPS_CRUISE &&
        navConfig()->mc.braking_speed_threshold > 0;


    /*
     * Case one, when we order to brake (sticks to the center) and we are moving above threshold
     * Speed is above 1m/s and sticks are centered
     * Extra condition: BRAKING flight mode has to be enabled
     */
    if (brakingEntryAllowed) {
        /*
         * Set currnt position and target position
         * Enabling NAV_CRUISE_BRAKING locks other routines from setting position!
         */
        setDesiredPosition(&navGetCurrentActualPositionAndVelocity()->pos, 0, NAV_POS_UPDATE_XY);

        ENABLE_STATE(NAV_CRUISE_BRAKING_LOCKED);
        ENABLE_STATE(NAV_CRUISE_BRAKING);

        //Set forced BRAKING disengage moment
        brakingModeDisengageAt = millis() + navConfig()->mc.braking_timeout;

        //If speed above threshold, start boost mode as well
        if (posControl.actualState.velXY > navConfig()->mc.braking_boost_speed_threshold) {
            ENABLE_STATE(NAV_CRUISE_BRAKING_BOOST);

            brakingBoostModeDisengageAt = millis() + navConfig()->mc.braking_boost_timeout;
        }

    }

    // We can enter braking only after user started to move the sticks
    if (STATE(NAV_CRUISE_BRAKING_LOCKED) && isAdjusting) {
        DISABLE_STATE(NAV_CRUISE_BRAKING_LOCKED);
    }

    /*
     * Case when speed dropped, disengage BREAKING_BOOST
     */
    if (
        STATE(NAV_CRUISE_BRAKING_BOOST) && (
            posControl.actualState.velXY <= navConfig()->mc.braking_boost_disengage_speed ||
            brakingBoostModeDisengageAt < millis()
    )) {
        DISABLE_STATE(NAV_CRUISE_BRAKING_BOOST);
    }

    /*
     * Case when we were braking but copter finally stopped or we started to move the sticks
     */
    if (STATE(NAV_CRUISE_BRAKING) && (
        posControl.actualState.velXY <= navConfig()->mc.braking_disengage_speed ||  //We stopped
        isAdjusting ||                                                              //Moved the sticks
        brakingModeDisengageAt < millis()                                           //Braking is done to timed disengage
    )) {
        DISABLE_STATE(NAV_CRUISE_BRAKING);
        DISABLE_STATE(NAV_CRUISE_BRAKING_BOOST);

        /*
         * When braking is done, store current position as desired one
         * We do not want to go back to the place where braking has started
         */
        setDesiredPosition(&navGetCurrentActualPositionAndVelocity()->pos, 0, NAV_POS_UPDATE_XY);
    }
#else
    UNUSED(isAdjusting);
#endif
}

void resetMulticopterPositionController(void)
{
    for (int axis = 0; axis < 2; axis++) {
        navPidReset(&posControl.pids.vel[axis]);
        posControl.rcAdjustment[axis] = 0;
        lastAccelTargetX = 0.0f;
        lastAccelTargetY = 0.0f;
    }
}

static bool adjustMulticopterCruiseSpeed(int16_t rcPitchAdjustment)
{
    static timeMs_t lastUpdateTimeMs;
    const timeMs_t currentTimeMs = millis();
    const timeMs_t updateDeltaTimeMs = currentTimeMs - lastUpdateTimeMs;
    lastUpdateTimeMs = currentTimeMs;

    const float rcVelX = rcPitchAdjustment * navConfig()->general.max_manual_speed / (float)(500 - rcControlsConfig()->pos_hold_deadband);

    if (rcVelX > posControl.cruise.multicopterSpeed) {
        posControl.cruise.multicopterSpeed = rcVelX;
    } else if (rcVelX < 0 && updateDeltaTimeMs < 100) {
        posControl.cruise.multicopterSpeed += MS2S(updateDeltaTimeMs) * rcVelX / 2.0f;
    } else {
        return false;
    }
    posControl.cruise.multicopterSpeed = constrainf(posControl.cruise.multicopterSpeed, 10.0f, navConfig()->general.max_manual_speed);

    return true;
}

static void setMulticopterStopPosition(void)
{
    fpVector3_t stopPosition;
    calculateMulticopterInitialHoldPosition(&stopPosition);
    setDesiredPosition(&stopPosition, 0, NAV_POS_UPDATE_XY);
}

bool adjustMulticopterPositionFromRCInput(int16_t rcPitchAdjustment, int16_t rcRollAdjustment)
{
    if (navGetMappedFlightModes(posControl.navState) & NAV_COURSE_HOLD_MODE) {
        if (rcPitchAdjustment) {
            return adjustMulticopterCruiseSpeed(rcPitchAdjustment);
        }

        return false;
    }

    // Process braking mode
    processMulticopterBrakingMode(rcPitchAdjustment || rcRollAdjustment);

    // Actually change position
    if (rcPitchAdjustment || rcRollAdjustment) {
        // If mode is GPS_CRUISE, move target position, otherwise POS controller will passthru the RC input to ANGLE PID
        if (navConfig()->general.flags.user_control_mode == NAV_GPS_CRUISE) {
            const float rcVelX = rcPitchAdjustment * navConfig()->general.max_manual_speed / (float)(500 - rcControlsConfig()->pos_hold_deadband);
            const float rcVelY = rcRollAdjustment * navConfig()->general.max_manual_speed / (float)(500 - rcControlsConfig()->pos_hold_deadband);

            // Rotate these velocities from body frame to to earth frame
            const float neuVelX = rcVelX * posControl.actualState.cosYaw - rcVelY * posControl.actualState.sinYaw;
            const float neuVelY = rcVelX * posControl.actualState.sinYaw + rcVelY * posControl.actualState.cosYaw;

            // Calculate new position target, so Pos-to-Vel P-controller would yield desired velocity
            posControl.desiredState.pos.x = navGetCurrentActualPositionAndVelocity()->pos.x + (neuVelX / posControl.pids.pos[X].param.kP);
            posControl.desiredState.pos.y = navGetCurrentActualPositionAndVelocity()->pos.y + (neuVelY / posControl.pids.pos[Y].param.kP);
        }

        return true;
    }
    else if (posControl.flags.isAdjustingPosition) {
        // Adjusting finished - reset desired position to stay exactly where pilot released the stick
        setMulticopterStopPosition();
    }

    return false;
}

static float getVelocityHeadingAttenuationFactor(void)
{
    // In WP mode scale velocity if heading is different from bearing
    if (navConfig()->mc.slowDownForTurning && (navGetCurrentStateFlags() & NAV_AUTO_WP)) {
        const int32_t headingError = constrain(wrap_18000(posControl.desiredState.yaw - posControl.actualState.yaw), -9000, 9000);
        const float velScaling = cos_approx(CENTIDEGREES_TO_RADIANS(headingError));

        return constrainf(velScaling * velScaling, 0.05f, 1.0f);
    } else {
        return 1.0f;
    }
}

static float getVelocityExpoAttenuationFactor(float velTotal, float velMax)
{
    // Calculate factor of how velocity with applied expo is different from unchanged velocity
    const float velScale = constrainf(velTotal / velMax, 0.01f, 1.0f);

    // navConfig()->max_speed * ((velScale * velScale * velScale) * posControl.posResponseExpo + velScale * (1 - posControl.posResponseExpo)) / velTotal;
    // ((velScale * velScale * velScale) * posControl.posResponseExpo + velScale * (1 - posControl.posResponseExpo)) / velScale
    // ((velScale * velScale) * posControl.posResponseExpo + (1 - posControl.posResponseExpo));
    return 1.0f - posControl.posResponseExpo * (1.0f - (velScale * velScale));  // x^3 expo factor
}

static void updatePositionVelocityController_MC(const float maxSpeed)
{
    if (FLIGHT_MODE(NAV_COURSE_HOLD_MODE)) {
        // Position held at cruise speeds below 0.5 m/s, otherwise desired neu velocities set directly from cruise speed
        if (posControl.cruise.multicopterSpeed >= 50) {
            // Rotate multicopter x velocity from body frame to earth frame
            posControl.desiredState.vel.x = posControl.cruise.multicopterSpeed * cos_approx(CENTIDEGREES_TO_RADIANS(posControl.cruise.course));
            posControl.desiredState.vel.y = posControl.cruise.multicopterSpeed * sin_approx(CENTIDEGREES_TO_RADIANS(posControl.cruise.course));

            return;
        } else if (posControl.flags.isAdjustingPosition) {
            setMulticopterStopPosition();
        }
    }

    const float posErrorX = posControl.desiredState.pos.x - navGetCurrentActualPositionAndVelocity()->pos.x;
    const float posErrorY = posControl.desiredState.pos.y - navGetCurrentActualPositionAndVelocity()->pos.y;

    // Calculate target velocity
    float neuVelX = posErrorX * posControl.pids.pos[X].param.kP;
    float neuVelY = posErrorY * posControl.pids.pos[Y].param.kP;

    // Scale velocity to respect max_speed
    float neuVelTotal = calc_length_pythagorean_2D(neuVelX, neuVelY);

    /*
     * We override computed speed with max speed in following cases:
     * 1 - computed velocity is > maxSpeed
     * 2 - in WP mission or RTH Trackback when: slowDownForTurning is OFF, not a hold waypoint and computed speed is < maxSpeed
     */
    if (
        ((navGetCurrentStateFlags() & NAV_AUTO_WP || posControl.flags.rthTrackbackActive) &&
        !isNavHoldPositionActive() &&
        neuVelTotal < maxSpeed &&
        !navConfig()->mc.slowDownForTurning
        ) || neuVelTotal > maxSpeed
    ) {
        neuVelX = maxSpeed * (neuVelX / neuVelTotal);
        neuVelY = maxSpeed * (neuVelY / neuVelTotal);
        neuVelTotal = maxSpeed;
    }

    posControl.pids.pos[X].output_constrained = neuVelX;
    posControl.pids.pos[Y].output_constrained = neuVelY;

    // Apply expo & attenuation if heading in wrong direction - turn first, accelerate later (effective only in WP mode)
    const float velHeadFactor = getVelocityHeadingAttenuationFactor();
    const float velExpoFactor = getVelocityExpoAttenuationFactor(neuVelTotal, maxSpeed);
    posControl.desiredState.vel.x = neuVelX * velHeadFactor * velExpoFactor;
    posControl.desiredState.vel.y = neuVelY * velHeadFactor * velExpoFactor;
}

static float computeNormalizedVelocity(const float value, const float maxValue)
{
    return constrainf(scaleRangef(fabsf(value), 0, maxValue, 0.0f, 1.0f), 0.0f, 1.0f);
}

static float computeVelocityScale(
    const float value,
    const float maxValue,
    const float attenuationFactor,
    const float attenuationStart,
    const float attenuationEnd
)
{
    const float normalized = computeNormalizedVelocity(value, maxValue);

    float scale = scaleRangef(normalized, attenuationStart, attenuationEnd, 0, attenuationFactor);
    return constrainf(scale, 0, attenuationFactor);
}

static void updatePositionAccelController_MC(timeDelta_t deltaMicros, float maxAccelLimit, const float maxSpeed)
{
    const float measurementX = navGetCurrentActualPositionAndVelocity()->vel.x;
    const float measurementY = navGetCurrentActualPositionAndVelocity()->vel.y;

    const float setpointX = posControl.desiredState.vel.x;
    const float setpointY = posControl.desiredState.vel.y;
    const float setpointXY = calc_length_pythagorean_2D(setpointX, setpointY);

    // Calculate velocity error
    const float velErrorX = setpointX - measurementX;
    const float velErrorY = setpointY - measurementY;

    // Calculate XY-acceleration limit according to velocity error limit
    float accelLimitX, accelLimitY;
    const float velErrorMagnitude = calc_length_pythagorean_2D(velErrorX, velErrorY);

    if (velErrorMagnitude > 0.1f) {
        accelLimitX = maxAccelLimit / velErrorMagnitude * fabsf(velErrorX);
        accelLimitY = maxAccelLimit / velErrorMagnitude * fabsf(velErrorY);
    } else {
        accelLimitX = maxAccelLimit / 1.414213f;
        accelLimitY = accelLimitX;
    }

    // Apply additional jerk limiting of 1700 cm/s^3 (~100 deg/s), almost any copter should be able to achieve this rate
    // This will assure that we wont't saturate out LEVEL and RATE PID controller

    float maxAccelChange = US2S(deltaMicros) * MC_POS_CONTROL_JERK_LIMIT_CMSSS;
    //When braking, raise jerk limit even if we are not boosting acceleration
#ifdef USE_MR_BRAKING_MODE
    if (STATE(NAV_CRUISE_BRAKING)) {
        maxAccelChange = maxAccelChange * 2;
    }
#endif

    const float accelLimitXMin = constrainf(lastAccelTargetX - maxAccelChange, -accelLimitX, +accelLimitX);
    const float accelLimitXMax = constrainf(lastAccelTargetX + maxAccelChange, -accelLimitX, +accelLimitX);
    const float accelLimitYMin = constrainf(lastAccelTargetY - maxAccelChange, -accelLimitY, +accelLimitY);
    const float accelLimitYMax = constrainf(lastAccelTargetY + maxAccelChange, -accelLimitY, +accelLimitY);

    // TODO: Verify if we need jerk limiting after all

    /*
     * This PID controller has dynamic dTerm scale. It's less active when controller
     * is tracking setpoint at high speed. Full dTerm is required only for position hold,
     * acceleration and deceleration
     * Scale down dTerm with 2D speed
     */
    const float setpointScale = computeVelocityScale(
        setpointXY,
        maxSpeed,
        multicopterPosXyCoefficients.dTermAttenuation,
        multicopterPosXyCoefficients.dTermAttenuationStart,
        multicopterPosXyCoefficients.dTermAttenuationEnd
    );
    const float measurementScale = computeVelocityScale(
        posControl.actualState.velXY,
        maxSpeed,
        multicopterPosXyCoefficients.dTermAttenuation,
        multicopterPosXyCoefficients.dTermAttenuationStart,
        multicopterPosXyCoefficients.dTermAttenuationEnd
    );

    //Choose smaller attenuation factor and convert from attenuation to scale
    const float dtermScale = 1.0f - MIN(setpointScale, measurementScale);

    // Apply PID with output limiting and I-term anti-windup
    // Pre-calculated accelLimit and the logic of navPidApply2 function guarantee that our newAccel won't exceed maxAccelLimit
    // Thus we don't need to do anything else with calculated acceleration
    float newAccelX = navPidApply3(
        &posControl.pids.vel[X],
        setpointX,
        measurementX,
        US2S(deltaMicros),
        accelLimitXMin,
        accelLimitXMax,
        0,      // Flags
        1.0f,   // Total gain scale
        dtermScale    // Additional dTerm scale
    );
    float newAccelY = navPidApply3(
        &posControl.pids.vel[Y],
        setpointY,
        measurementY,
        US2S(deltaMicros),
        accelLimitYMin,
        accelLimitYMax,
        0,      // Flags
        1.0f,   // Total gain scale
        dtermScale    // Additional dTerm scale
    );

    int32_t maxBankAngle = DEGREES_TO_DECIDEGREES(navConfig()->mc.max_bank_angle);

#ifdef USE_MR_BRAKING_MODE
    //Boost required accelerations
    if (STATE(NAV_CRUISE_BRAKING_BOOST) && multicopterPosXyCoefficients.breakingBoostFactor > 0.0f) {

        //Scale boost factor according to speed
        const float boostFactor = constrainf(
            scaleRangef(
                posControl.actualState.velXY,
                navConfig()->mc.braking_boost_speed_threshold,
                navConfig()->general.max_manual_speed,
                0.0f,
                multicopterPosXyCoefficients.breakingBoostFactor
            ),
            0.0f,
            multicopterPosXyCoefficients.breakingBoostFactor
        );

        //Boost required acceleration for harder braking
        newAccelX = newAccelX * (1.0f + boostFactor);
        newAccelY = newAccelY * (1.0f + boostFactor);

        maxBankAngle = DEGREES_TO_DECIDEGREES(navConfig()->mc.braking_bank_angle);
    }
#endif

    // Save last acceleration target
    lastAccelTargetX = newAccelX;
    lastAccelTargetY = newAccelY;

    // Rotate acceleration target into forward-right frame (aircraft)
    const float accelForward = newAccelX * posControl.actualState.cosYaw + newAccelY * posControl.actualState.sinYaw;
    const float accelRight = -newAccelX * posControl.actualState.sinYaw + newAccelY * posControl.actualState.cosYaw;

    // Calculate banking angles
    const float desiredPitch = atan2_approx(accelForward, GRAVITY_CMSS);
    const float desiredRoll = atan2_approx(accelRight * cos_approx(desiredPitch), GRAVITY_CMSS);

    posControl.rcAdjustment[ROLL] = constrain(RADIANS_TO_DECIDEGREES(desiredRoll), -maxBankAngle, maxBankAngle);
    posControl.rcAdjustment[PITCH] = constrain(RADIANS_TO_DECIDEGREES(desiredPitch), -maxBankAngle, maxBankAngle);
}

static void applyMulticopterPositionController(timeUs_t currentTimeUs)
{
    // Apply controller only if position source is valid. In absence of valid pos sensor (GPS loss), we'd stick in forced ANGLE mode
    // and pilots input would be passed thru to PID controller
    if (posControl.flags.estPosStatus < EST_USABLE) {
        /* No position data, disable automatic adjustment, rcCommand passthrough */
        posControl.rcAdjustment[PITCH] = 0;
        posControl.rcAdjustment[ROLL] = 0;

        return;
    }

    // Passthrough rcCommand if adjusting position in GPS_ATTI mode except when Course Hold active
    bool bypassPositionController = !FLIGHT_MODE(NAV_COURSE_HOLD_MODE) &&
                                    navConfig()->general.flags.user_control_mode == NAV_GPS_ATTI &&
                                    posControl.flags.isAdjustingPosition;

    if (posControl.flags.horizontalPositionDataNew) {
        // Indicate that information is no longer usable
        posControl.flags.horizontalPositionDataConsumed = true;

        static timeUs_t previousTimePositionUpdate = 0;     // Occurs @ GPS update rate
        const timeDeltaLarge_t deltaMicrosPositionUpdate = currentTimeUs - previousTimePositionUpdate;
        previousTimePositionUpdate = currentTimeUs;

        if (bypassPositionController) {
            return;
        }

        // If we have new position data - update velocity and acceleration controllers
        if (deltaMicrosPositionUpdate < MAX_POSITION_UPDATE_INTERVAL_US) {
            // Get max speed for current NAV mode
            float maxSpeed = getActiveSpeed();
            updatePositionVelocityController_MC(maxSpeed);
            updatePositionAccelController_MC(deltaMicrosPositionUpdate, NAV_MC_ACCELERATION_XY_MAX, maxSpeed);

            navDesiredVelocity[X] = constrain(lrintf(posControl.desiredState.vel.x), -32678, 32767);
            navDesiredVelocity[Y] = constrain(lrintf(posControl.desiredState.vel.y), -32678, 32767);
        }
        else {
            // Position update has not occurred in time (first start or glitch), reset position controller
            resetMulticopterPositionController();
        }
    } else if (bypassPositionController) {
        return;
    }

    rcCommand[PITCH] = pidAngleToRcCommand(posControl.rcAdjustment[PITCH], pidProfile()->max_angle_inclination[FD_PITCH]);
    rcCommand[ROLL] = pidAngleToRcCommand(posControl.rcAdjustment[ROLL], pidProfile()->max_angle_inclination[FD_ROLL]);
}

bool isMulticopterFlying(void)
{
    bool throttleCondition = rcCommand[THROTTLE] > currentBatteryProfile->nav.mc.hover_throttle;
    bool gyroCondition = averageAbsGyroRates() > 7.0f;

    return throttleCondition && gyroCondition;
}

/*-----------------------------------------------------------
 * Multicopter land detector
 *-----------------------------------------------------------*/
#if defined(USE_BARO)
float updateBaroAltitudeRate(float newBaroAltRate, bool updateValue)
{
    static float baroAltRate;
    if (updateValue) {
        baroAltRate = newBaroAltRate;
    }

    return baroAltRate;
}

static bool isLandingGbumpDetected(timeMs_t currentTimeMs)
{
    /* Detection based on G bump at touchdown, falling Baro altitude and throttle below hover.
     * G bump trigger: > 2g then falling back below 1g in < 0.1s.
     * Baro trigger: rate must be -ve at initial trigger g and < -2 m/s when g falls back below 1g
     * Throttle trigger: must be below hover throttle with lower threshold for manual throttle control */

    static timeMs_t gSpikeDetectTimeMs = 0;
    float baroAltRate = updateBaroAltitudeRate(0, false);

    if (!gSpikeDetectTimeMs && acc.accADCf[Z] > 2.0f && baroAltRate < 0.0f) {
        gSpikeDetectTimeMs = currentTimeMs;
    } else if (gSpikeDetectTimeMs) {
        if (currentTimeMs < gSpikeDetectTimeMs + 100) {
            if (acc.accADCf[Z] < 1.0f && baroAltRate < -200.0f) {
                const uint16_t idleThrottle = getThrottleIdleValue();
                const uint16_t hoverThrottleRange = currentBatteryProfile->nav.mc.hover_throttle - idleThrottle;
                return rcCommand[THROTTLE] < idleThrottle + ((navigationInAutomaticThrottleMode() ? 0.8 : 0.5) * hoverThrottleRange);
            }
        } else if (acc.accADCf[Z] <= 1.0f) {
            gSpikeDetectTimeMs = 0;
        }
    }

    return false;
}
#endif

bool isMulticopterLandingDetected(void)
{
    DEBUG_SET(DEBUG_LANDING, 4, 0);
    DEBUG_SET(DEBUG_LANDING, 3, averageAbsGyroRates() * 100);

    const timeMs_t currentTimeMs = millis();

#if defined(USE_BARO)
    if (sensors(SENSOR_BARO) && navConfig()->general.flags.landing_bump_detection && isLandingGbumpDetected(currentTimeMs)) {
        return true;    // Landing flagged immediately if landing bump detected
    }
#endif

    bool throttleIsBelowMidHover = rcCommand[THROTTLE] < (0.5 * (currentBatteryProfile->nav.mc.hover_throttle + getThrottleIdleValue()));

    /* Basic condition to start looking for landing
     * Detection active during Failsafe only if throttle below mid hover throttle
     * and WP mission not active (except landing states).
     * Also active in non autonomous flight modes but only when thottle low */
    bool startCondition = (navGetCurrentStateFlags() & (NAV_CTL_LAND | NAV_CTL_EMERG))
                          || (FLIGHT_MODE(FAILSAFE_MODE) && !FLIGHT_MODE(NAV_WP_MODE) && throttleIsBelowMidHover)
                          || (!navigationIsFlyingAutonomousMode() && throttleStickIsLow());

    static timeMs_t landingDetectorStartedAt;

    if (!startCondition || posControl.flags.resetLandingDetector) {
        landingDetectorStartedAt = 0;
        return posControl.flags.resetLandingDetector = false;
    }

    const float sensitivity = navConfig()->general.land_detect_sensitivity / 5.0f;

    // check vertical and horizontal velocities are low (cm/s)
    bool velCondition = fabsf(navGetCurrentActualPositionAndVelocity()->vel.z) < (MC_LAND_CHECK_VEL_Z_MOVING * sensitivity) &&
                        posControl.actualState.velXY < (MC_LAND_CHECK_VEL_XY_MOVING * sensitivity);
    // check gyro rates are low (degs/s)
    bool gyroCondition = averageAbsGyroRates() < (4.0f * sensitivity);
    DEBUG_SET(DEBUG_LANDING, 2, velCondition);
    DEBUG_SET(DEBUG_LANDING, 3, gyroCondition);

    bool possibleLandingDetected = false;

    if (navGetCurrentStateFlags() & NAV_CTL_LAND) {
        // We have likely landed if throttle is 40 units below average descend throttle
        // We use rcCommandAdjustedThrottle to keep track of NAV corrected throttle (isLandingDetected is executed
        // from processRx() and rcCommand at that moment holds rc input, not adjusted values from NAV core)
        DEBUG_SET(DEBUG_LANDING, 4, 1);

        static int32_t landingThrSum;
        static int32_t landingThrSamples;
        bool isAtMinimalThrust = false;

        if (!landingDetectorStartedAt) {
            landingThrSum = landingThrSamples = 0;
            landingDetectorStartedAt = currentTimeMs;
        }
        
        if (!landingThrSamples) {
            if (currentTimeMs - landingDetectorStartedAt < S2MS(MC_LAND_THR_STABILISE_DELAY)) {   // Wait for 1 second so throttle has stabilized.
                return false;
            } else {
                landingDetectorStartedAt = currentTimeMs;
            }
        }

        landingThrSamples += 1;
        landingThrSum += rcCommandAdjustedThrottle;
        isAtMinimalThrust = rcCommandAdjustedThrottle < (landingThrSum / landingThrSamples - MC_LAND_DESCEND_THROTTLE);

        possibleLandingDetected = isAtMinimalThrust && velCondition;

        DEBUG_SET(DEBUG_LANDING, 6, rcCommandAdjustedThrottle);
        DEBUG_SET(DEBUG_LANDING, 7, landingThrSum / landingThrSamples - MC_LAND_DESCEND_THROTTLE);
    } else {    // non autonomous and emergency landing
        DEBUG_SET(DEBUG_LANDING, 4, 2);
        if (landingDetectorStartedAt) {
            possibleLandingDetected = velCondition && gyroCondition;
        } else {
            landingDetectorStartedAt = currentTimeMs;
            return false;
        }
    }

    // If we have surface sensor (for example sonar) - use it to detect touchdown
    if ((posControl.flags.estAglStatus == EST_TRUSTED) && (posControl.actualState.agl.pos.z >= 0)) {
        // TODO: Come up with a clever way to let sonar increase detection performance, not just add extra safety.
        // TODO: Out of range sonar may give reading that looks like we landed, find a way to check if sonar is healthy.
        // surfaceMin is our ground reference. If we are less than 5cm above the ground - we are likely landed
        possibleLandingDetected = possibleLandingDetected && (posControl.actualState.agl.pos.z <= (posControl.actualState.surfaceMin + MC_LAND_SAFE_SURFACE));
    }

    DEBUG_SET(DEBUG_LANDING, 5, possibleLandingDetected);

    if (possibleLandingDetected) {
        /* Conditions need to be held for fixed safety time + optional extra delay.
         * Fixed time increased if Z velocity invalid to provide extra safety margin against false triggers */
        const uint16_t safetyTime = posControl.flags.estAltStatus == EST_NONE ? 5000 : 1000;
        timeMs_t safetyTimeDelay = safetyTime + navConfig()->general.auto_disarm_delay;
        return currentTimeMs - landingDetectorStartedAt > safetyTimeDelay;
    } else {
        landingDetectorStartedAt = currentTimeMs;
        return false;
    }
}

/*-----------------------------------------------------------
 * Multicopter emergency landing
 *-----------------------------------------------------------*/
static void applyMulticopterEmergencyLandingController(timeUs_t currentTimeUs)
{
    static timeUs_t previousTimePositionUpdate = 0;

    /* Attempt to stabilise */
    rcCommand[YAW] = 0;
    rcCommand[ROLL] = 0;
    rcCommand[PITCH] = 0;

    /* Altitude sensors gone haywire, attempt to land regardless */
    if (posControl.flags.estAltStatus < EST_USABLE) {
        if (failsafeConfig()->failsafe_procedure == FAILSAFE_PROCEDURE_DROP_IT) {
            rcCommand[THROTTLE] = getThrottleIdleValue();
            return;
        }
        rcCommand[THROTTLE] = setDesiredThrottle(currentBatteryProfile->failsafe_throttle, true);
        return;
    }

    // Normal sensor data available, use controlled landing descent
    if (posControl.flags.verticalPositionDataNew) {
        const timeDeltaLarge_t deltaMicrosPositionUpdate = currentTimeUs - previousTimePositionUpdate;
        previousTimePositionUpdate = currentTimeUs;

        // Check if last correction was not too long ago
        if (deltaMicrosPositionUpdate < MAX_POSITION_UPDATE_INTERVAL_US) {
            // target min descent rate 5m above takeoff altitude
            updateClimbRateToAltitudeController(-navConfig()->general.emerg_descent_rate, 500.0f, ROC_TO_ALT_TARGET);
            updateZController(deltaMicrosPositionUpdate);
        }
        else {
            // due to some glitch position update has not occurred in time, reset altitude controller
            resetMulticopterAltitudeController();
        }

        // Indicate that information is no longer usable
        posControl.flags.verticalPositionDataConsumed = true;
    }

    // Update throttle
    rcCommand[THROTTLE] = posControl.rcAdjustment[THROTTLE];

    // Hold position if possible
    if ((posControl.flags.estPosStatus >= EST_USABLE)) {
        applyMulticopterPositionController(currentTimeUs);
    }
}

/*-----------------------------------------------------------
 * Calculate loiter target based on current position and velocity
 *-----------------------------------------------------------*/
void calculateMulticopterInitialHoldPosition(fpVector3_t * pos)
{
    const float stoppingDistanceX = navGetCurrentActualPositionAndVelocity()->vel.x * posControl.posDecelerationTime;
    const float stoppingDistanceY = navGetCurrentActualPositionAndVelocity()->vel.y * posControl.posDecelerationTime;

    pos->x = navGetCurrentActualPositionAndVelocity()->pos.x + stoppingDistanceX;
    pos->y = navGetCurrentActualPositionAndVelocity()->pos.y + stoppingDistanceY;
}

void resetMulticopterHeadingController(void)
{
    updateHeadingHoldTarget(CENTIDEGREES_TO_DEGREES(posControl.actualState.yaw));
}

static void applyMulticopterHeadingController(void)
{
    if (FLIGHT_MODE(NAV_COURSE_HOLD_MODE)) {    // heading set by Nav during Course Hold so disable yaw stick input
        rcCommand[YAW] = 0;
    }

    updateHeadingHoldTarget(CENTIDEGREES_TO_DEGREES(posControl.desiredState.yaw));
}

void applyMulticopterNavigationController(navigationFSMStateFlags_t navStateFlags, timeUs_t currentTimeUs)
{
    if (navStateFlags & NAV_CTL_EMERG) {
        applyMulticopterEmergencyLandingController(currentTimeUs);
    } else {
        if (navStateFlags & NAV_CTL_ALT) {
            applyMulticopterAltitudeController(currentTimeUs);
        }

        if (navStateFlags & NAV_CTL_POS) {
            applyMulticopterPositionController(currentTimeUs);
        }

        if (navStateFlags & NAV_CTL_YAW) {
            applyMulticopterHeadingController();
        }
    }
}