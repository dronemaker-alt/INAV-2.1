#pragma once

#include "config/parameter_group.h"
#include "flight/failsafe.h"
#include "flight/mixer.h"
#include "flight/servos.h"

#ifndef MAX_MIXER_PROFILE_COUNT
#define MAX_MIXER_PROFILE_COUNT 2
#endif

typedef struct mixerConfig_s {
    int8_t motorDirectionInverted;
    uint8_t platformType;
    bool hasFlaps;
    int16_t appliedMixerPreset;
    uint8_t outputMode;
    bool motorstopOnLow;
    bool PIDProfileLinking;
    bool switchOnFSRTH;
    bool switchOnFSLand;
    int16_t switchOnFSStabilizationTimer;
    int16_t switchOnFSTransitionTimer;
} mixerConfig_t;
typedef struct mixerProfile_s {
    mixerConfig_t mixer_config;
    motorMixer_t MotorMixers[MAX_SUPPORTED_MOTORS];
    servoMixer_t ServoMixers[MAX_SERVO_RULES];
} mixerProfile_t;

PG_DECLARE_ARRAY(mixerProfile_t, MAX_MIXER_PROFILE_COUNT, mixerProfiles);


//mixerProfile Automated Transition PHASE
typedef enum {
    MIXERAT_PHASE_IDLE,
    MIXERAT_PHASE_TRANSITION_INITIALIZE,
    MIXERAT_PHASE_TRANSITIONING,
    MIXERAT_PHASE_DONE,
} mixerProfileATState_t;

typedef struct mixerProfileAT_s {
    mixerProfileATState_t phase;
    bool transitionInputMixing;
    timeMs_t transitionStartTime;
    timeMs_t transitionStabEndTime;
    timeMs_t transitionTransEndTime;
    bool lastTransitionInputMixing;
    bool lastMixerProfile;
} mixerProfileAT_t;
extern mixerProfileAT_t mixerProfileAT;
bool mixerATRequiresAngleMode(void);
bool mixerATUpdateState(failsafePhase_e required_fs_phase);

extern mixerConfig_t currentMixerConfig;
extern int currentMixerProfileIndex;
extern bool isMixerTransitionMixing;
#define mixerConfig() (&(mixerProfiles(systemConfig()->current_mixer_profile_index)->mixer_config))
#define mixerConfigMutable() ((mixerConfig_t *) mixerConfig())

#define primaryMotorMixer(_index) (&(mixerProfiles(systemConfig()->current_mixer_profile_index)->MotorMixers)[_index])
#define primaryMotorMixerMutable(_index) ((motorMixer_t *)primaryMotorMixer(_index))
#define customServoMixers(_index) (&(mixerProfiles(systemConfig()->current_mixer_profile_index)->ServoMixers)[_index])
#define customServoMixersMutable(_index) ((servoMixer_t *)customServoMixers(_index))

static inline const mixerProfile_t* mixerProfiles_CopyArray_by_index(int _index) { return &mixerProfiles_CopyArray[_index]; }
#define primaryMotorMixer_CopyArray() (mixerProfiles_CopyArray_by_index(systemConfig()->current_mixer_profile_index)->MotorMixers)
#define customServoMixers_CopyArray() (mixerProfiles_CopyArray_by_index(systemConfig()->current_mixer_profile_index)->ServoMixers)

#define mixerConfigByIndex(index) (&(mixerProfiles(index)->mixer_config))
#define mixerMotorMixersByIndex(index) (&(mixerProfiles(index)->MotorMixers))
#define mixerServoMixersByIndex(index) (&(mixerProfiles(index)->ServoMixers))

bool outputProfileHotSwitch(int profile_index);
bool checkMixerProfileHotSwitchAvalibility(void);
void mixerConfigInit(void);
void outputProfileUpdateTask(timeUs_t currentTimeUs);