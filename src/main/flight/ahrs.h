/*
 * This file is part of iNav.
 *
 * iNav is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * iNav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with iNav.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/axis.h"
#include "common/maths.h"
#include "common/vector.h"
#include "common/quaternion.h"
#include "common/time.h"
#include "config/parameter_group.h"

typedef union {
    int16_t raw[XYZ_AXIS_COUNT];
    struct {
        int16_t roll;
        int16_t pitch;
        int16_t yaw;
    } values;
} attitudeEulerAngles_t;

typedef struct ahrsConfig_s {
    uint16_t dcm_kp_acc;
    uint16_t dcm_kp_mag;
    uint16_t dcm_gps_gain;
    uint8_t small_angle;
} ahrsConfig_t;

PG_DECLARE(ahrsConfig_t, ahrsConfig);

extern fpVector3_t imuMeasuredAccelBF;    // cm/s/s
extern fpVector3_t imuMeasuredRotationBF; // rad/s
extern attitudeEulerAngles_t attitude;

void ahrsInit(void);

void ahrsSetMagneticDeclination(float declinationDeg);
void ahrsReset(bool recover_eulers);
void ahrsUpdate(timeUs_t currentTimeUs);
bool ahrsIsHealthy(void);
bool isAhrsHeadingValid(void);
float ahrsGetTiltAngle(void);

void updateWindEstimator(void);
// Wind velocity vectors in cm/s relative to the earth frame
float getEstimatedWindSpeed(int axis);
// Returns the horizontal wind velocity as a magnitude in cm/s and, optionally, its heading in EF in 0.01deg ([0, 360 * 100)).
float getEstimatedHorizontalWindSpeed(uint16_t *angle);

void ahrsTransformVectorBodyToEarth(fpVector3_t * v);
void ahrsTransformVectorEarthToBody(fpVector3_t * v);

float ahrsGetCosYaw(void);
float ahrsGetSinYaw(void);