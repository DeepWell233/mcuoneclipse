/*
 * Copyright (c) 2020, Erich Styger
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "platform.h"
#if PL_CONFIG_USE_STEPPER
#include "stepperConfig.h"
#include "stepper.h"
#if PL_CONFIG_USE_X12_STEPPER
  #include "McuX12_017.h"
#elif PL_CONFIG_USE_ULN2003
  #include "McuULN2003.h"
#elif PL_CONFIG_USE_STEPPER_EMUL
  #include "NeoStepperRing.h"
#endif
#if PL_CONFIG_USE_MAG_SENSOR
  #include "magnets.h"
#endif
#if PL_CONFIG_USE_NVMC
  #include "nvmc.h"
#endif
#if PL_CONFIG_USE_WDT
  #include "watchdog.h"
#endif
#include "McuRTOS.h"
#include "McuUtility.h"
#include "McuWait.h"
#include "leds.h"
#include "Shell.h"
#include "matrix.h"
#if McuLib_CONFIG_CPU_IS_LPC
  #include "fsl_ctimer.h"
  #include "fsl_sctimer.h"
#elif McuLib_CONFIG_CPU_IS_KINETIS
  #include "fsl_pit.h"
#endif
#include "StepperBoard.h"

#define STEPPER_HAND_ZERO_DELAY     (6)
#define STEPPER_CMD_QUEUE_LENGTH    (8) /* maximum number of items in stepper command queue */

typedef enum {
  SCT_CHANNEL_MASK_0 = (1<<0),
  SCT_CHANNEL_MASK_1 = (1<<1),
  SCT_CHANNEL_MASK_2 = (1<<2),
  SCT_CHANNEL_MASK_3 = (1<<3),
  SCT_CHANNEL_MASK_4 = (1<<4),
  SCT_CHANNEL_MASK_5 = (1<<5),
  SCT_CHANNEL_MASK_6 = (1<<6),
  SCT_CHANNEL_MASK_7 = (1<<7)
} SCT_CHANNEL_MASK_e;


/* default configuration, used for initializing the config */
static const STEPPER_Config_t defaultConfig =
{
  .device = NULL, /* motor or LED device */
  .stepFn = NULL, /* callback to increment or decrement steps */
};

/* device for a single LED ring */
typedef struct {
  void *device; /* point to the actual motor device */
  void (*stepFn)(void *device, int step); /* function pointer to perform a single step forward (1) or backward (-1) */
  int32_t pos; /* current position */
  int32_t doSteps; /* != 0: steps to do */
  int16_t delay; /* shortest delay: 0 */
  int16_t delayCntr; /* in the range of delay..0, step will be done at counter of 0 */
  int32_t accelStepCntr; /* current counter of steps since start */
  bool speedup, slowdown;
  QueueHandle_t queue; /* queue for the motor */
} STEPPER_Device_t;

void STEPPER_GetDefaultConfig(STEPPER_Config_t *config) {
  memcpy(config, &defaultConfig, sizeof(*config));
}

STEPPER_Handle_t STEPPER_(STEPPER_Handle_t device) {
#if STEPPER_CONFIG_USE_FREERTOS_HEAP
  vPortFree(device);
#else
  free(device);
#endif
  return NULL;
}

STEPPER_Handle_t STEPPER_InitDevice(STEPPER_Config_t *config) {
  STEPPER_Device_t *handle;

#if STEPPER_CONFIG_USE_FREERTOS_HEAP
  handle = (STEPPER_Device_t*)pvPortMalloc(sizeof(STEPPER_Device_t)); /* get a new device descriptor */
#else
  handle = (STEPPER_Device_t*)malloc(sizeof(STEPPER_Device_t)); /* get a new device descriptor */
#endif
  assert(handle!=NULL);
  if (handle!=NULL) { /* if malloc failed, will return NULL pointer */
    memset(handle, 0, sizeof(STEPPER_Device_t)); /* init all fields */
    handle->device = config->device;
    handle->stepFn = config->stepFn;
    handle->pos = 0;
    handle->doSteps = 0;
    handle->delay = 0;
    handle->delayCntr = 0;
    handle->speedup = false;
    handle->slowdown = false;
    handle->queue = xQueueCreate(STEPPER_CMD_QUEUE_LENGTH, sizeof(uint8_t*));
    if (handle->queue==NULL) {
      for(;;){} /* out of memory? */
    }
    vQueueAddToRegistry(handle->queue, "Squeue");
  }
  return handle;
}

#if McuLib_CONFIG_CPU_IS_LPC  /* LPC845-BRK */
  #define STEPPER_START_TIMER()        SCTIMER_StartTimer(SCT0, kSCTIMER_Counter_L)
  #define STEPPER_STOP_TIMER()         SCTIMER_StopTimer(SCT0, kSCTIMER_Counter_L)
#elif McuLib_CONFIG_CPU_IS_KINETIS
  #define PIT_BASEADDR       PIT
  #define PIT_SOURCE_CLOCK   CLOCK_GetFreq(kCLOCK_BusClk)
  #define PIT_CHANNEL        kPIT_Chnl_0
  #define PIT_HANDLER        PIT0_IRQHandler
  #define PIT_IRQ_ID         PIT0_IRQn

  #define STEPPER_START_TIMER()        PIT_StartTimer(PIT_BASEADDR, PIT_CHANNEL)
  #define STEPPER_STOP_TIMER()         PIT_StopTimer(PIT_BASEADDR, PIT_CHANNEL)
#endif
#define STEPPER_ACCEL_HIGHEST_POS   (300)

void STEPPER_StopTimer(void) {
  STEPPER_STOP_TIMER();
}

