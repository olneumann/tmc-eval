/*******************************************************************************
 * Copyright © 2024 Analog Devices Inc. All Rights Reserved. This software is
 * proprietary & confidential to Analog Devices, Inc. and its licensors.
 *******************************************************************************/


#include "tmc/StepDir.h"
#include "Board.h"
#include "tmc/ic/TMC2262/TMC2262.h"

#include "tmc/RAMDebug.h"
#include "hal/Timer.h"

#define VM_MIN         45   // VM[V/10] min
#define VM_MAX         650  // VM[V/10] max

#define TMC2262_MAX_VELOCITY  STEPDIR_MAX_VELOCITY

// Stepdir precision: 2^17 -> 17 digits of precision
#define STEPDIR_PRECISION (1 << 17)
#define DEFAULT_MOTOR  0

static bool vMaxModified = false;
static uint32_t vmax_position;
//static uint32_t vMax		   = 1;

static uint32_t right(uint8_t motor, int32_t velocity);
static uint32_t left(uint8_t motor, int32_t velocity);
static uint32_t rotate(uint8_t motor, int32_t velocity);
static uint32_t stop(uint8_t motor);
static uint32_t moveTo(uint8_t motor, int32_t position);
static uint32_t moveBy(uint8_t motor, int32_t *ticks);
static uint32_t GAP(uint8_t type, uint8_t motor, int32_t *value);
static uint32_t SAP(uint8_t type, uint8_t motor, int32_t value);
static void readRegister(uint8_t motor, uint16_t address, int32_t *value);
static void writeRegister(uint8_t motor, uint16_t address, int32_t value);
static uint32_t getMeasuredSpeed(uint8_t motor, int32_t *value);

static void periodicJob(uint32_t tick);
static void checkErrors(uint32_t tick);
static void deInit(void);
static uint32_t userFunction(uint8_t type, uint8_t motor, int32_t *value);

static uint8_t reset();
static void enableDriver(DriverState state);

static SPIChannelTypeDef *TMC2262_SPIChannel;
static TMC2262TypeDef TMC2262;


static inline TMC2262TypeDef *motorToIC(uint8_t motor)
{
	UNUSED(motor);
	return &TMC2262;
}

static inline SPIChannelTypeDef *channelToSPI(uint8_t channel)
{
	UNUSED(channel);

	return TMC2262_SPIChannel;
}

void tmc2262_readWriteArray(uint8_t channel, uint8_t *data, size_t length)
{
	channelToSPI(channel)->readWriteArray(data, length);
}

typedef struct
{
	IOPinTypeDef *N_DRN_EN;
	IOPinTypeDef *N_SLEEP;
	IOPinTypeDef *DIAG0;
	IOPinTypeDef *DIAG1;
	IOPinTypeDef *STEP_INT;
	IOPinTypeDef *DIR_INT;
} PinsTypeDef;

static PinsTypeDef Pins;

static uint32_t rotate(uint8_t motor, int32_t velocity)
{
	if (motor >= TMC2262_MOTORS)
		return TMC_ERROR_MOTOR;

	StepDir_rotate(motor, velocity);

	return TMC_ERROR_NONE;
}

static uint32_t right(uint8_t motor, int32_t velocity)
{
	return rotate(motor, velocity);

}

static uint32_t left(uint8_t motor, int32_t velocity)
{
	return rotate(motor, -velocity);

}

static uint32_t stop(uint8_t motor)
{
	return rotate(motor, 0);
}

static uint32_t moveTo(uint8_t motor, int32_t position)
{
	if (motor >= TMC2262_MOTORS)
		return TMC_ERROR_MOTOR;

	StepDir_moveTo(motor, position);

	return TMC_ERROR_NONE;
}

static uint32_t moveBy(uint8_t motor, int32_t *ticks)
{
	if (motor >= TMC2262_MOTORS)
		return TMC_ERROR_MOTOR;

	// determine actual position and add numbers of ticks to move
	*ticks += StepDir_getActualPosition(motor);

	return moveTo(motor, *ticks);
}

