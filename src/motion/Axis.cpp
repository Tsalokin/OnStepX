// -----------------------------------------------------------------------------------
// Axis motion control

#include "../Common.h"
#include "../tasks/OnTask.h"
extern Tasks tasks;
#include "../telescope/Telescope.h"
#include "Axis.h"
#include "../lib/sense/Sense.h"

extern unsigned long periodSubMicros;

void Axis::enable(bool value) {
  if (pins.enable != OFF) {
    if (value) digitalWrite(pins.enable, invertEnable?HIGH:LOW); else digitalWrite(pins.enable, invertEnable?LOW:HIGH);
  }
  enabled = value;
}

bool Axis::isEnabled() {
  return enabled;
}

double Axis::getStepsPerMeasure() {
  return settings.stepsPerMeasure;
}

int Axis::getStepsPerStepSlewing() {
  return stepsPerStepSlewing;
}

void Axis::setMotorCoordinate(double value) {
  long steps = lround(value*settings.stepsPerMeasure);
  setMotorCoordinateSteps(steps);
}

double Axis::getMotorCoordinate() {
  noInterrupts();
  long steps = motorSteps + backlashSteps;
  interrupts();
  return steps/settings.stepsPerMeasure;
}

void Axis::setMotorCoordinateSteps(long value) {
  indexSteps    = 0;
  noInterrupts();
  motorSteps    = value;
  targetSteps   = value;
  backlashSteps = 0;
  interrupts();
}

long Axis::getMotorCoordinateSteps() {
  noInterrupts();
  long steps = motorSteps + backlashSteps;
  interrupts();
  return steps;
}

long Axis::getInstrumentCoordinateSteps() {
  noInterrupts();
  long steps = motorSteps + indexSteps;
  interrupts();
  return steps;
}

void Axis::setInstrumentCoordinate(double value) {
  long steps = value*settings.stepsPerMeasure;
  noInterrupts();
  indexSteps = steps - motorSteps;
  interrupts();
}

double Axis::getInstrumentCoordinate() {
  noInterrupts();
  long steps = motorSteps + indexSteps;
  interrupts();
  return steps/settings.stepsPerMeasure;
}

void Axis::markOriginCoordinate() {
  noInterrupts();
  originSteps = motorSteps;
  interrupts();
}

void Axis::setTargetCoordinate(double value) {
  target = value;
  long steps = lround(value*settings.stepsPerMeasure);
  noInterrupts();
  targetSteps = steps - indexSteps;
  interrupts();
}

void Axis::setTargetCoordinateSteps(long value) {
  target = value/settings.stepsPerMeasure;
  noInterrupts();
  targetSteps = value - indexSteps;
  interrupts();
}

double Axis::getTargetCoordinate() {
  noInterrupts();
  long steps = targetSteps + indexSteps;
  interrupts();
  return steps/settings.stepsPerMeasure;
}

long Axis::getTargetCoordinateSteps() {
  noInterrupts();
  long steps = targetSteps + indexSteps;
  interrupts();
  return steps;
}

void Axis::incrementTargetCoordinate(double value) {
  target += value;
  setTargetCoordinate(value);
}

bool Axis::nearTarget() {
  return labs(motorSteps - targetSteps) <= step * 2;
}

double Axis::getOriginOrTargetDistance() {
  noInterrupts();
  long steps = motorSteps;
  interrupts();
  long distanceOrigin = labs(originSteps - steps);
  long distanceTarget = labs(targetSteps - steps);
  if (distanceOrigin < distanceTarget) return distanceOrigin/settings.stepsPerMeasure; else return distanceTarget/settings.stepsPerMeasure;
}

void Axis::setFrequencyBase(float frequency) {
  baseFreq = frequency;
}

void Axis::setFrequencyMax(float frequency) {
  maxFreq = frequency;
  if (frequency != 0.0) minPeriodMicros = 1000000.0F/((maxFreq + baseFreq)*settings.stepsPerMeasure); else minPeriodMicros = 0.0F;
}

void Axis::setSlewAccelerationRate(float mpsps) {
  slewMpspcs = mpsps/100.0F;
}

void Axis::setSlewAccelerationRateAbort(float mpsps) {
  abortMpspcs = mpsps/100.0F;
}

void Axis::autoSlewRateByDistance(float distance) {
  autoRate = AR_RATE_BY_DISTANCE;
  slewAccelerationDistance = distance;
  driver.modeDecaySlewing();
  VF("MSG: Axis::autoSlewRateByDistance(); Axis"); V(axisNumber); VLF(" slew started");
}