void STEPPER_StartTimer(void) {
  STEPPER_START_TIMER();
}

static void AccelDelay(STEPPER_Device_t *mot, int32_t steps) {
  if (steps<=STEPPER_ACCEL_HIGHEST_POS) {
    if (steps<=50) {
      mot->delayCntr += 10;
    } else if (steps<=100) {
      mot->delayCntr += 7;
    } else if (steps<=150) {
      mot->delayCntr += 5;
    } else if (steps<=250) {
      mot->delayCntr += 3;
    } else if (steps<=STEPPER_ACCEL_HIGHEST_POS) {
      mot->delayCntr += 1;
    }
  }
}

bool STEPPER_IsIdle(STEPPER_Handle_t stepper) {
  STEPPER_Device_t *mot = (STEPPER_Device_t*)stepper;
  return mot->doSteps==0;
}

bool STEPPER_TimerClockCallback(STEPPER_Handle_t stepper) {
  STEPPER_Device_t *mot = (STEPPER_Device_t*)stepper;

  if (mot->delayCntr==0) { /* delay expired */
    if (mot->doSteps!=0) { /* a step to do */
      if (mot->doSteps > 0) {
        mot->pos++;
        mot->stepFn(mot->device, 1);
        mot->doSteps--;
      } else if (mot->doSteps < 0)  {
        mot->pos--;
        mot->stepFn(mot->device, -1);
        mot->doSteps++;
      }
      mot->delayCntr = mot->delay; /* reload delay counter */
      if (mot->speedup || mot->slowdown) {
        int32_t stepsToGo;;

        stepsToGo = mot->doSteps;
        if (stepsToGo<0) { /* make it positive */
          stepsToGo = -stepsToGo;
        }
        if (mot->speedup && stepsToGo>STEPPER_ACCEL_HIGHEST_POS) { /* accelerate */
          if (mot->accelStepCntr<=STEPPER_ACCEL_HIGHEST_POS) {
            mot->accelStepCntr++;
          }
          AccelDelay(mot, mot->accelStepCntr);
        } else if (mot->slowdown && stepsToGo<STEPPER_ACCEL_HIGHEST_POS) { /* slow down */
          if (mot->accelStepCntr>=0) {
            mot->accelStepCntr--;
          }
          mot->accelStepCntr--;
          AccelDelay(mot, mot->accelStepCntr);
        }
      }
    } else {
      return false; /* no work to do */
    }
  } else {
    mot->delayCntr--; /* decrement delay counter */
  }
  return true; /* still work to do */
}

#if McuLib_CONFIG_CPU_IS_LPC  /* LPC845-BRK */
static void SCTIMER_Handler0(void) {
  uint32_t flags;

  flags = SCTIMER_GetStatusFlags(SCT0);
  if (flags & SCT_CHANNEL_MASK_0) {
    SCTIMER_ClearStatusFlags(SCT0, SCT_CHANNEL_MASK_0); /* Clear interrupt flag */
    STEPPER_TimerCallback();
  }
}
#elif McuLib_CONFIG_CPU_IS_KINETIS
void PIT_HANDLER(void) {
  PIT_ClearStatusFlags(PIT_BASEADDR, PIT_CHANNEL, kPIT_TimerFlag);
#if PL_CONFIG_USE_MATRIX
  MATRIX_TimerCallback();
#else
  STEPPER_TimerCallback();
#endif
  __DSB();
}
#endif

#if McuLib_CONFIG_CPU_IS_LPC  /* LPC845-BRK */
static void Timer_Init(void) {
  uint32_t eventNumberOutput = 0;
  sctimer_config_t sctimerInfo;
  uint32_t matchValue;
  status_t status;

  SCTIMER_GetDefaultConfig(&sctimerInfo);
  SCTIMER_Init(SCT0, &sctimerInfo);
  matchValue = USEC_TO_COUNT(200, CLOCK_GetFreq(kCLOCK_CoreSysClk));
  status = SCTIMER_CreateAndScheduleEvent(SCT0, kSCTIMER_MatchEventOnly, matchValue, 0 /* dummy I/O */, kSCTIMER_Counter_L /* dummy */, &eventNumberOutput);
  if (status==kStatus_Fail || eventNumberOutput!=0) {
    for(;;) {} /* should not happen! */
  }
  SCTIMER_SetupCounterLimitAction(SCT0, kSCTIMER_Counter_L, eventNumberOutput);
  SCTIMER_SetCallback(SCT0, SCTIMER_Handler0, eventNumberOutput);
  SCTIMER_EnableInterrupts(SCT0, (1<<eventNumberOutput));
  NVIC_SetPriority(SCT0_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY+1); /* less urgent than RS-485 Rx interrupt! */
  EnableIRQ(SCT0_IRQn); /* Enable at the NVIC */
}
#elif McuLib_CONFIG_CPU_IS_KINETIS
static void Timer_Init(void) {
  pit_config_t config;
  uint32_t delta = 2;

  PIT_GetDefaultConfig(&config);
  config.enableRunInDebug = false;
  PIT_Init(PIT_BASEADDR, &config);
  /* note: the LPC is running on 200us, but the K22 is a bit faster, so running slower */
  PIT_SetTimerPeriod(PIT_BASEADDR, PIT_CHANNEL, USEC_TO_COUNT(200U+delta, PIT_SOURCE_CLOCK));
  PIT_EnableInterrupts(PIT_BASEADDR, PIT_CHANNEL, kPIT_TimerInterruptEnable);
  NVIC_SetPriority(PIT_IRQ_ID, 0);
  EnableIRQ(PIT_IRQ_ID);
}
#endif

/*!
 * \brief Move clock to absolute degree position
 */