static uint32_t handleParameter(uint8_t readWrite, uint8_t motor, uint8_t type, int32_t *value)
{
	uint32_t buffer;
	uint32_t errors = TMC_ERROR_NONE;

	if(motor >= TMC2262_MOTORS)
		return TMC_ERROR_MOTOR;

	int32_t tempValue;

	switch(type)
	{
	case 0:
		// Target position
		if (readWrite == READ) {
			*value = StepDir_getTargetPosition(motor);
		} else if (readWrite == WRITE) {
			StepDir_moveTo(motor, *value);
		}
		break;
	case 1:
		// Actual position
		if (readWrite == READ) {
			*value = StepDir_getActualPosition(motor);
		} else if (readWrite == WRITE) {
			StepDir_setActualPosition(motor, *value);
		}
		break;
	case 2:
		// Target speed
		if (readWrite == READ) {
			*value = StepDir_getTargetVelocity(motor);
		} else if (readWrite == WRITE) {
			StepDir_rotate(motor, *value);
		}
		break;
	case 3:
		// Actual speed
		if (readWrite == READ) {
			switch (StepDir_getMode(motor)) {
			case STEPDIR_INTERNAL:
				*value = StepDir_getActualVelocity(motor);
				break;
			case STEPDIR_EXTERNAL:
			default:
				tempValue =
						(int32_t)(
								((int64_t) StepDir_getFrequency(motor)
										* (int64_t) 122)
										/ (int64_t)TMC2262_FIELD_READ(motorToIC(motor), TMC2262_TSTEP, TMC2262_TSTEP_MASK, TMC2262_TSTEP_SHIFT));
				*value = (abs(tempValue) < 20) ? 0 : tempValue;
				break;
			}
		} else if (readWrite == WRITE) {
			errors |= TMC_ERROR_TYPE;
		}
		break;
	case 4:
		// Maximum speed
		if (readWrite == READ) {
			*value = StepDir_getVelocityMax(motor);
		} else if (readWrite == WRITE) {
			StepDir_setVelocityMax(motor, abs(*value));
		}
		break;
	case 5:
		// Maximum acceleration
		if (readWrite == READ) {
			*value = StepDir_getAcceleration(motor);
		} else if (readWrite == WRITE) {
			StepDir_setAcceleration(motor, *value);
		}
		break;
	case 6:
		// Maximum current
		if (readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_IHOLD_IRUN,
					TMC2262_IRUN_MASK, TMC2262_IRUN_SHIFT);
		} else if (readWrite == WRITE) {
			TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_IHOLD_IRUN,
					TMC2262_IRUN_MASK, TMC2262_IRUN_SHIFT, *value);
		}
		break;
	case 7:
		// Standby current
		if (readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_IHOLD_IRUN,
					TMC2262_IHOLD_MASK, TMC2262_IHOLD_SHIFT);
		} else if (readWrite == WRITE) {
			TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_IHOLD_IRUN,
					TMC2262_IHOLD_MASK, TMC2262_IHOLD_SHIFT, *value);
		}
		break;
	case 8:
		// Position reached flag
		if (readWrite == READ) {
			*value = (StepDir_getStatus(motor) & STATUS_TARGET_REACHED) ? 1 : 0;
		} else if (readWrite == WRITE) {
			errors |= TMC_ERROR_TYPE;
		}
		break;
	case 14:
		// SW_MODE Register
		if(readWrite == READ) {
			*value = tmc2262_readInt(motorToIC(motor), TMC2262_SW_MODE);
		} else if(readWrite == WRITE) {
			tmc2262_writeInt(motorToIC(motor), TMC2262_SW_MODE, *value);
		}
		break;
	case 26:
		// Speed threshold for high speed mode
		if(readWrite == READ) {
			buffer = tmc2262_readInt(motorToIC(motor), TMC2262_THIGH);
			*value = MIN(0xFFFFF, (1 << 24) / ((buffer)? buffer : 1));
		} else if(readWrite == WRITE) {
			*value = MIN(0xFFFFF, (1 << 24) / ((*value)? *value:1));
			tmc2262_writeInt(motorToIC(motor), TMC2262_THIGH, *value);
		}
		break;
	case 27:
		// Minimum speed for switching to dcStep
		if(readWrite == READ) {
			*value = tmc2262_readInt(motorToIC(motor), TMC2262_TUDCSTEP);
		} else if(readWrite == WRITE) {
			tmc2262_writeInt(motorToIC(motor), TMC2262_TUDCSTEP, *value);
		}
		break;
	case 30:
		// Measured Speed
		if(readWrite == READ) {
			*value = TMC2262.velocity;
		} else if(readWrite == WRITE) {
			errors |= TMC_ERROR_TYPE;
		}
		break;
	case 31:
		// Current P
		if(readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_CURRENT_PI_REG, TMC2262_CUR_P_MASK, TMC2262_CUR_P_SHIFT);
		} else if(readWrite == WRITE) {
			TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_CURRENT_PI_REG, TMC2262_CUR_P_MASK, TMC2262_CUR_P_SHIFT, *value);
		}
		break;
	case 32:
		// Current I
		if(readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_CURRENT_PI_REG, TMC2262_CUR_I_MASK, TMC2262_CUR_I_SHIFT);
		} else if(readWrite == WRITE) {
			TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_CURRENT_PI_REG, TMC2262_CUR_I_MASK, TMC2262_CUR_I_SHIFT, *value);
		}
		break;
	case 37:
		// Current limit
		if(readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_CUR_ANGLE_LIMIT, TMC2262_CUR_PI_LIMIT_MASK, TMC2262_CUR_PI_LIMIT_SHIFT);
		} else if(readWrite == WRITE) {
			TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_CUR_ANGLE_LIMIT, TMC2262_CUR_PI_LIMIT_MASK, TMC2262_CUR_PI_LIMIT_SHIFT, *value);
		}
		break;
	case 40:
		// Measured current amplitude
		if(readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_CUR_ANGLE_MEAS, TMC2262_AMPL_MEAS_MASK, TMC2262_AMPL_MEAS_SHIFT);

		} else if(readWrite == WRITE) {
			TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_CUR_ANGLE_MEAS, TMC2262_AMPL_MEAS_MASK, TMC2262_AMPL_MEAS_SHIFT, *value);

		}
		break;
	case 140:
		// Microstep Resolution
		if(readWrite == READ) {
			*value = 0x100 >> TMC2262_FIELD_READ(motorToIC(motor), TMC2262_CHOPCONF, TMC2262_MRES_MASK, TMC2262_MRES_SHIFT);
		} else if(readWrite == WRITE) {
			switch(*value)
			{
			case 1:    *value = 8;   break;
			case 2:    *value = 7;   break;
			case 4:    *value = 6;   break;
			case 8:    *value = 5;   break;
			case 16:   *value = 4;   break;
			case 32:   *value = 3;   break;
			case 64:   *value = 2;   break;
			case 128:  *value = 1;   break;
			case 256:  *value = 0;   break;
			default:   *value = -1;  break;
			}

			if(*value != -1)
			{
				TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_CHOPCONF, TMC2262_MRES_MASK, TMC2262_MRES_SHIFT, *value);
			}
			else
			{
				errors |= TMC_ERROR_VALUE;
			}
		}
		break;
	case 162:
		// Chopper blank time
		if(readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_CHOPCONF, TMC2262_TBL_MASK, TMC2262_TBL_SHIFT);
		} else if(readWrite == WRITE) {
			TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_CHOPCONF, TMC2262_TBL_MASK, TMC2262_TBL_SHIFT, *value);
		}
		break;
	case 163:
		// Constant TOff Mode
		if(readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_CHOPCONF, TMC2262_CHM_MASK, TMC2262_CHM_SHIFT);
		} else if(readWrite == WRITE) {
			TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_CHOPCONF, TMC2262_CHM_MASK, TMC2262_CHM_SHIFT, *value);
		}
		break;
	case 164:
		// Disable fast decay comparator
		if(readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_CHOPCONF, TMC2262_DISFDCC_MASK, TMC2262_DISFDCC_SHIFT);
		} else if(readWrite == WRITE) {
			TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_CHOPCONF, TMC2262_DISFDCC_MASK, TMC2262_DISFDCC_SHIFT, *value);
		}
		break;
	case 165:
		// Chopper hysteresis end / fast decay time
		buffer = tmc2262_readInt(motorToIC(motor), TMC2262_CHOPCONF);
		if(readWrite == READ) {
			if(buffer & (1 << TMC2262_CHM_SHIFT))
			{
				*value = (buffer >> TMC2262_HEND_OFFSET_SHIFT) & TMC2262_HEND_OFFSET_MASK;
			}
			else
			{
				*value = (tmc2262_readInt(motorToIC(motor), TMC2262_CHOPCONF) >> TMC2262_HSTRT_TFD210_SHIFT) & TMC2262_HSTRT_TFD210_MASK;
				if(buffer & TMC2262_FD3_SHIFT)
					*value |= 1<<3; // MSB wird zu value dazugefügt
			}
		} else if(readWrite == WRITE) {
			if(tmc2262_readInt(motorToIC(motor), TMC2262_CHOPCONF) & (1<<14))
			{
				TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_CHOPCONF, TMC2262_HEND_OFFSET_MASK, TMC2262_HEND_OFFSET_SHIFT, *value);
			}
			else
			{
				TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_CHOPCONF, TMC2262_FD3_MASK, TMC2262_FD3_SHIFT, (*value & (1<<3))); // MSB wird zu value dazugefügt
				TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_CHOPCONF, TMC2262_HSTRT_TFD210_MASK, TMC2262_HSTRT_TFD210_SHIFT, *value);
			}
		}
		break;
	case 166:
		// Chopper hysteresis start / sine wave offset
		buffer = tmc2262_readInt(motorToIC(motor), TMC2262_CHOPCONF);
		if(readWrite == READ) {
			if(buffer & (1 << TMC2262_CHM_SHIFT))
			{
				*value = (buffer >> TMC2262_HSTRT_TFD210_SHIFT) & TMC2262_HSTRT_TFD210_MASK;
			}
			else
			{
				*value = (buffer >> TMC2262_HEND_OFFSET_SHIFT) & TMC2262_HEND_OFFSET_MASK;
				if(buffer & (1 << TMC2262_FD3_SHIFT))
					*value |= 1<<3; // MSB wird zu value dazugefügt
			}
		} else if(readWrite == WRITE) {
			if(buffer & (1 << TMC2262_CHM_SHIFT))
			{
				TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_CHOPCONF, TMC2262_HSTRT_TFD210_MASK, TMC2262_HSTRT_TFD210_SHIFT, *value);
			}
			else
			{
				TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_CHOPCONF, TMC2262_HEND_OFFSET_MASK, TMC2262_HEND_OFFSET_SHIFT, *value);
			}
		}
		break;
	case 167:
		// Chopper off time
		if(readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_CHOPCONF, TMC2262_TOFF_MASK, TMC2262_TOFF_SHIFT);
		} else if(readWrite == WRITE) {
			TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_CHOPCONF, TMC2262_TOFF_MASK, TMC2262_TOFF_SHIFT, *value);
		}
		break;
	case 168:
		// smartEnergy current minimum (SEIMIN)
		if(readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_COOLCONF, TMC2262_SEIMIN_MASK, TMC2262_SEIMIN_SHIFT);
		} else if(readWrite == WRITE) {
			TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_COOLCONF, TMC2262_SEIMIN_MASK, TMC2262_SEIMIN_SHIFT, *value);
		}
		break;
	case 169:
		// smartEnergy current down step
		if(readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_COOLCONF, TMC2262_SEDN_MASK, TMC2262_SEDN_SHIFT);
		} else if(readWrite == WRITE) {
			TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_COOLCONF, TMC2262_SEDN_MASK, TMC2262_SEDN_SHIFT, *value);
		}
		break;
	case 170:
		// smartEnergy hysteresis
		if(readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_COOLCONF, TMC2262_SEMAX_MASK, TMC2262_SEMAX_SHIFT);
		} else if(readWrite == WRITE) {
			TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_COOLCONF, TMC2262_SEMAX_MASK, TMC2262_SEMAX_SHIFT, *value);
		}
		break;
	case 171:
		// smartEnergy current up step
		if(readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_COOLCONF, TMC2262_SEUP_MASK, TMC2262_SEUP_SHIFT);
		} else if(readWrite == WRITE) {
			TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_COOLCONF, TMC2262_SEUP_MASK, TMC2262_SEUP_SHIFT, *value);
		}
		break;
	case 172:
		// smartEnergy hysteresis start
		if(readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_COOLCONF, TMC2262_SEMIN_MASK, TMC2262_SEMIN_SHIFT);
		} else if(readWrite == WRITE) {
			TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_COOLCONF, TMC2262_SEMIN_MASK, TMC2262_SEMIN_SHIFT, *value);
		}
		break;
	case 173:
		// stallGuard4 filter enable
		if(readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_SGP_CONF, TMC2262_SGP_FILT_EN_MASK, TMC2262_SGP_FILT_EN_SHIFT);
		} else if(readWrite == WRITE) {
			TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_SGP_CONF, TMC2262_SGP_FILT_EN_MASK, TMC2262_SGP_FILT_EN_SHIFT, *value);
		}
		break;
	case 174:
		// stallGuard4 threshold
		if(readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_SGP_CONF, TMC2262_SGP_THRS_MASK, TMC2262_SGP_THRS_SHIFT);
			*value = CAST_Sn_TO_S32(*value, 7);
		} else if(readWrite == WRITE) {
			TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_SGP_CONF, TMC2262_SGP_THRS_MASK, TMC2262_SGP_THRS_SHIFT, *value);
		}
		break;
	case 175:
		// stallGuard2 filter enable
		if(readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_COOLCONF, TMC2262_SFILT_MASK, TMC2262_SFILT_SHIFT);
		} else if(readWrite == WRITE) {
			TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_COOLCONF, TMC2262_SFILT_MASK, TMC2262_SFILT_SHIFT, *value);
		}
		break;
	case 176:
		// stallGuard2 threshold
		if(readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_COOLCONF, TMC2262_SGT_MASK, TMC2262_SGT_SHIFT);
			*value = CAST_Sn_TO_S32(*value, 7);
		} else if(readWrite == WRITE) {
			TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_COOLCONF, TMC2262_SGT_MASK, TMC2262_SGT_SHIFT, *value);
		}
		break;
	case 180:
		// smartEnergy actual current
		if(readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_DRV_STATUS, TMC2262_CS_ACTUAL_MASK, TMC2262_CS_ACTUAL_SHIFT);
		} else if(readWrite == WRITE) {
			errors |= TMC_ERROR_TYPE;
		}
		break;
	case 181:
		// smartEnergy stall velocity
		if(readWrite == READ) {
			*value = StepDir_getStallGuardThreshold(motor);
		} else if(readWrite == WRITE) {
			// Store the threshold value in the internal StepDir generator
			StepDir_setStallGuardThreshold(motor, *value);

			// Convert the value for the TCOOLTHRS register
			// The IC only sends out Stallguard errors while TCOOLTHRS >= TSTEP >= TPWMTHRS
			// The TSTEP value is measured. To prevent measurement inaccuracies hiding
			// a stall signal, we decrease the needed velocity by roughly 12% before converting it.
			*value -= (*value) >> 3;
			if (*value)
			{
				*value = MIN(0x000FFFFF, (1<<24) / (*value));
			}
			else
			{
				*value = 0x000FFFFF;
			}
			tmc2262_writeInt(motorToIC(motor), TMC2262_TCOOLTHRS, *value);
		}
		break;
	case 182:
		// smartEnergy threshold speed
		if(readWrite == READ) {
			buffer = tmc2262_readInt(motorToIC(motor), TMC2262_TCOOLTHRS);
			*value = MIN(0xFFFFF, (1<<24) / ((buffer)? buffer:1));
		} else if(readWrite == WRITE) {
			*value = MIN(0xFFFFF, (1<<24) / ((*value)? *value:1));
			tmc2262_writeInt(motorToIC(motor), TMC2262_TCOOLTHRS, *value);
		}
		break;
	case 185:
		// Chopper synchronization
		if(readWrite == READ) {
			*value = (tmc2262_readInt(motorToIC(motor), TMC2262_CHOPCONF) >> 20) & 0x0F;
		} else if(readWrite == WRITE) {
			buffer = tmc2262_readInt(motorToIC(motor), TMC2262_CHOPCONF);
			buffer &= ~(0x0F<<20);
			buffer |= (*value & 0x0F) << 20;
			tmc2262_writeInt(motorToIC(motor), TMC2262_CHOPCONF, buffer);
		}
		break;
	case 186:
		// PWM threshold speed
		if(readWrite == READ) {
			buffer = tmc2262_readInt(motorToIC(motor), TMC2262_TPWMTHRS);
			*value = MIN(0xFFFFF, (1<<24) / ((buffer)? buffer:1));
		} else if(readWrite == WRITE) {
			*value = MIN(0xFFFFF, (1<<24) / ((*value)? *value:1));
			tmc2262_writeInt(motorToIC(motor), TMC2262_TPWMTHRS, *value);
		}
		break;
	case 191:
		// PWM frequency
		if(readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_PWMCONF, TMC2262_PWM_FREQ_MASK, TMC2262_PWM_FREQ_SHIFT);
		} else if(readWrite == WRITE) {
			TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_PWMCONF, TMC2262_PWM_FREQ_MASK, TMC2262_PWM_FREQ_SHIFT, *value);
		}
		break;
	case 194:
		// MSCNT
		if(readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_MSCNT, TMC2262_MSCNT_MASK, TMC2262_MSCNT_SHIFT);
		} else if(readWrite == WRITE) {
			errors |= TMC_ERROR_TYPE;
		}
		break;
		/*	case 195:
		// MEAS_SD_EN
		if(readWrite == READ) {
		 *value = TMC5262_FIELD_READ(motorToIC(motor), TMC5262_PWMCONF, TMC5262_PWMCONF_SD_ON_MEAS_MASK, TMC5262_PWMCONF_SD_ON_MEAS_SHIFT);
		} else if(readWrite == WRITE) {
			TMC5262_FIELD_WRITE(motorToIC(motor), TMC5262_PWMCONF, TMC5262_PWMCONF_SD_ON_MEAS_MASK, TMC5262_PWMCONF_SD_ON_MEAS_SHIFT, *value);
		}
		break;*/
	case 204:
		// Freewheeling mode
		if(readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_PWMCONF, TMC2262_FREEWHEEL_MASK, TMC2262_FREEWHEEL_SHIFT);
		} else if(readWrite == WRITE) {
			TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_PWMCONF, TMC2262_FREEWHEEL_MASK, TMC2262_FREEWHEEL_SHIFT, *value);
		}
		break;
	case 206:
		// Load value
		if(readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_DRV_STATUS, TMC2262_SG_RESULT_MASK, TMC2262_SG_RESULT_SHIFT);
		} else if(readWrite == WRITE) {
			errors |= TMC_ERROR_TYPE;
		}
		break;
	case 209:
		// Encoder position
		if(readWrite == READ) {
			*value = tmc2262_readInt(motorToIC(motor), TMC2262_X_ENC);
		} else if(readWrite == WRITE) {
			tmc2262_writeInt(motorToIC(motor), TMC2262_X_ENC, *value);
		}
		break;
	case 210:
		// Encoder Resolution
		if(readWrite == READ) {
			*value = tmc2262_readInt(motorToIC(motor), TMC2262_ENC_CONST);
		} else if(readWrite == WRITE) {
			tmc2262_writeInt(motorToIC(motor), TMC2262_ENC_CONST, *value);
		}
		break;
	case 212:
		// Current range from DRV_CONF reg
		if(readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_DRV_CONF, TMC2262_CURRENT_RANGE_MASK, TMC2262_CURRENT_RANGE_SHIFT);
		} else if(readWrite == WRITE) {
			TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_DRV_CONF, TMC2262_CURRENT_RANGE_MASK, TMC2262_CURRENT_RANGE_SHIFT, *value);
		}
		break;
	case 213:
		// ADCTemperatur
		if(readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_ADC_VSUPPLY_TEMP, TMC2262_ADC_TEMP_MASK, TMC2262_ADC_TEMP_SHIFT);
		} else if(readWrite == WRITE) {
			errors |= TMC_ERROR_TYPE;
		}
		break;
	case 215:
		// ADCSupply
		if(readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_ADC_VSUPPLY_TEMP, TMC2262_ADC_VSUPPLY_MASK, TMC2262_ADC_VSUPPLY_SHIFT);
		} else if(readWrite == WRITE) {
			errors |= TMC_ERROR_TYPE;
		}
		break;
	case 216:
		// Overvoltage Limit ADC value
		if(readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_OTW_OV_VTH, TMC2262_OVERVOLTAGE_VTH_MASK, TMC2262_OVERVOLTAGE_VTH_SHIFT);
		} else if(readWrite == WRITE) {
			TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_OTW_OV_VTH, TMC2262_OVERVOLTAGE_VTH_MASK, TMC2262_OVERVOLTAGE_VTH_SHIFT, *value);
		}
		break;
	case 217:
		// Overtemperature Warning Limit
		if(readWrite == READ) {
			*value = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_OTW_OV_VTH, TMC2262_OVERTEMPPREWARNING_VTH_MASK, TMC2262_OVERTEMPPREWARNING_VTH_SHIFT);
		} else if(readWrite == WRITE) {
			TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_OTW_OV_VTH, TMC2262_OVERTEMPPREWARNING_VTH_MASK, TMC2262_OVERTEMPPREWARNING_VTH_SHIFT, *value);
		}
		break;
	case 218:
		// ADCTemperatur Converted
		if(readWrite == READ) {

			int32_t adc = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_ADC_VSUPPLY_TEMP, TMC2262_ADC_TEMP_MASK, TMC2262_ADC_TEMP_SHIFT);
			*value = (int32_t)10*(adc-2038)/77;
		} else if(readWrite == WRITE) {
			errors |= TMC_ERROR_TYPE;
		}
		break;
	case 220:
		// ADCSupply
		if(readWrite == READ) {
			int32_t adc = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_ADC_VSUPPLY_TEMP, TMC2262_ADC_VSUPPLY_MASK, TMC2262_ADC_VSUPPLY_SHIFT);
			*value = (int32_t)32*3052*adc/10000;
		} else if(readWrite == WRITE) {
			errors |= TMC_ERROR_TYPE;
		}
		break;
	case 221:
		// Overvoltage Limit converted
		if(readWrite == READ) {
			int32_t val = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_OTW_OV_VTH, TMC2262_OVERVOLTAGE_VTH_MASK, TMC2262_OVERVOLTAGE_VTH_SHIFT);
			*value = (int32_t)32*3052*val/10000;
		} else if(readWrite == WRITE) {
			int32_t val = (int32_t)(*value*10000/(3052*32));
			TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_OTW_OV_VTH, TMC2262_OVERVOLTAGE_VTH_MASK, TMC2262_OVERVOLTAGE_VTH_SHIFT, val);
		}
		break;
	case 222:
		// Overtemperature Warning Limit
		if(readWrite == READ) {
			int32_t temp = TMC2262_FIELD_READ(motorToIC(motor), TMC2262_OTW_OV_VTH, TMC2262_OVERTEMPPREWARNING_VTH_MASK, TMC2262_OVERTEMPPREWARNING_VTH_SHIFT);
			*value = (int32_t)(temp-2038)/7.7;
		} else if(readWrite == WRITE) {
			float valf  = *value*7.7;
			int32_t val = (int32_t)valf;
			val = val+2038;
			TMC2262_FIELD_WRITE(motorToIC(motor), TMC2262_OTW_OV_VTH, TMC2262_OVERTEMPPREWARNING_VTH_MASK, TMC2262_OVERTEMPPREWARNING_VTH_SHIFT, val);
		}
		break;
	default:
		errors |= TMC_ERROR_TYPE;
		break;
	}
	return errors;
}