void Axis::autoSlewRateByDistanceStop() {
  driver.modeDecayTracking();
  autoRate = AR_NONE;
}

void Axis::autoSlew(Direction direction) {
  if (direction == DIR_NONE) return;
  if (autoRate == AR_NONE) {
    driver.modeDecaySlewing();
    VF("MSG: Axis::autoSlew(); Axis"); V(axisNumber); VLF(" slew started");
  }
  if (direction == DIR_FORWARD) autoRate = AR_RATE_BY_TIME_FORWARD; else autoRate = AR_RATE_BY_TIME_REVERSE;
}

void Axis::autoSlewStop() {
  if (autoRate != AR_NONE && autoRate != AR_RATE_BY_TIME_ABORT) {
    VF("MSG: Axis::autoSlewStop(); Axis"); V(axisNumber); VLF(" slew stopping");
    autoRate = AR_RATE_BY_TIME_END;
    poll();
  }
}

void Axis::autoSlewAbort() {
  if (autoRate != AR_NONE) {
    VF("MSG: Axis::autoSlewAbort(); Axis"); V(axisNumber); VLF(" slew aborting");
    autoRate = AR_RATE_BY_TIME_ABORT;
    poll();
  }
}

bool Axis::autoSlewActive() {
  return autoRate != AR_NONE;  
}

void Axis::poll() {
  // make sure we're ready
  if (axisNumber == 0) return;

  // check for limits
  error.minLimitSensed = senses.read(hMinSense);
  error.maxLimitSensed = senses.read(hMaxSense);

  // automatically abort slew and stop tracking
  if (autoRate != AR_RATE_BY_TIME_ABORT) {
    if (lastPeriod != 0 && trackingStep == -1 && motionReverseError()) {
      VLF("MSG: Axis::poll(); motion reverse err");
      autoSlewAbort();
      if (tracking) { tracking = false; VLF("MSG: Axis::poll(); tracking stopped"); }
      return;
    }
    if (lastPeriod != 0 && trackingStep == 1 && motionForwardError()) {
      VLF("MSG: Axis::poll(); motion forward err");
      autoSlewAbort();
      if (tracking) { tracking = false; VLF("MSG: Axis::poll(); tracking stopped"); }
      return;
    }
  }

  if (autoRate != AR_NONE) {
    // acceleration
    if (autoRate == AR_RATE_BY_DISTANCE) {
      freq = (getOriginOrTargetDistance()/slewAccelerationDistance)*maxFreq + backlashFreq;
      if (freq < backlashFreq) freq = backlashFreq;
      if (freq > maxFreq) freq = maxFreq;
      if (direction == DIR_REVERSE) freq = -freq;
    } else
    if (autoRate == AR_RATE_BY_TIME_FORWARD) {
      freq += slewMpspcs;
      if (freq > maxFreq) freq = maxFreq;
    } else
    if (autoRate == AR_RATE_BY_TIME_REVERSE) {
      freq -= slewMpspcs;
      if (freq < -maxFreq) freq = -maxFreq;
    } else
    if (autoRate == AR_RATE_BY_TIME_END) {
      if (freq > slewMpspcs) freq -= slewMpspcs; else if (freq < -slewMpspcs) freq += slewMpspcs; else freq = 0.0F;
      if (fabs(freq) <= slewMpspcs) {
        driver.modeDecayTracking();
        autoRate = AR_NONE;
        freq = 0.0F;
        VF("MSG: Axis::poll(); axis"); V(axisNumber); VLF(" slew stopped");
      }
    } else
    if (autoRate == AR_RATE_BY_TIME_ABORT) {
      if (freq > abortMpspcs) freq -= abortMpspcs; else if (freq < -abortMpspcs) freq += abortMpspcs; else freq = 0.0F;
      if (fabs(freq) <= abortMpspcs) {
        driver.modeDecayTracking();
        autoRate = AR_NONE;
        freq = 0.0F;
        VF("MSG: Axis::poll(); axis"); V(axisNumber); VLF(" slew aborted");
      }
    } else freq = 0.0F;

    // ISR swap and microstep mode switching
    if (microstepModeControl == MMC_SLEWING) {
      if (fabs(freq) <= backlashFreq*1.2F || autoRate == AR_NONE) {
        if (driver.modeSwitchAllowed()) {
          VF("MSG: Axis::poll(); axis"); V(axisNumber); VLF(" mode switch tracking set");
          driver.modeMicrostepTracking();
        }
        microstepModeControl = MMC_TRACKING;
        enableMoveFast(false);
        VF("MSG: Axis::poll(); axis"); V(axisNumber); VF(" high speed ISR swapped out at "); VL(radToDeg(freq));
      }
    } else {
      if (fabs(freq) > backlashFreq*1.2F) {
        if (driver.modeSwitchAllowed()) {
          if (microstepModeControl == MMC_TRACKING) {
            microstepModeControl = MMC_SLEWING_REQUEST;
            VF("MSG: Axis::poll(); axis"); V(axisNumber); VLF(" mode switch slewing requested");
            return;
          } else
          if (microstepModeControl != MMC_SLEWING_READY) return;
          VF("MSG: Axis::poll(); axis"); V(axisNumber); VLF(" mode switch slewing set");
          slewStep = driver.modeMicrostepSlewing();
        }
        microstepModeControl = MMC_SLEWING;
        enableMoveFast(true);
        VF("MSG: Axis::poll(); axis"); V(axisNumber); VF(" high speed ISR swapped in at "); VL(radToDeg(freq));
      }
    }
  } else freq = 0.0F;

  // apply the composite or backlash frequency as required
  float f = freq;
  if (tracking) f += baseFreq;
  if (microstepModeControl == MMC_SLEWING) {
    setFrequency(f/slewStep);
  } else {
    if (inBacklash()) { if (f >= 0.0) f = backlashFreq; else f = -backlashFreq; }
    setFrequency(f);
  }

  // refresh the driver status
  driver.updateStatus();
}