void STEPPER_MoveClockDegreeAbs(STEPPER_Handle_t stepper, int32_t degree, STEPPER_MoveMode_e mode, uint8_t delay, bool speedUp, bool slowDown) {
  int32_t steps, currPos, targetPos;
  STEPPER_Device_t *device = (STEPPER_Device_t*)stepper;

  if (degree<0) {
	  degree = 360+degree;
  }
  degree %= 360;
  targetPos = (STEPPER_CLOCK_360_STEPS*degree)/360;
  currPos = device->pos;
  currPos %= STEPPER_CLOCK_360_STEPS;
  if (currPos<0) { /* make it positive */
    currPos = STEPPER_CLOCK_360_STEPS+currPos;
  }
  if (mode==STEPPER_MOVE_MODE_CW) {
    steps = targetPos-currPos;
    if (steps<0) {
      steps = STEPPER_CLOCK_360_STEPS+steps;
    }
  } else if (mode==STEPPER_MOVE_MODE_CCW) {
    steps = targetPos-currPos;
    if (steps>0) {
      steps = -STEPPER_CLOCK_360_STEPS+steps;
    }
  } else { /* STEPPER_MOVE_MODE_SHORT */
    steps = targetPos-currPos;
    if (steps>STEPPER_CLOCK_360_STEPS/2) {
      steps = -(STEPPER_CLOCK_360_STEPS-steps);
    } else if (steps < -(STEPPER_CLOCK_360_STEPS/2)) {
      steps = -(-STEPPER_CLOCK_360_STEPS-steps);
    }
  }
  device->doSteps = steps;
  device->accelStepCntr = 0;
  device->delay = delay;
  device->speedup = speedUp;
  device->slowdown = slowDown;
}

void STEPPER_MoveMotorStepsRel(STEPPER_Handle_t stepper, int32_t steps, uint16_t delay) {
  STEPPER_Device_t *device = (STEPPER_Device_t*)stepper;

  device->doSteps = steps;
  device->accelStepCntr = 0;
  device->delay = delay;
}

/*!
 * \brief Move clock by relative degree
 */
void STEPPER_MoveMotorDegreeRel(STEPPER_Handle_t stepper, int32_t degree, uint16_t delay) {
  int32_t steps;

  if (degree>=0) {
    steps = (STEPPER_CLOCK_360_STEPS*degree)/360;
  } else {
    steps = -(STEPPER_CLOCK_360_STEPS*-degree)/360;
  }
  STEPPER_MoveMotorStepsRel(stepper, steps, delay);
}

/*!
 * \brief Move clock by relative degree
 */
void STEPPER_MoveClockDegreeRel(STEPPER_Handle_t stepper, int32_t degree, STEPPER_MoveMode_e mode, uint8_t delay, bool speedUp, bool slowDown) {
  STEPPER_Device_t *device = (STEPPER_Device_t*)stepper;
  int32_t steps;

  if (degree>=0) {
    steps = (STEPPER_CLOCK_360_STEPS*degree)/360;
  } else {
    steps = -(STEPPER_CLOCK_360_STEPS*-degree)/360;
  }
  device->doSteps = steps;
  device->accelStepCntr = 0;
  device->delay = delay;
  device->speedup = speedUp;
  device->slowdown = slowDown;
}

void *STEPPER_GetDevice(STEPPER_Handle_t stepper) {
  STEPPER_Device_t *device = (STEPPER_Device_t*)stepper;

  return device->device;
}

void STEPPER_Deinit(void) {
#if PL_CONFIG_USE_X12_STEPPER
  /* having two IC's for the motor driver/device */
  McuX12_017_DeinitDevice(STEPPER_Clocks[0].mot[0].device);
  McuX12_017_DeinitDevice(STEPPER_Clocks[2].mot[0].device);
#endif
}

#if PL_CONFIG_USE_MAG_SENSOR
uint8_t STEPPER_MoveHandOnSensor(STEPPER_Motor_t *motors[], size_t nofMotors, bool onSensor, int32_t stepSize, int32_t timeoutms, uint32_t waitms, uint16_t delay) {
  uint8_t res = ERR_OK;
  bool done;

  /* move hand over sensor */
  for(;;) { /* breaks */
    done = true;
    for(int i=0; i<nofMotors; i++) { /* check if all motors are on sensor */
      if (MAG_IsTriggered(motors[i]->mag)!=onSensor) {
        done = false; /* not yet */
        break;
      }
    }
    if (done || timeoutms<0) { /* all hands on sensor */
      break;
    }
    for(int i=0; i<nofMotors; i++) {
      if (MAG_IsTriggered(motors[i]->mag)!=onSensor) {
        STEPPER_MoveMotorStepsRel(motors[i], stepSize, delay); /* make by 1 degree */
      }
    } /* for */
    STEPPER_MoveAndWait(waitms);
    timeoutms -= waitms;
  }
  if (timeoutms<0) {
    res = ERR_UNDERFLOW;
  }
  return res;
}
#endif

#if PL_CONFIG_USE_MAG_SENSOR
void STEPPER_MoveByOffset(STEPPER_Motor_t *motors[], int16_t offsets[], size_t nofMotors, uint16_t delay) {
  /* here all hands are on the sensor: adjust with offset */
   for(int i=0; i<nofMotors; i++) {
     STEPPER_MoveMotorStepsRel(motors[i], offsets[i], delay);
   } /* for */
   STEPPER_MoveAndWait(10);
}

void STEPPER_SetZeroPosition(STEPPER_Motor_t *motors[], size_t nofMotors) {
  /* set zero position */
  for(int i=0; i<nofMotors; i++) {
    X12_017_SetPos(motors[i]->device, motors[i]->mot, 0);
  }
}