static uint32_t SAP(uint8_t type, uint8_t motor, int32_t value)
{
	return handleParameter(WRITE, motor, type, &value);
}

static uint32_t GAP(uint8_t type, uint8_t motor, int32_t *value)
{
	return handleParameter(READ, motor, type, value);
}

static uint32_t getMeasuredSpeed(uint8_t motor, int32_t *value)
{
	if(motor >= TMC2262_MOTORS)
		return TMC_ERROR_MOTOR;

	switch (motor) {
	case 0:
		*value = StepDir_getActualVelocity(0);
		break;
	default:
		return TMC_ERROR_MOTOR;
		break;
	}
	return TMC_ERROR_NONE;
}

static void writeRegister(uint8_t motor, uint16_t address, int32_t value)
{
	tmc2262_writeInt(motorToIC(motor), (uint8_t) address, value);
}

static void readRegister(uint8_t motor, uint16_t address, int32_t *value)
{
	*value = tmc2262_readInt(motorToIC(motor), (uint8_t) address);
}

static void periodicJob(uint32_t tick)
{
	for(uint8_t motor = 0; motor < TMC2262_MOTORS; motor++)
	{
		tmc2262_periodicJob(&TMC2262, tick);
		StepDir_periodicJob(motor);
	}
}

static void checkErrors(uint32_t tick)
{
	UNUSED(tick);
	Evalboards.ch2.errors = 0;
}

