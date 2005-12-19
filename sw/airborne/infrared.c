/*
 * Paparazzi mcu0 $Id$
 *  
 * Copyright (C) 2003  Pascal Brisset, Antoine Drouin
 *
 * This file is part of paparazzi.
 *
 * paparazzi is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * paparazzi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with paparazzi; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA. 
 *
 */
/** \file infrared.c
 *  \brief Regroup all functions link to \a ir
 */

#include <stdlib.h>

#include "adc_ap.h"
#include "infrared.h"
#include "gps.h"
#include "autopilot.h"
#include "estimator.h"
#include "downlink.h"

int16_t ir_roll;
int16_t ir_pitch;

/** Initialized to \a IR_DEFAULT_CONTRAST. Changed with calibration */
int16_t ir_contrast     = IR_DEFAULT_CONTRAST;
/** Initialized to \a IR_DEFAULT_CONTRAST.
 *  Changed with @@@@@ EST-CE QUE CA CHANGE @@@@@ */

/** \def RadOfIrFromConstrast(c)
 *  \brief Contrast measurement
 *  \note <b>Plane must be nose down !</b>
 */
#define RadOfIrFromConstrast(c) ir_rad_of_ir = IR_RAD_OF_IR_CONTRAST / c;

/** rad_of_ir variable factor: let convert \a ir value in radian.
 *  Initialized with airframe \a IR_RAD_OF_IR_CONTRAST and \a IR_DEFAULT_CONTRAST constants. \n
 *  Change when \a lls work.
 */
float ir_rad_of_ir = IR_RAD_OF_IR_CONTRAST / IR_DEFAULT_CONTRAST;

float estimator_rad_of_ir, estimator_ir, estimator_rad;

static struct adc_buf buf_ir1;
static struct adc_buf buf_ir2;

/** \fn void ir_init(void)
 *  \brief Initialisation of \a ir */
/** Initialize \a ir with the \a IR_DEFAULT_CONTRAST \n
 *  Initialize \a adc_buf_channel
 */
#ifndef ADC_CHANNEL_IR_NB_SAMPLES
#define ADC_CHANNEL_IR_NB_SAMPLES DEFAULT_AV_NB_SAMPLE
#else
#warning " ADC_CHANNEL_IR_NB_SAMPLES";
#endif
void ir_init(void) {
  RadOfIrFromConstrast(IR_DEFAULT_CONTRAST);
  adc_buf_channel(ADC_CHANNEL_IR1, &buf_ir1, ADC_CHANNEL_IR_NB_SAMPLES);
  adc_buf_channel(ADC_CHANNEL_IR2, &buf_ir2, ADC_CHANNEL_IR_NB_SAMPLES);
 
  estimator_rad_of_ir = ir_rad_of_ir;
}

/** \fn void ir_update(void)
 *  \brief Update \a ir */
/** Different if the \a SIMUL flag is true or not. \n
 */
void ir_update(void) {
#if defined SITL
  /** ir_roll set by simulator in sim_ir.c */
#else
#if defined SIMUL
  extern volatile int16_t simul_ir_roll, simul_ir_pitch;
  ir_roll = simul_ir_roll; 
  ir_pitch = simul_ir_pitch;
#else
  int16_t x1_mean = buf_ir1.sum/buf_ir1.av_nb_sample;
  int16_t x2_mean = buf_ir2.sum/buf_ir2.av_nb_sample;
  ir_roll = IR_RollOfIrs(x1_mean, x2_mean);
  ir_pitch = IR_PitchOfIrs(x1_mean, x2_mean);
#endif
  ir_roll -= IR_ADC_ROLL_NEUTRAL;
  ir_pitch -= IR_ADC_PITCH_NEUTRAL;
#endif
}

/** \brief Contrast measurement
 *  \note <b>Plane must be nose down !</b>
 */
static void ir_gain_calib(void) {
  /* plane nose down -> negativ value */
  ir_contrast = abs(ir_pitch);
  RadOfIrFromConstrast(ir_contrast);
}