uint8_t STEPPER_ZeroHand(STEPPER_Motor_t *motors[], int16_t offsets[], size_t nofMotors, uint16_t delay) {
  uint8_t res = ERR_OK;

  /* if hand is on sensor: move hand out of the sensor area */
  for(int i=0; i<nofMotors; i++) {
    if (MAG_IsTriggered(motors[i]->mag)) { /* hand on sensor? */
      STEPPER_MoveMotorDegreeRel(motors[i], 90, delay);
    }
  } /* for */
  STEPPER_MoveAndWait(10);

  /* move forward in larger steps to find sensor */
  if (STEPPER_MoveHandOnSensor(motors, nofMotors, true, 10, 10000, 10, delay)!=ERR_OK) {
    res = ERR_FAILED;
  }

  /* step back in micro-steps just to leave the sensor */
  if (STEPPER_MoveHandOnSensor(motors, nofMotors, false, -1, 10000, 10, delay)!=ERR_OK) {
    res = ERR_FAILED;
  }

  /* step forward in micro-steps just to enter the sensor again */
  if (STEPPER_MoveHandOnSensor(motors, nofMotors, true, 1, 10000, 2, delay)!=ERR_OK) {
    res = ERR_FAILED;
  }

  /* here all hands are on the sensor: adjust with offset */
  STEPPER_MoveByOffset(motors, offsets, nofMotors, delay);
  /* set zero position */
  STEPPER_SetZeroPosition(motors, nofMotors);
  return res;
}

uint8_t STEPPER_ZeroAllHands(void) {
  uint8_t res = ERR_OK;
  STEPPER_Motor_t *motors[STEPPER_NOF_CLOCKS*STEPPER_NOF_CLOCK_MOTORS];
  int16_t offsets[STEPPER_NOF_CLOCKS*STEPPER_NOF_CLOCK_MOTORS];

  /* fill in motor array information */
  for(int c=0; c<STEPPER_NOF_CLOCKS; c++) {
    for (int m=0; m<STEPPER_NOF_CLOCK_MOTORS; m++) {
      motors[c*STEPPER_NOF_CLOCK_MOTORS + m] = &STEPPER_Clocks[c].mot[m];
      offsets[c*STEPPER_NOF_CLOCK_MOTORS + m] = NVMC_GetStepperZeroOffset(c, m);
    }
  }
  /* zero all of them */
  if (STEPPER_ZeroHand(motors, offsets, STEPPER_NOF_CLOCKS*STEPPER_NOF_CLOCK_MOTORS, STEPPER_HAND_ZERO_DELAY)!=ERR_OK) {
    res = ERR_FAILED;
  }
  return res;
}

uint8_t STEPPER_SetOffsetFrom12(void) {
  /* all hands shall be at 12-o-clock position */
  uint8_t res = ERR_OK;
  STEPPER_Motor_t *motors[STEPPER_NOF_CLOCKS*STEPPER_NOF_CLOCK_MOTORS];
  int16_t offsets[STEPPER_NOF_CLOCKS*STEPPER_NOF_CLOCK_MOTORS];

  /* first zero position at current position and set delay */
  for(int c=0; c<STEPPER_NOF_CLOCKS; c++) {
    for (int m=0; m<STEPPER_NOF_CLOCK_MOTORS; m++) {
      X12_017_SetPos(STEPPER_Clocks[c].mot[m].device, STEPPER_Clocks[c].mot[m].mot, 0);
      STEPPER_Clocks[c].mot[m].delay = 8;
    }
  }

  /* fill in motor array information */
  for(int c=0; c<STEPPER_NOF_CLOCKS; c++) {
    for (int m=0; m<STEPPER_NOF_CLOCK_MOTORS; m++) {
      motors[c*STEPPER_NOF_CLOCK_MOTORS + m] = &STEPPER_Clocks[c].mot[m];
    }
  }
  /* move forward in larger steps to find sensor */
  if (STEPPER_MoveHandOnSensor(motors, sizeof(motors)/sizeof(motors[0]), true, -10, 10000, 5, STEPPER_HAND_ZERO_DELAY)!=ERR_OK) {
    res = ERR_FAILED;
  }

  /* step back in micro-steps just to leave the sensor */
  if (STEPPER_MoveHandOnSensor(motors, sizeof(motors)/sizeof(motors[0]), false, 1, 10000, 2, STEPPER_HAND_ZERO_DELAY)!=ERR_OK) {
    res = ERR_FAILED;
  }

  /* step forward in micro-steps just to enter the sensor again */
  if (STEPPER_MoveHandOnSensor(motors, sizeof(motors)/sizeof(motors[0]), true, -1, 10000, 2, STEPPER_HAND_ZERO_DELAY)!=ERR_OK) {
    res = ERR_FAILED;
    return res;
  }

  /* store new offsets */
  NVMC_Data_t data;

  data = *NVMC_GetDataPtr(); /* struct copy */
  for(int c=0; c<STEPPER_NOF_CLOCKS; c++) {
    for (int m=0; m<STEPPER_NOF_CLOCK_MOTORS; m++) {
      data.zeroOffsets[c][m] = -X12_017_GetPos(STEPPER_Clocks[c].mot[m].device, STEPPER_Clocks[c].mot[m].mot);
    }
  }
  res = NVMC_WriteConfig(&data);
  if (res!=ERR_OK) {
    return res;
  }
  /* fill in motor array information */
  for(int c=0; c<STEPPER_NOF_CLOCKS; c++) {
    for (int m=0; m<STEPPER_NOF_CLOCK_MOTORS; m++) {
      offsets[c*STEPPER_NOF_CLOCK_MOTORS + m] = NVMC_GetStepperZeroOffset(c, m);
    }
  }
  STEPPER_MoveByOffset(motors, offsets, sizeof(motors)/sizeof(motors[0]), STEPPER_HAND_ZERO_DELAY);
  return res;
}