static uint32_t GIO(uint8_t type, uint8_t motor, int32_t *value)
{
	UNUSED(motor);

	switch(type) {
	case 0: // Reading analog values at DIAG0
		*value = *HAL.ADCs->AIN0;
		break;
	case 1: // Reading analog values at DIAG1
		*value = *HAL.ADCs->AIN1;
		break;
	default:
		return TMC_ERROR_TYPE;
	}

	return TMC_ERROR_NONE;
}

static void deInit(void)
{
	HAL.IOs->config->setHigh(Pins.N_DRN_EN);
	HAL.IOs->config->reset(Pins.N_SLEEP);
	StepDir_deInit();
}

static uint8_t reset()
{
	if (StepDir_getActualVelocity(0) && !VitalSignsMonitor.brownOut)
		return 0;

	tmc2262_reset(&TMC2262);
	StepDir_init(STEPDIR_PRECISION);
	StepDir_setPins(0, Pins.STEP_INT, Pins.DIR_INT, Pins.DIAG1);
	StepDir_setVelocityMax(0, 100000);
	StepDir_setAcceleration(0, 25000);
	enableDriver(DRIVER_ENABLE);
	return 1;
}

static uint8_t restore()
{
	tmc2262_restore(&TMC2262);

	StepDir_init(STEPDIR_PRECISION);
	StepDir_setPins(0, Pins.STEP_INT, Pins.DIR_INT, Pins.DIAG1);
	StepDir_setVelocityMax(0, 100000);
	StepDir_setAcceleration(0, 25000);
	return 1;
}