/** Maximal delay waits before calibration.
		After, no more calibration is possible */
#define MAX_DELAY_FOR_CALIBRATION 10

uint8_t calib_status = NO_CALIB;

/** \brief Calibrate contrast if paparazzi mode is
 * set to auto1 before MAX_DELAY_FOR_CALIBRATION secondes */
/**User must put verticaly the uav (nose bottom) and push
 * radio roll stick to get new calibration
 * If not, the default calibration is used.
 */
void ground_calibrate( bool_t triggered ) {
  switch (calib_status) {
  case NO_CALIB:
    if (cputime < MAX_DELAY_FOR_CALIBRATION && pprz_mode == PPRZ_MODE_AUTO1 ) {
      calib_status = WAITING_CALIB_CONTRAST;
      DOWNLINK_SEND_CALIB_START();
    }
    break;
  case WAITING_CALIB_CONTRAST:
    if (triggered) {
      ir_gain_calib();
      estimator_rad_of_ir = ir_rad_of_ir;
      calib_status = CALIB_DONE;
      DOWNLINK_SEND_CALIB_CONTRAST(&ir_contrast);
    }
    break;
  case CALIB_DONE:
    break;
  }
}

#define INIT_WEIGHT 100. /* The number of times the initial value has to be taken */
#define RHO 0.995 /* The higher, the slower the estimation is changing */

#define G 9.81

void estimator_update_ir_estim( void ) {
  static float last_hspeed_dir;
  static uint32_t last_t; /* ms */
  static bool_t initialized = FALSE;
  static float sum_xy, sum_xx;

  if (initialized) {
    float dt = (float)(gps_itow - last_t) / 1e3;
    if (dt > 0.1) { // Against division by zero
      float dpsi = (estimator_hspeed_dir - last_hspeed_dir); 
      NormRadAngle(dpsi);
      estimator_rad = dpsi/dt*NOMINAL_AIRSPEED/G; /* tan linearized */
      NormRadAngle(estimator_rad);
      estimator_ir = (float)ir_roll;
      float absphi = fabs(estimator_rad);
      if (absphi < 1.0 && absphi > 0.05 && (- ir_contrast/2 < ir_roll && ir_roll < ir_contrast/2)) {
	sum_xy = estimator_rad * estimator_ir + RHO * sum_xy;
	sum_xx = estimator_ir * estimator_ir + RHO * sum_xx;
#if defined IR_RAD_OF_IR_MIN_VALUE & defined IR_RAD_OF_IR_MAX_VALUE
	float result = sum_xy / sum_xx;
	if (result < IR_RAD_OF_IR_MIN_VALUE)
	  estimator_rad_of_ir = IR_RAD_OF_IR_MIN_VALUE;
	else if (result > IR_RAD_OF_IR_MAX_VALUE)
	  estimator_rad_of_ir = IR_RAD_OF_IR_MAX_VALUE;
	else
	  estimator_rad_of_ir = result;
#else
	  estimator_rad_of_ir = sum_xy / sum_xx;
#endif
      }
    } 
  } else {
    initialized = TRUE;
    float init_ir2 = ir_contrast;
    init_ir2 = init_ir2*init_ir2;
    sum_xy = INIT_WEIGHT * estimator_rad_of_ir * init_ir2;
    sum_xx = INIT_WEIGHT * init_ir2;
  }

  last_hspeed_dir = estimator_hspeed_dir;
  last_t = gps_itow;
}

float ir_roll_neutral  = RadOfDeg(IR_ROLL_NEUTRAL_DEFAULT);
float ir_pitch_neutral = RadOfDeg(IR_PITCH_NEUTRAL_DEFAULT);

void estimator_update_state_infrared( void ) {
  float rad_of_ir = (ir_estim_mode == IR_ESTIM_MODE_ON ? 
		     estimator_rad_of_ir :
		     ir_rad_of_ir);
		     
  estimator_phi  = rad_of_ir * ir_roll - ir_roll_neutral;
  estimator_theta = rad_of_ir * ir_pitch - ir_pitch_neutral;
}