uint8_t STEPPER_Test(int8_t clock) {
  /* Test the clock stepper motors. Pass -1 to run the test for all motors/clocks */
  for (int m=0; m<STEPPER_NOF_CLOCK_MOTORS; m++) {
    /* clockwise */
    for(int i=0; i<4; i++) {
      if (clock==-1) { /* all */
        for(int c=0; c<STEPPER_NOF_CLOCKS; c++) {
          STEPPER_MoveClockDegreeRel(c, m, 90, STEPPER_MOVE_MODE_CW, 4, true, true);
        }
      } else {
        STEPPER_MoveClockDegreeRel(clock, m, 90, STEPPER_MOVE_MODE_CW, 4, true, true);
      }
      STEPPER_MoveAndWait(1000);
    }
    /* counter-clockwise */
    for(int i=0; i<4; i++) {
      if (clock==-1) { /* all */
        for(int c=0; c<STEPPER_NOF_CLOCKS; c++) {
          STEPPER_MoveClockDegreeRel(c, m, -90, STEPPER_MOVE_MODE_CCW, 4, true, true);
        }
      } else {
        STEPPER_MoveClockDegreeRel(clock, m, -90, STEPPER_MOVE_MODE_CCW, 4, true, true);
      }
      STEPPER_MoveAndWait(1000);
    }
  }
  return ERR_OK;
}

#endif

QueueHandle_t STEPPER_GetQueue(STEPPER_Handle_t stepper) {
  STEPPER_Device_t *device = (STEPPER_Device_t*)stepper;

   return device->queue;
}

void STEPPER_StrCatStatus(STEPPER_Handle_t stepper, unsigned char *buf, size_t bufSize) {
  STEPPER_Device_t *device = (STEPPER_Device_t*)stepper;

  McuUtility_strcat(buf, bufSize, (unsigned char*)"pos:");
  McuUtility_strcatNum32sFormatted(buf, bufSize, device->pos, ' ', 5);
#if 0 && PL_CONFIG_USE_NVMC
  McuUtility_strcat(buf, bufSize, (unsigned char*)", offs:");
  McuUtility_strcatNum16sFormatted(buf, bufSize, NVMC_GetStepperZeroOffset(i, 0), ' ', 4);
#endif
  McuUtility_strcat(buf, bufSize, (unsigned char*)", delay:");
  McuUtility_strcatNum16sFormatted(buf, bufSize, device->delay, ' ', 2);
  McuUtility_strcat(buf, bufSize, (unsigned char*)", #qItem:");
  McuUtility_strcatNum16sFormatted(buf, bufSize, uxQueueMessagesWaiting(device->queue), ' ', 2);
}

static uint8_t PrintStatus(const McuShell_StdIOType *io) {
  unsigned char buf[128];

  McuShell_SendStatusStr((unsigned char*)"stepper", (unsigned char*)"Stepper clock settings\r\n", io->stdOut);

  McuUtility_strcpy(buf, sizeof(buf), (unsigned char*)"360 degree steps: ");
  McuUtility_strcatNum32s(buf, sizeof(buf), STEPPER_CLOCK_360_STEPS);
  McuUtility_strcat(buf, sizeof(buf), (unsigned char*)"\r\n");
  McuShell_SendStatusStr((unsigned char*)"  steps", buf, io->stdOut);

#if PL_CONFIG_USE_X12_STEPPER
  McuX12_017_GetDeviceStatusString(STEPPER_Clocks[0].mot[X12_017_M0].device, buf, sizeof(buf));
  McuUtility_strcat(buf, sizeof(buf), (unsigned char*)"\r\n");
  McuUtility_strcpy(statStr, sizeof(statStr), (unsigned char*)"  X12.017[0]");
  McuShell_SendStatusStr(statStr, buf, io->stdOut);
#endif

#if PL_CONFIG_USE_X12_STEPPER
  McuX12_017_GetDeviceStatusString(STEPPER_Clocks[2].mot[X12_017_M0].device, buf, sizeof(buf));
  McuUtility_strcat(buf, sizeof(buf), (unsigned char*)"\r\n");
  McuUtility_strcpy(statStr, sizeof(statStr), (unsigned char*)"  X12.017[1]");
  McuShell_SendStatusStr(statStr, buf, io->stdOut);
#endif

  return ERR_OK;
}

static uint8_t PrintHelp(const McuShell_StdIOType *io) {
  McuShell_SendHelpStr((unsigned char*)"stepper", (unsigned char*)"Group of stepper commands\r\n", io->stdOut);
  McuShell_SendHelpStr((unsigned char*)"  help|status", (unsigned char*)"Print help or status information\r\n", io->stdOut);
  McuShell_SendHelpStr((unsigned char*)"  reset", (unsigned char*)"Performs a X12.017 driver reset\r\n", io->stdOut);
  McuShell_SendHelpStr((unsigned char*)"  test <c>", (unsigned char*)"Test stepper motors of clock (0-3), or -1 for all\r\n", io->stdOut);
#if PL_CONFIG_USE_MAG_SENSOR
  McuShell_SendHelpStr((unsigned char*)"  zero all", (unsigned char*)"Move all motors to zero position using magnet sensor\r\n", io->stdOut);
  McuShell_SendHelpStr((unsigned char*)"  zero <c> <m>", (unsigned char*)"Move clock (0-3) and motor (0-1) to zero position using magnet sensor\r\n", io->stdOut);
  McuShell_SendHelpStr((unsigned char*)"  offs <c> <m> <v>", (unsigned char*)"Set offset value for clock (0-3) and motor (0-1)\r\n", io->stdOut);
  McuShell_SendHelpStr((unsigned char*)"  offs 12", (unsigned char*)"Calculate offset from 12-o-clock\r\n", io->stdOut);
#endif
  McuShell_SendHelpStr((unsigned char*)"  idle", (unsigned char*)"Check if steppers are idle\r\n", io->stdOut);
  return ERR_OK;
}