void Axis::setFrequency(float frequency) {
  if (frequency < 0.0F) {
    frequency = -frequency;
    noInterrupts(); trackingStep = -1; interrupts();
  } else {
    noInterrupts(); trackingStep =  1; interrupts();
  }

  lastFreq = frequency;
  // frequency in measures per second to period in microsecond counts per step
  float period = 1000000.0F/(frequency*settings.stepsPerMeasure);
  if (period < minPeriodMicros) period = minPeriodMicros;
  if (STEP_WAVE_FORM == SQUARE) period /= 2.0F;
  if (!isnan(period) && fabs(period) <= 134000000.0F) {
    // convert microsecond counts to sub-microsecond counts
    period *= 16.0F;
    lastPeriodSet = (unsigned long)lround(period);

    // adjust period for MCU clock inaccuracy
    period *= (SIDEREAL_PERIOD/periodSubMicros);
    // if this is the active period, just return
    if (lastPeriod == (unsigned long)lround(period)) return;
    lastPeriod = (unsigned long)lround(period);
  } else { period = 0.0; lastPeriodSet = 0; lastPeriod = 0; }
  tasks.setPeriodSubMicros(taskHandle, lastPeriod);
}

float Axis::getFrequency() {
  return getFrequencySteps()/settings.stepsPerMeasure;
}

float Axis::getFrequencySteps() {
  if (lastPeriodSet == 0) return 0;
  #if STEP_WAVE_FORM == SQUARE
    return 16000000.0F/(lastPeriodSet*2.0F);
  #else
    return 16000000.0F/lastPeriodSet;
  #endif
}

void Axis::setTracking(bool state) {
  this->tracking = state;
}

bool Axis::getTracking() {
  return tracking;
}

void Axis::setBacklash(float value) {
  noInterrupts();
  backlashAmountSteps = round(value*settings.stepsPerMeasure);
  interrupts();
}

float Axis::getBacklash() {
  noInterrupts();
  uint16_t b = backlashSteps;
  interrupts();
  return b/settings.stepsPerMeasure;
}

bool Axis::inBacklash() {
  noInterrupts();
  uint16_t b = backlashSteps;
  interrupts();
  if (b <= 0 || b >= backlashAmountSteps) return false; else return true;
}

void Axis::disableBacklash() {
  noInterrupts();
  backlashStepsStore = backlashSteps;
  motorSteps += backlashSteps;
  backlashSteps = 0;
  interrupts();
}

void Axis::enableBacklash() {
  noInterrupts();
  backlashSteps = backlashStepsStore;
  motorSteps -= backlashSteps;
  interrupts();
  backlashStepsStore = 0;
}

void Axis::setMotionLimitsCheck(bool state) {
  limitsCheck = state;
}

bool Axis::motionForwardError() {
  return driver.getStatus().fault ||
         (limitsCheck && getInstrumentCoordinate() > settings.limits.max) ||
         error.maxLimitSensed;
}

bool Axis::motionReverseError() {
  return driver.getStatus().fault ||
         (limitsCheck && getInstrumentCoordinate() < settings.limits.min) ||
         error.minLimitSensed;
}

bool Axis::motionError() {
  return motionForwardError() || motionReverseError();
}