static void enableDriver(DriverState state)
{
	if(state == DRIVER_USE_GLOBAL_ENABLE)
		state = Evalboards.driverEnable;

	if(state ==  DRIVER_DISABLE)
		HAL.IOs->config->setHigh(Pins.N_DRN_EN);
	else if((state == DRIVER_ENABLE) && (Evalboards.driverEnable == DRIVER_ENABLE))
		HAL.IOs->config->setLow(Pins.N_DRN_EN);
}

static void timer_overflow(timer_channel channel)
{
	UNUSED(channel);

	// RAMDebug
	debug_nextProcess();
}

void TMC2262_init(void)
{
	Pins.N_DRN_EN = &HAL.IOs->pins->DIO0;
	Pins.N_SLEEP = &HAL.IOs->pins->DIO8;
	Pins.STEP_INT = &HAL.IOs->pins->DIO6;
	Pins.DIR_INT = &HAL.IOs->pins->DIO7;

	HAL.IOs->config->toOutput(Pins.N_DRN_EN);
	HAL.IOs->config->toOutput(Pins.N_SLEEP);
	HAL.IOs->config->toOutput(Pins.STEP_INT);
	HAL.IOs->config->toOutput(Pins.DIR_INT);

	HAL.IOs->config->setHigh(Pins.N_SLEEP);
	HAL.IOs->config->setHigh(Pins.N_DRN_EN);


	TMC2262_SPIChannel = &HAL.SPI->ch2;
	TMC2262_SPIChannel->CSN = &HAL.IOs->pins->SPI2_CSN0;

	Evalboards.ch2.config->reset        = reset;
	Evalboards.ch2.config->restore      = restore;
	Evalboards.ch2.config->state        = CONFIG_RESET;

	tmc2262_init(&TMC2262, 0, Evalboards.ch2.config);

	// Initialize the software StepDir generator
	StepDir_init(STEPDIR_PRECISION);
	StepDir_setPins(0, Pins.STEP_INT, Pins.DIR_INT, Pins.DIAG1);
	StepDir_setVelocityMax(0, 100000);
	StepDir_setAcceleration(0, 25000);

	vmax_position = 0;

	Evalboards.ch2.rotate               = rotate;
	Evalboards.ch2.right                = right;
	Evalboards.ch2.left                 = left;
	Evalboards.ch2.stop                 = stop;
	Evalboards.ch2.GAP                  = GAP;
	Evalboards.ch2.SAP                  = SAP;
	Evalboards.ch2.moveTo               = moveTo;
	Evalboards.ch2.moveBy               = moveBy;
	Evalboards.ch2.writeRegister        = writeRegister;
	Evalboards.ch2.readRegister         = readRegister;
	Evalboards.ch2.periodicJob          = periodicJob;
	Evalboards.ch2.GIO                  = GIO;
	Evalboards.ch2.getMeasuredSpeed     = getMeasuredSpeed;
	Evalboards.ch2.enableDriver         = enableDriver;
	Evalboards.ch2.checkErrors          = checkErrors;
	Evalboards.ch2.numberOfMotors       = TMC2262_MOTORS;
	Evalboards.ch2.VMMin                = VM_MIN;
	Evalboards.ch2.VMMax                = VM_MAX;
	Evalboards.ch2.deInit               = deInit;

	Timer.overflow_callback = timer_overflow;
	Timer.init();

	enableDriver(DRIVER_USE_GLOBAL_ENABLE);
};