uint8_t STEPPER_ParseCommand(const unsigned char *cmd, bool *handled, const McuShell_StdIOType *io) {
  int32_t steps, clk, m;
  const unsigned char *p;
  uint8_t res = ERR_OK;

  if (McuUtility_strcmp((char*)cmd, McuShell_CMD_HELP)==0 || McuUtility_strcmp((char*)cmd, "stepper help")==0) {
    *handled = TRUE;
    return PrintHelp(io);
  } else if ((McuUtility_strcmp((char*)cmd, McuShell_CMD_STATUS)==0) || (McuUtility_strcmp((char*)cmd, "stepper status")==0)) {
    *handled = TRUE;
    return PrintStatus(io);
  } else if (McuUtility_strcmp((char*)cmd, "stepper reset")==0) {
    *handled = TRUE;
#if PL_CONFIG_USE_X12_STEPPER
    McuX12_017_ResetDriver(STEPPER_Clocks[0].mot[X12_017_M0].device);
#endif
    return ERR_OK;
#if PL_CONFIG_USE_MAG_SENSOR
  } else if (McuUtility_strcmp((char*)cmd, "stepper zero all")==0) {
    *handled = TRUE;
    return STEPPER_ZeroAllHands();
  } else if (McuUtility_strncmp((char*)cmd, "stepper zero ", sizeof("stepper zero ")-1)==0) {
    *handled = TRUE;
    p = cmd + sizeof("stepper zero ")-1;
    if (   McuUtility_xatoi(&p, &clk)==ERR_OK && clk>=0 && clk<STEPPER_NOF_CLOCKS
        && McuUtility_xatoi(&p, &m)==ERR_OK && m>=0 && m<STEPPER_NOF_CLOCK_MOTORS
       )
    {
      STEPPER_Motor_t *motors[1];
      int16_t offset;

      motors[0] = &STEPPER_Clocks[clk].mot[m];
      offset = NVMC_GetStepperZeroOffset(clk, m);
      res = STEPPER_ZeroHand(motors, &offset, 1, STEPPER_HAND_ZERO_DELAY);
      if (res!=ERR_OK) {
        McuShell_SendStr((unsigned char*)"failed to find magnet/zero position\r\n", io->stdErr);
      }
    } else {
      return ERR_FAILED;
    }
  } else if (McuUtility_strcmp((char*)cmd, "stepper offs 12")==0) {
    *handled = TRUE;
    return STEPPER_SetOffsetFrom12();
  } else if (McuUtility_strncmp((char*)cmd, "stepper test ", sizeof("stepper test ")-1)==0) {
    *handled = TRUE;
    p = cmd + sizeof("stepper test ")-1;
    if (McuUtility_xatoi(&p, &clk)==ERR_OK && ((clk>=0 && clk<STEPPER_NOF_CLOCKS) || clk==-1)) {
      return STEPPER_Test(clk);
    }
    return ERR_FAILED;
  } else if (McuUtility_strncmp((char*)cmd, "stepper offs ", sizeof("stepper offs ")-1)==0) {
    int32_t val;

    *handled = TRUE;
    p = cmd + sizeof("stepper offs ")-1;
    if (   McuUtility_xatoi(&p, &clk)==ERR_OK && clk>=0 && clk<STEPPER_NOF_CLOCKS
        && McuUtility_xatoi(&p, &m)==ERR_OK && m>=0 && m<STEPPER_NOF_CLOCK_MOTORS
        && McuUtility_xatoi(&p, &val)==ERR_OK
       )
    {
  #if PL_CONFIG_USE_NVMC
      NVMC_Data_t data;

      if (NVMC_IsErased()) {
        McuShell_SendStr((unsigned char*)"FLASH is erased, initialize it first!\r\n", io->stdErr);
        return ERR_FAILED;
      }
      data = *NVMC_GetDataPtr(); /* struct copy */
      data.zeroOffsets[clk][m] = val;
      return NVMC_WriteConfig(&data);
  #endif
    } else {
      return ERR_FAILED;
    }
#endif
  } else if (McuUtility_strncmp((char*)cmd, "stepper step ", sizeof("stepper step ")-1)==0) {
    *handled = TRUE;
    p = cmd + sizeof("stepper step ")-1;
    if (McuUtility_xatoi(&p, &clk)==ERR_OK && clk>=0 && clk<STEPPER_NOF_CLOCKS) {
      if (McuUtility_xatoi(&p, &m)==ERR_OK && m>=0 && m<STEPPER_NOF_CLOCK_MOTORS) {
        if (McuUtility_xatoi(&p, &steps)==ERR_OK) {
#if PL_CONFIG_USE_X12_STEPPER
          McuX12_017_DoSteps(STEPPER_Clocks[clk].mot[m].device, STEPPER_Clocks[clk].mot[m].mot, steps);
#endif
          return ERR_OK;
        } else {
          return ERR_FAILED;
        }
      } else {
        return ERR_FAILED;
      }
    } else {
      return ERR_FAILED;
    }
#if 0
  } else if (McuUtility_strncmp((char*)cmd, "stepper goto ", sizeof("stepper goto ")-1)==0) {
    int32_t steps;

    *handled = TRUE;
    p = cmd + sizeof("stepper goto ")-1;
    if (McuUtility_xatoi(&p, &clk)==ERR_OK && clk>=0 && clk<STEPPER_NOF_CLOCKS) {
      if (McuUtility_xatoi(&p, &m)==ERR_OK && m>=0 && m<STEPPER_NOF_CLOCK_MOTORS) {
        if (McuUtility_xatoi(&p, &steps)==ERR_OK) {
          STEPPER_Clocks[clk].mot[m].doSteps = steps;
          STEPPER_Clocks[clk].mot[m].accelStepCntr = 0;
        }
      } else {
        return ERR_FAILED;
      }
    } else {
      return ERR_FAILED;
    }
    STEPPER_START_TIMER();
    return ERR_OK;
#endif
#if 0
  } else if (McuUtility_strcmp((char*)cmd, "stepper idle")==0) {
    *handled = TRUE;
    if (STEPPER_IsIdle()) {
      McuShell_SendStr((unsigned char*)"idle\r\n", io->stdOut);
    } else {
      McuShell_SendStr((unsigned char*)"busy\r\n", io->stdOut);
    }
    return ERR_OK;
#endif
  }
  return res;
}

#if 0
void STEPPER_SetAccelTable(STEPPER_Motor_t *motor, const uint16_t (*table)[2], size_t nofTableEntries) {
  motor->accelTable = table;
  motor->nofAccelTableEntries = nofTableEntries;
}
#endif


void STEPPER_NormalizePosition(STEPPER_Handle_t stepper) {
  STEPPER_Device_t *device = (STEPPER_Device_t*)stepper;

  device->pos %= STEPPER_CLOCK_360_STEPS;
}

void STEPPER_Init(void) {
#if PL_CONFIG_USE_X12_STEPPER
  McuX12_017_Config_t config;
  McuX12_017_Handle_t device;

  McuX12_017_GetDefaultConfig(&config);

  /* initialize first X12.017 */
  /* DRV_RESET: PIO0_14 */
  config.hasReset = true;
  config.hw_reset.gpio = GPIO;
  config.hw_reset.port = 0U;
  config.hw_reset.pin  = 14U;

  /* M0_DIR: PIO1_9 */
  config.motor[X12_017_M0].hw_dir.gpio = GPIO;
  config.motor[X12_017_M0].hw_dir.port = 1U;
  config.motor[X12_017_M0].hw_dir.pin  = 9U;

  /* M0_STEP: PIO0_12 */
  config.motor[X12_017_M0].hw_step.gpio = GPIO;
  config.motor[X12_017_M0].hw_step.port = 0U;
  config.motor[X12_017_M0].hw_step.pin  = 12U;

  /* M1_DIR: PIO0_13 */
  config.motor[X12_017_M1].hw_dir.gpio = GPIO;
  config.motor[X12_017_M1].hw_dir.port = 0U;
  config.motor[X12_017_M1].hw_dir.pin  = 13U;

  /* M1_STEP: PIO1_8 */
  config.motor[X12_017_M1].hw_step.gpio = GPIO;
  config.motor[X12_017_M1].hw_step.port = 1U;
  config.motor[X12_017_M1].hw_step.pin  = 8U;

  /* M2_DIR: PIO0_4 */
  config.motor[X12_017_M2].hw_dir.gpio = GPIO;
  config.motor[X12_017_M2].hw_dir.port = 0U;
  config.motor[X12_017_M2].hw_dir.pin  = 4U;

  /* M2_STEP: PIO0_28 */
  config.motor[X12_017_M2].isInverted = true;
  config.motor[X12_017_M2].hw_step.gpio = GPIO;
  config.motor[X12_017_M2].hw_step.port = 0U;
  config.motor[X12_017_M2].hw_step.pin  = 28U;

#if PL_CONFIG_BOARD_VERSION==1 /* PIO0_11 needs external pull-up! */
  /* M3_DIR: PIO0_11 */
  config.motor[X12_017_M3].isInverted = true;
  config.motor[X12_017_M3].hw_dir.gpio = GPIO;
  config.motor[X12_017_M3].hw_dir.port = 0U;
  config.motor[X12_017_M3].hw_dir.pin  = 11U;
#else
  /* M3_DIR: PIO0_27 */
  config.motor[X12_017_M3].isInverted = true;
  config.motor[X12_017_M3].hw_dir.gpio = GPIO;
  config.motor[X12_017_M3].hw_dir.port = 0U;
  config.motor[X12_017_M3].hw_dir.pin  = 27U;
#endif

#if PL_CONFIG_BOARD_VERSION==1
  /* M3_STEP: PIO1_0 */
  config.motor[X12_017_M3].hw_step.gpio = GPIO;
  config.motor[X12_017_M3].hw_step.port = 1U;
  config.motor[X12_017_M3].hw_step.pin  = 0U;
#else
  /* M3_STEP: PIO0_26 */
  config.motor[X12_017_M3].hw_step.gpio = GPIO;
  config.motor[X12_017_M3].hw_step.port = 0U;
  config.motor[X12_017_M3].hw_step.pin  = 26U;
#endif
  device = McuX12_017_InitDevice(&config);

  /* create clock descriptor */
  /* M1: inner shaft */
  STEPPER_Clocks[0].mot[0].device = device;
  STEPPER_Clocks[0].mot[0].mot = X12_017_M1;
  STEPPER_Clocks[0].mot[0].mag = MAG_MAG1;
  STEPPER_Clocks[0].mot[0].doSteps = 0;
  STEPPER_Clocks[0].mot[0].accelStepCntr = 0;
  /* M0: outer shaft */
  STEPPER_Clocks[0].mot[1].device = device;
  STEPPER_Clocks[0].mot[1].mot = X12_017_M0;
  STEPPER_Clocks[0].mot[1].mag = MAG_MAG0;
  STEPPER_Clocks[0].mot[1].doSteps = 0;
  STEPPER_Clocks[0].mot[1].accelStepCntr = 0;

  /* M3: inner shaft */
  STEPPER_Clocks[1].mot[0].device = device;
  STEPPER_Clocks[1].mot[0].mot = X12_017_M3;
  STEPPER_Clocks[1].mot[0].mag = MAG_MAG2;
  STEPPER_Clocks[1].mot[0].doSteps = 0;
  STEPPER_Clocks[1].mot[0].accelStepCntr = 0;

  /* M2: outer shaft */
  STEPPER_Clocks[1].mot[1].device = device;
  STEPPER_Clocks[1].mot[1].mot = X12_017_M2;
  STEPPER_Clocks[1].mot[1].mag = MAG_MAG3;
  STEPPER_Clocks[1].mot[1].doSteps = 0;
  STEPPER_Clocks[1].mot[1].accelStepCntr = 0;

  /* initialize second X12.017 */
  config.hasReset = false; /* second device shares the reset line from the first */
  /* M4_DIR: PIO0_0 */
  config.motor[X12_017_M0].hw_dir.gpio = GPIO;
  config.motor[X12_017_M0].hw_dir.port = 0U;
  config.motor[X12_017_M0].hw_dir.pin  = 0U;

  /* M4_STEP: PIO1_7 */
  config.motor[X12_017_M0].hw_step.gpio = GPIO;
  config.motor[X12_017_M0].hw_step.port = 1U;
  config.motor[X12_017_M0].hw_step.pin  = 7U;

  /* M5_DIR: PIO0_6 */
  config.motor[X12_017_M1].hw_dir.gpio = GPIO;
  config.motor[X12_017_M1].hw_dir.port = 0U;
  config.motor[X12_017_M1].hw_dir.pin  = 6U;

  /* M5_STEP: PIO0_7 */
  config.motor[X12_017_M1].hw_step.gpio = GPIO;
  config.motor[X12_017_M1].hw_step.port = 0U;
  config.motor[X12_017_M1].hw_step.pin  = 7U;

  /* M6_DIR: PIO0_8 */
  config.motor[X12_017_M2].isInverted = true;
  config.motor[X12_017_M2].hw_dir.gpio = GPIO;
  config.motor[X12_017_M2].hw_dir.port = 0U;
  config.motor[X12_017_M2].hw_dir.pin  = 8U;

  /* M6_STEP: PIO0_9 */
  config.motor[X12_017_M2].hw_step.gpio = GPIO;
  config.motor[X12_017_M2].hw_step.port = 0U;
  config.motor[X12_017_M2].hw_step.pin  = 9U;

  /* M7_DIR: PIO1_5 */
  config.motor[X12_017_M3].isInverted = true;
  config.motor[X12_017_M3].hw_dir.gpio = GPIO;
  config.motor[X12_017_M3].hw_dir.port = 1U;
  config.motor[X12_017_M3].hw_dir.pin  = 5U;

  /* M7_STEP: PIO1_6 */
  config.motor[X12_017_M3].hw_step.gpio = GPIO;
  config.motor[X12_017_M3].hw_step.port = 1U;
  config.motor[X12_017_M3].hw_step.pin  = 6U;

  device = McuX12_017_InitDevice(&config);

  /* create clock descriptor */
  /* M3: inner shaft */
  STEPPER_Clocks[2].mot[0].device = device;
  STEPPER_Clocks[2].mot[0].mot = X12_017_M3;
  STEPPER_Clocks[2].mot[0].mag = MAG_MAG4;
  STEPPER_Clocks[2].mot[0].doSteps = 0;
  STEPPER_Clocks[2].mot[0].accelStepCntr = 0;

  /* M2: outer shaft */
  STEPPER_Clocks[2].mot[1].device = device;
  STEPPER_Clocks[2].mot[1].mot = X12_017_M2;
  STEPPER_Clocks[2].mot[1].mag = MAG_MAG5;
  STEPPER_Clocks[2].mot[1].doSteps = 0;
  STEPPER_Clocks[2].mot[1].accelStepCntr = 0;

  /* M1: inner shaft */
  STEPPER_Clocks[3].mot[0].device = device;
  STEPPER_Clocks[3].mot[0].mot = X12_017_M1;
  STEPPER_Clocks[3].mot[0].mag = MAG_MAG7;
  STEPPER_Clocks[3].mot[0].doSteps = 0;
  STEPPER_Clocks[3].mot[0].accelStepCntr = 0;

  /* M0: outer shaft */
  STEPPER_Clocks[3].mot[1].device = device;
  STEPPER_Clocks[3].mot[1].mot = X12_017_M0;
  STEPPER_Clocks[3].mot[1].mag = MAG_MAG6;
  STEPPER_Clocks[3].mot[1].doSteps = 0;
  STEPPER_Clocks[3].mot[1].accelStepCntr = 0;
#elif PL_CONFIG_USE_STEPPER_EMUL
#endif /* #if PL_CONFIG_USE_X12_STEPPER */

#if PL_CONFIG_USE_X12_STEPPER
  McuX12_017_ResetDriver(STEPPER_Clocks[0].mot[0].device); /* shared reset line with second device */
#endif
  Timer_Init();
}

#endif /* PL_CONFIG_USE_STEPPER */
