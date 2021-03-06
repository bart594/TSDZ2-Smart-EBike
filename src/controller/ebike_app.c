/*
 * TongSheng TSDZ2 motor controller firmware
 *
 * Copyright (C) Casainho, Leon, MSpider65 2020.
 *
 * Released under the GPL License, Version 3
 */

#include "ebike_app.h"
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include "stm8s.h"
#include "stm8s_gpio.h"
#include "main.h"
#include "interrupts.h"
#include "adc.h"
#include "main.h"
#include "motor.h"
#include "pwm.h"
#include "uart.h"
#include "brake.h"
#include "lights.h"
#include "common.h"

// Initial configuration values
volatile struct_configuration_variables m_configuration_variables = {
        .ui16_battery_low_voltage_cut_off_x10 = 300, // 36 V battery, 30.0V (3.0 * 10)
        .ui16_wheel_perimeter = 2050,                // 26'' wheel: 2050 mm perimeter
        .ui8_wheel_speed_max = 45,                   // 45 Km/h
        .ui8_motor_type = 0,         				// 48V motor
        .ui8_pedal_torque_per_10_bit_ADC_step_x100 = 67,
        .ui8_target_battery_max_power_div25 = 20,    // 500W (500/25 = 20)
        .ui8_optional_ADC_function = 0,               // 0 = no function (throttle/temp)
		.ui8_hall_ref_angles = {PHASE_ROTOR_ANGLE_30, PHASE_ROTOR_ANGLE_90, PHASE_ROTOR_ANGLE_150, PHASE_ROTOR_ANGLE_210, PHASE_ROTOR_ANGLE_270, PHASE_ROTOR_ANGLE_330},
		.ui8_hall_counter_offsets = {HALL_COUNTER_OFFSET_UP, HALL_COUNTER_OFFSET_DOWN, HALL_COUNTER_OFFSET_UP, HALL_COUNTER_OFFSET_DOWN, HALL_COUNTER_OFFSET_UP, HALL_COUNTER_OFFSET_DOWN}			
     	};
		
// system
static uint8_t    ui8_riding_mode = OFF_MODE;
static uint8_t    ui8_riding_mode_parameter = 0;
static uint8_t    ui8_riding_mode_parameter_power = 0;
static uint8_t    ui8_riding_mode_parameter_power_soft_start = 0;
static uint8_t    ui8_riding_mode_parameter_torque = 0;
static uint8_t    ui8_riding_mode_parameter_torque_soft_start = 0;
static uint8_t    ui8_power_assist_multiplier_x10 = 0;
static uint8_t    ui8_torque_assist_factor = 0;
static uint8_t    ui8_system_state = NO_ERROR;
static uint8_t    ui8_motor_enabled = 1;
static uint8_t    ui8_lights_configuration = 10;
static uint8_t    ui8_lights_state = 0;
volatile uint8_t  ui8_assist_without_pedal_rotation_threshold = 0;
volatile uint8_t  ui8_soft_start_feature_enabled = 0;

// power control
static uint8_t    ui8_battery_current_max = DEFAULT_VALUE_BATTERY_CURRENT_MAX;
static uint16_t   ui8_duty_cycle_ramp_up_inverse_step = PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_DEFAULT;
static uint16_t   ui8_duty_cycle_ramp_up_inverse_step_default = PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_DEFAULT;
static uint16_t   ui8_duty_cycle_ramp_down_inverse_step = PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_DEFAULT;
static uint16_t   ui16_battery_voltage_filtered_x1000 = 0;
static uint8_t    ui8_battery_current_filtered_x10 = 0;
static uint8_t    ui8_adc_battery_current_max = ADC_10_BIT_BATTERY_CURRENT_MAX;
static uint8_t    ui8_adc_battery_current_target = 0;
static uint8_t    ui8_duty_cycle_target = 0;
static uint8_t	  ui8_field_weakening_current_adc = 0;
static uint8_t	  ui8_adc_battery_current_min = 0;


// cadence sensor
uint16_t ui16_cadence_ticks_count_min_speed_adj = CADENCE_SENSOR_CALC_COUNTER_MIN;
static uint8_t ui8_pedal_cadence_RPM = 0;


// torque sensor
uint16_t ui16_adc_pedal_torque_offset = 100;
volatile uint16_t ui16_adc_pedal_torque = 0;
static uint16_t   ui16_adc_pedal_torque_delta = 0;
static uint16_t   ui16_pedal_torque_x100 = 0;
volatile uint16_t  ui16_m_torque_sensor_weight_x10 = 0;

// wheel speed sensor
static uint16_t ui16_wheel_speed_x10 = 0;

// throttle control
volatile uint8_t ui8_adc_throttle = 0;

// motor temperature control
static uint16_t ui16_adc_motor_temperature_filtered = 0;
static uint16_t ui8_motor_temperature_filtered = 0;
static uint8_t ui8_motor_temperature_max_value_to_limit = 0;
static uint8_t ui8_motor_temperature_min_value_to_limit = 0;

// hybrid assist
static uint8_t ui8_hybrid_mode_enabled = 0;
//torque linearization
static uint8_t ui8_torque_linearization_enabled = 0;

#define TORQUE_SENSOR_LINEARIZE_NR_POINTS 6

uint16_t ui16_torque_sensor_linear_values[TORQUE_SENSOR_LINEARIZE_NR_POINTS * 2];

/*uint16_t ui16_torque_sensor_linear_values[TORQUE_SENSOR_LINEARIZE_NR_POINTS * 2] =
{
  // ADC 10 bits step, steps_per_kg_x100
  0, 20,
  210, 20,
  240, 33,
  260  50,
  290, 90,
  310, 170,
};
*/

// eMTB assist
#define eMTB_POWER_FUNCTION_ARRAY_SIZE      241

static const uint8_t ui8_eMTB_power_function_160[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 8, 8, 8, 8, 8, 9, 9, 9, 9, 9, 10, 10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 15, 15, 15, 15, 16, 16, 16, 16, 17, 17, 17, 17, 18, 18, 18, 18, 19, 19, 19, 20, 20, 20, 20, 21, 21, 21, 22, 22, 22, 22, 23, 23, 23, 24, 24, 24, 24, 25, 25, 25, 26, 26, 26, 27, 27, 27, 27, 28, 28, 28, 29, 29, 29, 30, 30, 30, 31, 31, 31, 32, 32, 32, 33, 33, 33, 34, 34, 34, 35, 35, 35, 36, 36, 36, 37, 37, 37, 38, 38, 38, 39, 39, 40, 40, 40, 41, 41, 41, 42, 42, 42, 43, 43, 44, 44, 44, 45, 45, 45, 46, 46, 47, 47, 47, 48, 48, 48, 49, 49, 50, 50, 50, 51, 51, 52, 52, 52, 53, 53, 54, 54, 54, 55, 55, 56, 56, 56, 57, 57, 58, 58, 58, 59, 59, 60, 60, 61, 61, 61, 62, 62, 63, 63, 63, 64, 64 };
static const uint8_t ui8_eMTB_power_function_165[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13, 14, 14, 14, 14, 15, 15, 15, 16, 16, 16, 16, 17, 17, 17, 18, 18, 18, 19, 19, 19, 20, 20, 20, 21, 21, 21, 22, 22, 22, 23, 23, 23, 24, 24, 24, 25, 25, 25, 26, 26, 27, 27, 27, 28, 28, 28, 29, 29, 30, 30, 30, 31, 31, 32, 32, 32, 33, 33, 34, 34, 34, 35, 35, 36, 36, 36, 37, 37, 38, 38, 39, 39, 39, 40, 40, 41, 41, 42, 42, 42, 43, 43, 44, 44, 45, 45, 46, 46, 47, 47, 47, 48, 48, 49, 49, 50, 50, 51, 51, 52, 52, 53, 53, 54, 54, 55, 55, 56, 56, 57, 57, 58, 58, 59, 59, 60, 60, 61, 61, 62, 62, 63, 63, 64, 64, 65, 65, 66, 66, 67, 67, 68, 68, 69, 69, 70, 71, 71, 72, 72, 73, 73, 74, 74, 75, 75, 76, 77, 77, 78, 78, 79, 79, 80, 81, 81, 82, 82, 83, 83, 84, 85 };
static const uint8_t ui8_eMTB_power_function_170[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 13, 13, 13, 14, 14, 14, 15, 15, 15, 16, 16, 16, 17, 17, 18, 18, 18, 19, 19, 19, 20, 20, 21, 21, 21, 22, 22, 23, 23, 23, 24, 24, 25, 25, 26, 26, 26, 27, 27, 28, 28, 29, 29, 30, 30, 30, 31, 31, 32, 32, 33, 33, 34, 34, 35, 35, 36, 36, 37, 37, 38, 38, 39, 39, 40, 40, 41, 41, 42, 42, 43, 43, 44, 45, 45, 46, 46, 47, 47, 48, 48, 49, 49, 50, 51, 51, 52, 52, 53, 53, 54, 55, 55, 56, 56, 57, 58, 58, 59, 59, 60, 61, 61, 62, 63, 63, 64, 64, 65, 66, 66, 67, 68, 68, 69, 70, 70, 71, 71, 72, 73, 73, 74, 75, 75, 76, 77, 77, 78, 79, 80, 80, 81, 82, 82, 83, 84, 84, 85, 86, 87, 87, 88, 89, 89, 90, 91, 92, 92, 93, 94, 94, 95, 96, 97, 97, 98, 99, 100, 100, 101, 102, 103, 103, 104, 105, 106, 107, 107, 108, 109, 110, 110, 111 };
static const uint8_t ui8_eMTB_power_function_175[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9, 10, 10, 10, 11, 11, 11, 12, 12, 13, 13, 13, 14, 14, 14, 15, 15, 16, 16, 17, 17, 17, 18, 18, 19, 19, 20, 20, 20, 21, 21, 22, 22, 23, 23, 24, 24, 25, 25, 26, 26, 27, 27, 28, 28, 29, 29, 30, 31, 31, 32, 32, 33, 33, 34, 34, 35, 36, 36, 37, 37, 38, 39, 39, 40, 40, 41, 42, 42, 43, 44, 44, 45, 45, 46, 47, 47, 48, 49, 49, 50, 51, 51, 52, 53, 53, 54, 55, 56, 56, 57, 58, 58, 59, 60, 61, 61, 62, 63, 64, 64, 65, 66, 67, 67, 68, 69, 70, 70, 71, 72, 73, 74, 74, 75, 76, 77, 78, 78, 79, 80, 81, 82, 83, 83, 84, 85, 86, 87, 88, 88, 89, 90, 91, 92, 93, 94, 95, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146 };
static const uint8_t ui8_eMTB_power_function_180[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 7, 7, 7, 8, 8, 8, 9, 9, 9, 10, 10, 11, 11, 11, 12, 12, 13, 13, 14, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 23, 23, 24, 24, 25, 25, 26, 27, 27, 28, 28, 29, 30, 30, 31, 32, 32, 33, 34, 34, 35, 36, 36, 37, 38, 38, 39, 40, 41, 41, 42, 43, 43, 44, 45, 46, 46, 47, 48, 49, 50, 50, 51, 52, 53, 54, 54, 55, 56, 57, 58, 59, 59, 60, 61, 62, 63, 64, 65, 66, 67, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 105, 106, 107, 108, 109, 110, 111, 112, 114, 115, 116, 117, 118, 119, 120, 122, 123, 124, 125, 126, 128, 129, 130, 131, 132, 134, 135, 136, 137, 139, 140, 141, 142, 144, 145, 146, 147, 149, 150, 151, 153, 154, 155, 157, 158, 159, 161, 162, 163, 165, 166, 167, 169, 170, 171, 173, 174, 176, 177, 178, 180, 181, 182, 184, 185, 187, 188, 190, 191, 192 };
static const uint8_t ui8_eMTB_power_function_195[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 2, 2, 2, 3, 3, 3, 3, 4, 4, 5, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 13, 13, 14, 15, 15, 16, 17, 17, 18, 19, 20, 21, 21, 22, 23, 24, 25, 26, 27, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 39, 40, 41, 42, 43, 44, 45, 47, 48, 49, 50, 51, 53, 54, 55, 57, 58, 59, 61, 62, 63, 65, 66, 68, 69, 70, 72, 73, 75, 76, 78, 79, 81, 83, 84, 86, 87, 89, 91, 92, 94, 96, 97, 99, 101, 103, 104, 106, 108, 110, 112, 113, 115, 117, 119, 121, 123, 125, 127, 129, 131, 132, 134, 136, 139, 141, 143, 145, 147, 149, 151, 153, 155, 157, 160, 162, 164, 166, 168, 171, 173, 175, 177, 180, 182, 184, 187, 189, 191, 194, 196, 199, 201, 203, 206, 208, 211, 213, 216, 218, 221, 224, 226, 229, 231, 234, 237, 239, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240 };
static const uint8_t ui8_eMTB_power_function_210[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 5, 5, 6, 7, 7, 8, 9, 9, 10, 11, 12, 13, 14, 14, 15, 16, 17, 19, 20, 21, 22, 23, 24, 26, 27, 28, 30, 31, 32, 34, 35, 37, 39, 40, 42, 43, 45, 47, 49, 50, 52, 54, 56, 58, 60, 62, 64, 66, 68, 71, 73, 75, 77, 80, 82, 84, 87, 89, 92, 94, 97, 99, 102, 104, 107, 110, 113, 115, 118, 121, 124, 127, 130, 133, 136, 139, 142, 145, 149, 152, 155, 158, 162, 165, 169, 172, 176, 179, 183, 186, 190, 194, 197, 201, 205, 209, 213, 216, 220, 224, 228, 232, 237, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240 };
static const uint8_t ui8_eMTB_power_function_225[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 6, 7, 8, 8, 9, 10, 12, 13, 14, 15, 17, 18, 20, 21, 23, 24, 26, 28, 30, 32, 34, 36, 38, 40, 43, 45, 47, 50, 52, 55, 58, 61, 64, 66, 70, 73, 76, 79, 82, 86, 89, 93, 96, 100, 104, 108, 112, 116, 120, 124, 128, 133, 137, 142, 146, 151, 156, 161, 166, 171, 176, 181, 186, 191, 197, 202, 208, 214, 219, 225, 231, 237, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240 };
static const uint8_t ui8_eMTB_power_function_240[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 3, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 15, 17, 19, 21, 23, 25, 27, 30, 32, 35, 38, 41, 44, 47, 51, 54, 58, 62, 66, 70, 74, 79, 83, 88, 93, 98, 103, 108, 114, 120, 125, 131, 137, 144, 150, 157, 164, 171, 178, 185, 193, 200, 208, 216, 224, 233, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240 };
static const uint8_t ui8_eMTB_power_function_255[eMTB_POWER_FUNCTION_ARRAY_SIZE] = { 0, 0, 0, 0, 0, 1, 1, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 18, 21, 24, 26, 30, 33, 37, 41, 45, 49, 54, 58, 64, 69, 75, 80, 87, 93, 100, 107, 114, 122, 130, 138, 146, 155, 164, 174, 184, 194, 204, 215, 226, 238, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240, 240 };

// cruise
static int16_t i16_cruise_pid_kp = 0;
static int16_t i16_cruise_pid_ki = 0;
static uint8_t ui8_cruise_PID_initialize = 1;
static uint16_t ui16_wheel_speed_target_received_x10 = 0;

// UART
#define UART_NUMBER_DATA_BYTES_TO_RECEIVE   10   // change this value depending on how many data bytes there are to receive ( Package = one start byte + data bytes + two bytes 16 bit CRC )
#define UART_NUMBER_DATA_BYTES_TO_SEND      21  // change this value depending on how many data bytes there are to send ( Package = one start byte + data bytes + two bytes 16 bit CRC )

volatile uint8_t ui8_received_package_flag = 0;
volatile uint8_t ui8_rx_buffer[UART_NUMBER_DATA_BYTES_TO_RECEIVE + 3];
volatile uint8_t ui8_rx_counter = 0;
volatile uint8_t ui8_tx_buffer[UART_NUMBER_DATA_BYTES_TO_SEND + 3];
volatile uint8_t ui8_tx_buffer_index;
volatile uint8_t ui8_i;
volatile uint8_t ui8_byte_received;
volatile uint8_t ui8_state_machine = 0;
static uint16_t  ui16_crc_rx;
static uint16_t  ui16_crc_tx;
							 
volatile uint8_t ui8_message_ID = 0;
volatile uint8_t ui8_packet_type = UART_PACKET_CONFIG;
volatile uint8_t ui8_missed_uart_packets = 0;
		 

static void communications_controller (void);
static void uart_receive_package (void);
static void uart_send_package (void);


// system functions
static void get_battery_voltage_filtered(void);
static void get_battery_current_filtered(void);
static void get_pedal_torque(void);
static void calc_wheel_speed(void);
static void calc_cadence(void);

static void ebike_control_lights(void);
static void ebike_control_motor(void);
static void check_system(void);

static void apply_power_assist();
static void apply_emtb_assist();
static void apply_walk_assist();
static void apply_cruise();
static void apply_calibration_assist();
static void apply_throttle();
static void apply_temperature_limiting();
static void apply_speed_limit();
static void linearize_torque_sensor_to_kgs(uint16_t *ui16_p_torque_sensor_adc_steps, uint16_t *ui16_torque_sensor_weight);


void ebike_app_controller (void)
{ 
  static uint8_t ui8_counter;  
  
  calc_wheel_speed();               // calculate the wheel speed
  calc_cadence();                   // calculate the cadence and set limits from wheel speed
  
  get_battery_voltage_filtered();   // get filtered voltage from FOC calculations
  get_battery_current_filtered();   // get filtered current from FOC calculations
  get_pedal_torque();               // get pedal torque
  
  check_system();                   // check if there are any errors for motor control 
    
  // send/receive data every 4 cycles (30ms * 4)
  if (!(ui8_counter++ & 0x03))
  communications_controller(); 		// get data to use for motor control and also send new data
  
  ebike_control_lights();           // use received data and sensor input to control external lights
  ebike_control_motor();            // use received data and sensor input to control motor
  
  /*------------------------------------------------------------------------
  
    NOTE: regarding function call order
    
    Do not change order of functions if not absolutely sure it will 
    not cause any undesirable consequences.
    
  ------------------------------------------------------------------------*/
}



static void ebike_control_motor (void)
{
  // reset control variables (safety)
  ui8_duty_cycle_ramp_up_inverse_step = PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_DEFAULT;
  ui8_duty_cycle_ramp_down_inverse_step = PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_DEFAULT;
  ui8_adc_battery_current_target = 0;
  ui8_duty_cycle_target = 0;

  // reset initialization of Cruise PID controller
  if (ui8_riding_mode != CRUISE_MODE) { ui8_cruise_PID_initialize = 1; }
  
  // select riding mode
  switch (ui8_riding_mode)
  {
    case POWER_ASSIST_MODE: apply_power_assist(); break;
   
    case eMTB_ASSIST_MODE: apply_emtb_assist(); break;
    
    case WALK_ASSIST_MODE: apply_walk_assist(); break;
    
    case CRUISE_MODE: apply_cruise(); break;
	
	case MOTOR_CALIBRATION_MODE: apply_calibration_assist(); break;

  }
  
  // select optional ADC function
  switch (m_configuration_variables.ui8_optional_ADC_function)
  {
    case THROTTLE_CONTROL: apply_throttle(); break;
    
    case TEMPERATURE_CONTROL: apply_temperature_limiting(); break;
  }
  
  // speed limit
  apply_speed_limit();
  
   // reset control parameters if... (safety)
    if (ui8_brake_state || ui8_system_state != NO_ERROR || !ui8_motor_enabled) {
        ui8_controller_duty_cycle_ramp_up_inverse_step = PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_DEFAULT;
        ui8_controller_duty_cycle_ramp_down_inverse_step = PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_MIN;
        ui8_controller_adc_battery_current_target = 0;
        ui8_controller_duty_cycle_target = 0;
    } else {
        // limit max current if higher than configured hardware limit (safety)
        if (ui8_adc_battery_current_max > ADC_10_BIT_BATTERY_CURRENT_MAX) {
            ui8_adc_battery_current_max = ADC_10_BIT_BATTERY_CURRENT_MAX;
        }

        // limit target current if higher than max value (safety)
        if (ui8_adc_battery_current_target > ui8_adc_battery_current_max) {
            ui8_adc_battery_current_target = ui8_adc_battery_current_max;
        }

        // limit target duty cycle if higher than max value
        if (ui8_duty_cycle_target > PWM_DUTY_CYCLE_MAX) {
            ui8_duty_cycle_target = PWM_DUTY_CYCLE_MAX;
        }

        // limit target duty cycle ramp up inverse step if lower than min value (safety)
        if (ui8_duty_cycle_ramp_up_inverse_step < PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_MIN) {
            ui8_duty_cycle_ramp_up_inverse_step = PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_MIN;
        }

        // limit target duty cycle ramp down inverse step if lower than min value (safety)
        if (ui8_duty_cycle_ramp_down_inverse_step < PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_MIN) {
            ui8_duty_cycle_ramp_down_inverse_step = PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_MIN;
        }

        // set duty cycle ramp up in controller
        ui8_controller_duty_cycle_ramp_up_inverse_step = ui8_duty_cycle_ramp_up_inverse_step;

        // set duty cycle ramp down in controller
        ui8_controller_duty_cycle_ramp_down_inverse_step = ui8_duty_cycle_ramp_down_inverse_step;

        // set target battery current in controller
        ui8_controller_adc_battery_current_target = ui8_adc_battery_current_target;

        // set target duty cycle in controller
        ui8_controller_duty_cycle_target = ui8_duty_cycle_target;
    }

    // check if the motor should be enabled or disabled
    if (ui8_motor_enabled
            && (ui16_motor_speed_erps == 0)
            && (!ui8_adc_battery_current_target)
            && (!ui8_g_duty_cycle)) {
        ui8_motor_enabled = 0;
        motor_disable_pwm();
    } else if (!ui8_motor_enabled
            && (ui16_motor_speed_erps < 50) // enable the motor only if it rotates slowly or is stopped
            && (ui8_adc_battery_current_target)
            && (!ui8_brake_state)) {
        ui8_motor_enabled = 1;
        ui8_duty_cycle_ramp_up_inverse_step = PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_MIN;
        ui8_duty_cycle_ramp_down_inverse_step = PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_MIN;
        ui8_g_duty_cycle = PWM_DUTY_CYCLE_STARTUP;
        ui8_fw_hall_counter_offset = 0;
        motor_enable_pwm();
    }

}



static void apply_power_assist()
{
  #define TORQUE_ASSIST_FACTOR_DENOMINATOR      (uint8_t)90   // scale the torque assist target current
  
  uint8_t  ui8_tmp;
  uint16_t ui16_adc_battery_current_target_power_assist;
  uint16_t ui16_adc_battery_current_target_torque_assist;
  uint16_t ui16_adc_battery_current_target;  
  
	// check for assist without pedal rotation threshold when there is no pedal rotation and standing still
  if (ui8_assist_without_pedal_rotation_threshold && !ui8_pedal_cadence_RPM)
	{
  if (ui16_adc_pedal_torque_delta > (90 - ui8_assist_without_pedal_rotation_threshold)) { ui8_pedal_cadence_RPM = 1;}
	}
	
    //soft start feature
  if (ui8_soft_start_feature_enabled && !ui16_wheel_speed_x10){ 
      ui8_power_assist_multiplier_x10 = ui8_riding_mode_parameter_power_soft_start;
      ui8_torque_assist_factor = ui8_riding_mode_parameter_torque_soft_start;
  }else{
	// get the torque assist factor
      ui8_power_assist_multiplier_x10 = ui8_riding_mode_parameter_power;
    // torque assist value 
      ui8_torque_assist_factor = ui8_riding_mode_parameter_torque;
  }
  
  // calculate power assist by multipling human power with the power assist multiplier
  uint32_t ui32_power_assist_x100 = ((uint32_t) ui16_pedal_torque_x100 * ui8_pedal_cadence_RPM * ui8_power_assist_multiplier_x10) / 96U; 
  
  /*------------------------------------------------------------------------

    NOTE: regarding the human power calculation
    
    (1) Formula: power = torque * rotations per second * 2 * pi
    (2) Formula: power = torque * rotations per minute * 2 * pi / 60
    (3) Formula: power = torque * rotations per minute * 0.1047
    (4) Formula: power = torque * 100 * rotations per minute * 0.001047
    (5) Formula: power = torque * 100 * rotations per minute / 955
    (6) Formula: power * 10  =  torque * 100 * rotations per minute / 96
    
  ------------------------------------------------------------------------*/  
  
  // calculate target current
  uint16_t ui16_battery_current_target_x100 = (uint16_t)((ui32_power_assist_x100 * 1000) / ui16_battery_voltage_filtered_x1000);
  
  // set battery current target power assist in ADC steps
  ui16_adc_battery_current_target_power_assist = ui16_battery_current_target_x100 / BATTERY_CURRENT_PER_10_BIT_ADC_STEP_X100;

  //apply torque assist if hybrid mode enabled
  if (ui8_hybrid_mode_enabled && ui16_adc_pedal_torque_delta && ui8_pedal_cadence_RPM){
  
  if (ui8_torque_linearization_enabled){ui16_adc_pedal_torque_delta = ui16_pedal_torque_x100 / 100;}
  // calculate torque assist target current
  ui16_adc_battery_current_target_torque_assist = ((uint16_t) ui16_adc_pedal_torque_delta * ui8_torque_assist_factor) / TORQUE_ASSIST_FACTOR_DENOMINATOR;
  }else{
  ui16_adc_battery_current_target_torque_assist = 0;}
  	
	// set battery current target in ADC steps
	if(ui16_adc_battery_current_target_power_assist > ui16_adc_battery_current_target_torque_assist){
		ui16_adc_battery_current_target = ui16_adc_battery_current_target_power_assist;
	}else{
		ui16_adc_battery_current_target = ui16_adc_battery_current_target_torque_assist;}
		
  
 // set motor acceleration
    if (ui16_wheel_speed_x10 >= 200) {
        ui8_duty_cycle_ramp_up_inverse_step = PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_MIN;
        ui8_duty_cycle_ramp_down_inverse_step = PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_MIN;
    } else {
            ui8_duty_cycle_ramp_up_inverse_step = map_ui8((uint8_t)(ui16_wheel_speed_x10>>2),
                    (uint8_t)10, // 10*4 = 40 -> 4 kph
                    (uint8_t)60, // 60*4 = 200 -> 24 kph
                    (uint8_t)ui8_duty_cycle_ramp_up_inverse_step_default,
                    (uint8_t)PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_MIN);
            ui8_tmp = map_ui8(ui8_pedal_cadence_RPM,
                    (uint8_t)20, // 20 rpm
                    (uint8_t)90, // 90 rpm
                    (uint8_t)ui8_duty_cycle_ramp_up_inverse_step_default,
                    (uint8_t)PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_MIN);
            
		if (ui8_tmp < ui8_duty_cycle_ramp_up_inverse_step)
                ui8_duty_cycle_ramp_up_inverse_step = ui8_tmp;

        ui8_duty_cycle_ramp_down_inverse_step = map_ui8((uint8_t)(ui16_wheel_speed_x10>>2),
                (uint8_t)10, // 10*4 = 40 -> 4 kph
                (uint8_t)60, // 60*4 = 200 -> 24 kph
                (uint8_t)PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_DEFAULT,
                (uint8_t)PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_MIN);
        ui8_tmp = map_ui8(ui8_pedal_cadence_RPM,
                (uint8_t)20, // 20 rpm
                (uint8_t)90, // 90 rpm
                (uint8_t)PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_DEFAULT,
                (uint8_t)PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_MIN);
        if (ui8_tmp < ui8_duty_cycle_ramp_down_inverse_step)
            ui8_duty_cycle_ramp_down_inverse_step = ui8_tmp;
    }


    // add field weakening current 
  if (ui8_field_weakening_enabled && ui8_field_weakening_current_adc){
        ui8_tmp = map_ui8(ui8_fw_hall_counter_offset,
                (uint8_t)2, 							// min fw angel - don't add current on low rpm
                (uint8_t)FW_HALL_COUNTER_OFFSET_MAX - 1, //max FW angle
                (uint8_t)0,
                (uint8_t)ui8_field_weakening_current_adc);
				
		ui16_adc_battery_current_target = ui16_adc_battery_current_target + ui8_tmp;
		}
		
   // set battery current target
  if (ui16_adc_battery_current_target > ui8_adc_battery_current_max) { ui8_adc_battery_current_target = ui8_adc_battery_current_max; }
  else { ui8_adc_battery_current_target = ui16_adc_battery_current_target; }
  
  //keep min current for faster motor engage
  if((ui8_adc_battery_current_min)&&(ui16_wheel_speed_x10 || ui8_pedal_cadence_RPM)&&(!ui8_adc_battery_current_target))
    {
	  ui8_adc_battery_current_target = ui8_adc_battery_current_min;
	}
	
   // set duty cycle target
  if (ui8_adc_battery_current_target) { ui8_duty_cycle_target = PWM_DUTY_CYCLE_MAX; }
  else { ui8_duty_cycle_target = 0; }
  
}



static void apply_emtb_assist()
{
  #define eMTB_ASSIST_ADC_TORQUE_OFFSET    10
  uint8_t  ui8_tmp;
  
  // check for assist without pedal rotation threshold when there is no pedal rotation and standing still
  if (ui8_assist_without_pedal_rotation_threshold && !ui8_pedal_cadence_RPM)
  {
    if (ui16_adc_pedal_torque_delta > (90 - ui8_assist_without_pedal_rotation_threshold)) { ui8_pedal_cadence_RPM = 1; }
  }
  
  if ((ui16_adc_pedal_torque_delta > 0) && 
      (ui16_adc_pedal_torque_delta < (eMTB_POWER_FUNCTION_ARRAY_SIZE - eMTB_ASSIST_ADC_TORQUE_OFFSET)) &&
      (ui8_pedal_cadence_RPM))
  {
    // initialize eMTB assist target current
    uint8_t ui8_adc_battery_current_target_eMTB_assist = 0;
    
    // get the eMTB assist sensitivity
    uint8_t ui8_eMTB_assist_sensitivity = ui8_riding_mode_parameter;

																									  
  
    switch (ui8_eMTB_assist_sensitivity)
    {
      case 1: ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_160[ui16_adc_pedal_torque_delta]; break;
      case 2: ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_165[ui16_adc_pedal_torque_delta]; break;
      case 3: ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_170[ui16_adc_pedal_torque_delta]; break;
      case 4: ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_175[ui16_adc_pedal_torque_delta]; break;
      case 5: ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_180[ui16_adc_pedal_torque_delta]; break;
      case 6: ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_195[ui16_adc_pedal_torque_delta]; break;
      case 7: ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_210[ui16_adc_pedal_torque_delta]; break;
      case 8: ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_225[ui16_adc_pedal_torque_delta]; break;
      case 9: ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_240[ui16_adc_pedal_torque_delta]; break;
      case 10: ui8_adc_battery_current_target_eMTB_assist = ui8_eMTB_power_function_255[ui16_adc_pedal_torque_delta]; break;
    }

    // set motor acceleration
    if (ui16_wheel_speed_x10 >= 200) {
            ui8_duty_cycle_ramp_up_inverse_step = PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_MIN;
            ui8_duty_cycle_ramp_down_inverse_step = PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_MIN;
    } else {
            ui8_duty_cycle_ramp_up_inverse_step = map_ui8((uint8_t)(ui16_wheel_speed_x10>>2),
                    (uint8_t)10, // 10*4 = 40 -> 4 kph
                    (uint8_t)60, // 60*4 = 200 -> 24 kph
                    (uint8_t)ui8_duty_cycle_ramp_up_inverse_step_default,
                    (uint8_t)PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_MIN);
            ui8_tmp = map_ui8(ui8_pedal_cadence_RPM,
                    (uint8_t)20, // 20 rpm
                    (uint8_t)90, // 90 rpm
                    (uint8_t)ui8_duty_cycle_ramp_up_inverse_step_default,
                    (uint8_t)PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_MIN);
    
	if (ui8_tmp < ui8_duty_cycle_ramp_up_inverse_step)
        ui8_duty_cycle_ramp_up_inverse_step = ui8_tmp;

            ui8_duty_cycle_ramp_down_inverse_step = map_ui8((uint8_t)(ui16_wheel_speed_x10>>2),
                    (uint8_t)10, // 10*4 = 40 -> 4 kph
                    (uint8_t)60, // 60*4 = 240 -> 24 kph
                    (uint8_t)PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_DEFAULT,
                    (uint8_t)PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_MIN);
            ui8_tmp = map_ui8(ui8_pedal_cadence_RPM,
                    (uint8_t)20, // 20 rpm
                    (uint8_t)90, // 90 rpm
                    (uint8_t)PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_DEFAULT,
                    (uint8_t)PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_MIN);
    if (ui8_tmp < ui8_duty_cycle_ramp_down_inverse_step)
        ui8_duty_cycle_ramp_down_inverse_step = ui8_tmp;
    }
		

    // add field weakening current 
    if (ui8_field_weakening_enabled && ui8_field_weakening_current_adc){
        ui8_tmp = map_ui8(ui8_fw_hall_counter_offset,
                (uint8_t)2, 							 	// min fw angel - don't add current on low rpm
                (uint8_t)FW_HALL_COUNTER_OFFSET_MAX - 1, 	//max FW angle
                (uint8_t)0,
                (uint8_t)ui8_field_weakening_current_adc);
				
		ui8_adc_battery_current_target_eMTB_assist = ui8_adc_battery_current_target_eMTB_assist + ui8_tmp;
		}
	
	// set battery current target
	if (ui8_adc_battery_current_target_eMTB_assist > ui8_adc_battery_current_max) { ui8_adc_battery_current_target = ui8_adc_battery_current_max; }
    else { ui8_adc_battery_current_target = ui8_adc_battery_current_target_eMTB_assist; }

	//keep min current for faster engage
	if((ui8_adc_battery_current_min)&&(ui16_wheel_speed_x10 || ui8_pedal_cadence_RPM)&&(!ui8_adc_battery_current_target))
    {
	  ui8_adc_battery_current_target = ui8_adc_battery_current_min;
	}
	
    // set duty cycle target
    if (ui8_adc_battery_current_target) { ui8_duty_cycle_target = PWM_DUTY_CYCLE_MAX; }
    else { ui8_duty_cycle_target = 0; }
  }
}



static void apply_calibration_assist() {
    // ui8_riding_mode_parameter contains the target duty cycle
    uint8_t ui8_calibration_assist_duty_cycle_target = ui8_riding_mode_parameter;

    // limit cadence assist duty cycle target
    if (ui8_calibration_assist_duty_cycle_target >= PWM_DUTY_CYCLE_MAX) {
        ui8_calibration_assist_duty_cycle_target = (uint8_t)(PWM_DUTY_CYCLE_MAX-1);
    }

    // set motor acceleration
    ui8_duty_cycle_ramp_up_inverse_step = PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_MIN;
    ui8_duty_cycle_ramp_down_inverse_step = PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_MIN;

    // set battery current target
    ui8_adc_battery_current_target = ui8_adc_battery_current_max;

    // set duty cycle target
    ui8_duty_cycle_target = ui8_calibration_assist_duty_cycle_target;
}



static void apply_walk_assist()
{
  #define WALK_ASSIST_DUTY_CYCLE_MAX                      80
  #define WALK_ASSIST_ADC_BATTERY_CURRENT_MAX             80
  
  if (ui16_wheel_speed_x10 < WALK_ASSIST_THRESHOLD_SPEED_X10)
  {
    // get the walk assist duty cycle target
    uint8_t ui8_walk_assist_duty_cycle_target = ui8_riding_mode_parameter;
    
    // check so that walk assist level factor is not too large (too powerful), if it is -> limit the value
    if (ui8_walk_assist_duty_cycle_target > WALK_ASSIST_DUTY_CYCLE_MAX) { ui8_walk_assist_duty_cycle_target = WALK_ASSIST_DUTY_CYCLE_MAX; }
    
    // set motor acceleration
    ui8_duty_cycle_ramp_up_inverse_step = WALK_ASSIST_DUTY_CYCLE_RAMP_UP_INVERSE_STEP;
    ui8_duty_cycle_ramp_down_inverse_step = PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_DEFAULT;
    
    // set battery current target
    ui8_adc_battery_current_target = ui8_min(WALK_ASSIST_ADC_BATTERY_CURRENT_MAX, ui8_adc_battery_current_max);
    
    // set duty cycle target
    ui8_duty_cycle_target = ui8_walk_assist_duty_cycle_target;
  }
}



static void apply_cruise()
{
#define CRUISE_PID_INTEGRAL_LIMIT                 1000
#define CRUISE_PID_KD                             0

    if (ui16_wheel_speed_x10 > CRUISE_THRESHOLD_SPEED_X10) {
        static int16_t i16_error;
        static int16_t i16_last_error;
        static int16_t i16_integral;
        static int16_t i16_derivative;
        static int16_t i16_control_output;
        static uint16_t ui16_wheel_speed_target_x10;

        // initialize cruise PID controller
        if (ui8_cruise_PID_initialize) {
            ui8_cruise_PID_initialize = 0;

            // reset PID variables
            i16_error = 0;
            i16_last_error = 0;
            i16_integral = 320; // initialize integral to a value so the motor does not start from zero
            i16_derivative = 0;
            i16_control_output = 0;

            // check what target wheel speed to use (received or current)
            ui16_wheel_speed_target_received_x10 = (uint16_t) ui8_riding_mode_parameter * 10;

            if (ui16_wheel_speed_target_received_x10 > 0) {
                // set received target wheel speed to target wheel speed
                ui16_wheel_speed_target_x10 = ui16_wheel_speed_target_received_x10;
            } else {
                // set current wheel speed to maintain
                ui16_wheel_speed_target_x10 = ui16_wheel_speed_x10;
            }
        }

        // calculate error
        i16_error = (ui16_wheel_speed_target_x10 - ui16_wheel_speed_x10);

        // calculate integral
        i16_integral = i16_integral + i16_error;

        // limit integral
        if (i16_integral > CRUISE_PID_INTEGRAL_LIMIT) {
            i16_integral = CRUISE_PID_INTEGRAL_LIMIT;
        } else if (i16_integral < 0) {
            i16_integral = 0;
        }

        // calculate derivative
        i16_derivative = i16_error - i16_last_error;

        // save error to last error
        i16_last_error = i16_error;

        // calculate control output ( output =  P I D )
        i16_control_output = (i16_cruise_pid_kp * i16_error) + (i16_cruise_pid_ki * i16_integral) + (CRUISE_PID_KD * i16_derivative);

        // limit control output to just positive values
        if (i16_control_output < 0) {
            i16_control_output = 0;
        }

        // limit control output to the maximum value
        if (i16_control_output > 1000) {
            i16_control_output = 1000;
        }

        // set motor acceleration
        ui8_duty_cycle_ramp_up_inverse_step = CRUISE_DUTY_CYCLE_RAMP_UP_INVERSE_STEP;
        ui8_duty_cycle_ramp_down_inverse_step = PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_DEFAULT;

        // set battery current target
        ui8_adc_battery_current_target = ui8_adc_battery_current_max;

        // set duty cycle target  |  map the control output to an appropriate target PWM value
        ui8_duty_cycle_target = map_ui8((uint8_t) (i16_control_output >> 2),
                (uint8_t)0,                   // minimum control output from PID
                (uint8_t)250,                 // maximum control output from PID
                (uint8_t)0,                   // minimum duty cycle
                (uint8_t)(PWM_DUTY_CYCLE_MAX-1)); // maximum duty cycle
    }
}



static void apply_throttle() {

    // map value from 0 to 255
    ui8_adc_throttle = map_ui8((uint8_t)(ui16_adc_throttle >> 2),
            (uint8_t) ADC_THROTTLE_MIN_VALUE,
            (uint8_t) ADC_THROTTLE_MAX_VALUE,
            (uint8_t) 0,
            (uint8_t) 255);

    // map ADC throttle value from 0 to max battery current
    uint8_t ui8_adc_battery_current_target_throttle = map_ui8((uint8_t) ui8_adc_throttle,
            (uint8_t) 0,
            (uint8_t) 255,
            (uint8_t) 0,
            (uint8_t) ui8_adc_battery_current_max);

    if (ui8_adc_battery_current_target_throttle > ui8_adc_battery_current_target) {
        // set motor acceleration
        if (ui16_wheel_speed_x10 >= 255) {
            ui8_duty_cycle_ramp_up_inverse_step = PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_MIN;
            ui8_duty_cycle_ramp_down_inverse_step = PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_MIN;
        } else {
            ui8_duty_cycle_ramp_up_inverse_step = map_ui8((uint8_t) ui16_wheel_speed_x10,
                    (uint8_t) 40,
                    (uint8_t) 255,
                    (uint8_t) THROTTLE_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_DEFAULT,
                    (uint8_t) THROTTLE_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_MIN);

            ui8_duty_cycle_ramp_down_inverse_step = map_ui8((uint8_t) ui16_wheel_speed_x10,
                    (uint8_t) 40,
                    (uint8_t) 255,
                    (uint8_t) PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_DEFAULT,
                    (uint8_t) PWM_DUTY_CYCLE_RAMP_DOWN_INVERSE_STEP_MIN);
        }
        // set battery current target
        ui8_adc_battery_current_target = ui8_adc_battery_current_target_throttle;

        // set duty cycle target
        ui8_duty_cycle_target = PWM_DUTY_CYCLE_MAX;
    }
}



static void apply_temperature_limiting()
{
  
  // get ADC measurement
  volatile uint16_t ui16_temp = ui16_adc_throttle;
  
  // filter ADC measurement to motor temperature variable
  ui16_adc_motor_temperature_filtered = filter(ui16_temp, ui16_adc_motor_temperature_filtered, 8);
  
    // convert ADC value
  ui8_motor_temperature_filtered = (uint8_t)((ui16_adc_motor_temperature_filtered * 39) / 80);

    // min temperature value can not be equal or higher than max temperature value
  if (ui8_motor_temperature_min_value_to_limit >= ui8_motor_temperature_max_value_to_limit) {
        ui8_adc_battery_current_target = 0;
    } else {
        // adjust target current if motor over temperature limit
        ui8_adc_battery_current_target = map_ui8(ui8_motor_temperature_filtered,
                ui8_motor_temperature_min_value_to_limit,
                ui8_motor_temperature_max_value_to_limit,
                ui8_adc_battery_current_target,
                0);
    }
}



static void apply_speed_limit() 
{
    if (m_configuration_variables.ui8_wheel_speed_max > 0) {
        // set battery current target limit based on speed limit, faster versions works up to limit of 90km/h
        if (m_configuration_variables.ui8_wheel_speed_max > 50) { // shift down to avoid use of slow map_ui16 function
            ui8_adc_battery_current_target = map_ui8((uint8_t)((ui16_wheel_speed_x10 >> 1) - 200),
                (uint8_t)(((uint16_t)(m_configuration_variables.ui8_wheel_speed_max) * (uint8_t)5U) - (uint8_t)210U),
                (uint8_t)(((uint16_t)(m_configuration_variables.ui8_wheel_speed_max) * (uint8_t)5U) - (uint8_t)195U),
                ui8_adc_battery_current_target,
                0);
        } else {
            ui8_adc_battery_current_target = map_ui8((uint8_t)(ui16_wheel_speed_x10 >> 1),
                (uint8_t)(((uint8_t)(m_configuration_variables.ui8_wheel_speed_max) * (uint8_t)5U) - (uint8_t)10U), // ramp down from 2km/h under
                (uint8_t)(((uint8_t)(m_configuration_variables.ui8_wheel_speed_max) * (uint8_t)5U) + (uint8_t)5U), // to 1km/h over
                ui8_adc_battery_current_target,
                0);
        } 
    }
}



static void calc_wheel_speed(void)
{ 
  // calc wheel speed in km/h
  if (ui16_wheel_speed_sensor_ticks)
  {
  uint16_t ui16_tmp = ui16_wheel_speed_sensor_ticks;
  // !!!warning if PWM_CYCLES_SECOND is not a multiple of 1000
  ui16_wheel_speed_x10 = ((uint32_t)m_configuration_variables.ui16_wheel_perimeter * ((PWM_CYCLES_SECOND/1000)*36U)) / ui16_tmp;	
  }
  else
  {
    ui16_wheel_speed_x10 = 0;
  }
}



static void calc_cadence(void) {
	
    // get the cadence sensor ticks
    uint16_t ui16_cadence_sensor_ticks_temp = ui16_cadence_sensor_ticks;

    // adjust cadence sensor ticks counter min depending on wheel speed
    uint8_t ui8_temp = map_ui8((uint8_t)(ui16_wheel_speed_x10 >> 2),
            10 /* 40 >> 2 */,
            100 /* 400 >> 2 */,
            (CADENCE_SENSOR_CALC_COUNTER_MIN >> 4),
            (CADENCE_SENSOR_TICKS_COUNTER_MIN_AT_SPEED >> 4));

    ui16_cadence_ticks_count_min_speed_adj = (uint16_t)ui8_temp << 4;

	// calculate cadence in RPM and avoid zero division
	if (ui16_cadence_sensor_ticks_temp)
		ui8_pedal_cadence_RPM = (PWM_CYCLES_SECOND * 3U) / ui16_cadence_sensor_ticks_temp;
	else
		ui8_pedal_cadence_RPM = 0;

      /*-------------------------------------------------------------------------------------------------
      
        NOTE: regarding the cadence calculation
        Cadence in standard mode is calculated by counting how many ticks there are between two 
        transitions of LOW to HIGH.
        
        Formula for calculating the cadence in RPM:
        (1) Cadence in RPM = (60 * PWM_CYCLES_SECOND) / CADENCE_SENSOR_NUMBER_MAGNETS) / ticks
		(2) Cadence in RPM = (PWM_CYCLES_SECOND * 3) / ticks
        
      -------------------------------------------------------------------------------------------------*/
}




static void get_battery_voltage_filtered(void)
{
  ui16_battery_voltage_filtered_x1000 = ui16_adc_battery_voltage_filtered * BATTERY_VOLTAGE_PER_10_BIT_ADC_STEP_X1000;
}




static void get_battery_current_filtered(void)
{
  ui8_battery_current_filtered_x10 = (uint16_t)(ui8_adc_battery_current_filtered * (uint8_t)BATTERY_CURRENT_PER_10_BIT_ADC_STEP_X100) / 10;
}



static void linearize_torque_sensor_to_kgs(uint16_t *ui16_p_torque_sensor_adc_steps, uint16_t *ui16_torque_sensor_weight_x10)
{
  uint16_t ui16_array_sum[TORQUE_SENSOR_LINEARIZE_NR_POINTS];
  uint8_t ui8_end = 0;
  uint16_t ui16_p_torque_sensor_adc_absolute_steps;

  memset(ui16_array_sum, 0, sizeof(ui16_array_sum));

  if(*ui16_p_torque_sensor_adc_steps > 0){
  ui16_p_torque_sensor_adc_absolute_steps = *ui16_p_torque_sensor_adc_steps + ui16_adc_pedal_torque_offset;

  if(ui16_p_torque_sensor_adc_absolute_steps < ui16_torque_sensor_linear_values[2])
  {
    ui16_array_sum[0] = *ui16_p_torque_sensor_adc_steps;
    ui8_end = 1;
  }
  else
  {
    ui16_array_sum[0] = ui16_torque_sensor_linear_values[2] - ui16_adc_pedal_torque_offset;
  }

  if(ui8_end == 0)
  {
    if(ui16_p_torque_sensor_adc_absolute_steps < ui16_torque_sensor_linear_values[4])
    {
      ui16_array_sum[1] = (ui16_p_torque_sensor_adc_absolute_steps - ui16_torque_sensor_linear_values[2]);
      ui8_end = 1;
    }
    else
    {
      ui16_array_sum[1] = ui16_torque_sensor_linear_values[4] - ui16_torque_sensor_linear_values[2];
    }
  }

  if(ui8_end == 0)
  {
    if(ui16_p_torque_sensor_adc_absolute_steps < ui16_torque_sensor_linear_values[6])
    {
      ui16_array_sum[2] = (ui16_p_torque_sensor_adc_absolute_steps - ui16_torque_sensor_linear_values[4]);
      ui8_end = 1;
    }
    else
    {
      ui16_array_sum[2] = ui16_torque_sensor_linear_values[6] - ui16_torque_sensor_linear_values[4];
    }
  }

  if(ui8_end == 0)
  {
    if(ui16_p_torque_sensor_adc_absolute_steps < ui16_torque_sensor_linear_values[8])
    {
      ui16_array_sum[3] = (ui16_p_torque_sensor_adc_absolute_steps - ui16_torque_sensor_linear_values[6]);
      ui8_end = 1;
    }
    else
    {
      ui16_array_sum[3] = ui16_torque_sensor_linear_values[8] - ui16_torque_sensor_linear_values[6];
    }
  }

    if(ui8_end == 0)
  {
    if(ui16_p_torque_sensor_adc_absolute_steps < ui16_torque_sensor_linear_values[10])
    {
      ui16_array_sum[4] = (ui16_p_torque_sensor_adc_absolute_steps - ui16_torque_sensor_linear_values[8]);
      ui8_end = 1;
    }
    else
    {
      ui16_array_sum[4] = ui16_torque_sensor_linear_values[10] - ui16_torque_sensor_linear_values[8];
    }
  }
  
  if(ui8_end == 0)
  {
    ui16_array_sum[5] = ui16_p_torque_sensor_adc_absolute_steps - ui16_torque_sensor_linear_values[10];
  }

  *ui16_torque_sensor_weight_x10 = 
     (ui16_array_sum[0] * ui16_torque_sensor_linear_values[1] +
      ui16_array_sum[1] * ui16_torque_sensor_linear_values[3] +
      ui16_array_sum[2] * ui16_torque_sensor_linear_values[5] +
      ui16_array_sum[3] * ui16_torque_sensor_linear_values[7] +
      ui16_array_sum[4] * ui16_torque_sensor_linear_values[9] +
	  ui16_array_sum[5] * ui16_torque_sensor_linear_values[11]) / 10;
  }
  // no torque_sensor_adc_steps
  else
  {
    *ui16_torque_sensor_weight_x10 = 0;
  }
}



#define TOFFSET_START_CYCLES 160 // Torque offset calculation stars after 160 cycles = 4sec (25ms*160)
#define TOFFSET_END_CYCLES 200   // Torque offset calculation ends after 200 cycles = 5sec (25ms*200)
static uint8_t toffset_cycle_counter = 0;

static void get_pedal_torque(void) {
    if (toffset_cycle_counter < TOFFSET_END_CYCLES) {
    	if (toffset_cycle_counter > TOFFSET_START_CYCLES) {
			uint16_t ui16_tmp = ui16_adc_torque;
			ui16_adc_pedal_torque_offset = filter(ui16_tmp, ui16_adc_pedal_torque_offset, 2);
    	}
        toffset_cycle_counter++;
        if (toffset_cycle_counter == TOFFSET_END_CYCLES) {
            ui16_adc_pedal_torque_offset += ADC_TORQUE_SENSOR_CALIBRATION_OFFSET;
        }
        ui16_adc_pedal_torque = ui16_adc_pedal_torque_offset;
    } else {
        // get adc pedal torque
        ui16_adc_pedal_torque = ui16_adc_torque;
    }

    // calculate the delta value of adc pedal torque and the adc pedal torque offset from calibration
    if (ui16_adc_pedal_torque > ui16_adc_pedal_torque_offset) {
        ui16_adc_pedal_torque_delta = ui16_adc_pedal_torque - ui16_adc_pedal_torque_offset;
    } else {
        ui16_adc_pedal_torque_delta = 0;
    }

  if ((ui8_torque_linearization_enabled) && (ui8_packet_type == UART_PACKET_REGULAR)){
   // linearize and calculate weight on pedals
  linearize_torque_sensor_to_kgs(&ui16_adc_pedal_torque_delta, &ui16_m_torque_sensor_weight_x10);
  ui16_pedal_torque_x100 = ui16_m_torque_sensor_weight_x10 * 17; 
  }else{
  // calculate torque on pedals
  ui16_pedal_torque_x100 = ui16_adc_pedal_torque_delta * m_configuration_variables.ui8_pedal_torque_per_10_bit_ADC_step_x100; // default = 67
  }

  }


  
  struct_configuration_variables* get_configuration_variables (void)
{
  return &m_configuration_variables;
}



static void check_system()
{
  #define MOTOR_BLOCKED_COUNTER_THRESHOLD               10    // 10  =>  1.0 second
  #define MOTOR_BLOCKED_BATTERY_CURRENT_THRESHOLD_X10   50    // 50  =>  5.0 amps
  #define MOTOR_BLOCKED_ERPS_THRESHOLD                  10    // 10 ERPS
  #define MOTOR_BLOCKED_RESET_COUNTER_THRESHOLD         100   // 100  =>  10 seconds
  
  static uint8_t ui8_motor_blocked_counter;
  static uint8_t ui8_motor_blocked_reset_counter;

  // if the motor blocked error is enabled start resetting it
  if (ui8_system_state == ERROR_MOTOR_BLOCKED)
  {
    // increment motor blocked reset counter with 100 milliseconds
    ui8_motor_blocked_reset_counter++;
    
    // check if the counter has counted to the set threshold for reset
    if (ui8_motor_blocked_reset_counter > MOTOR_BLOCKED_RESET_COUNTER_THRESHOLD)
    {
      // reset motor blocked error code
      if (ui8_system_state == ERROR_MOTOR_BLOCKED) { ui8_system_state = NO_ERROR; }
      
      // reset the counter that clears the motor blocked error
      ui8_motor_blocked_reset_counter = 0;
    }
  }
  else
  {
    // if battery current is over the current threshold and the motor ERPS is below threshold start setting motor blocked error code
    if ((ui8_battery_current_filtered_x10 > MOTOR_BLOCKED_BATTERY_CURRENT_THRESHOLD_X10) && (ui16_motor_speed_erps < MOTOR_BLOCKED_ERPS_THRESHOLD))
    {
      // increment motor blocked counter with 100 milliseconds
      ++ui8_motor_blocked_counter;
      
      // check if motor is blocked for more than some safe threshold
      if (ui8_motor_blocked_counter > MOTOR_BLOCKED_COUNTER_THRESHOLD)
      {
        // set error code
        ui8_system_state = ERROR_MOTOR_BLOCKED;
        
        // reset motor blocked counter as the error code is set
        ui8_motor_blocked_counter = 0;
      }
    }
    else
    {
      // current is below the threshold and/or motor ERPS is above the threshold so reset the counter
      ui8_motor_blocked_counter = 0;
    }
  }
  
  
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
  
  
  // check torque sensor
  if (((ui16_adc_pedal_torque_offset > 300) || (ui16_adc_pedal_torque_offset < 10) || (ui16_adc_pedal_torque > 500)) &&
      ((ui8_riding_mode == POWER_ASSIST_MODE) || (ui8_riding_mode == eMTB_ASSIST_MODE)))
  {
    // set error code
    ui8_system_state = ERROR_TORQUE_SENSOR;
  }
  else if (ui8_system_state == ERROR_TORQUE_SENSOR)
  {
    // reset error code
    ui8_system_state = NO_ERROR;
  }
  
  
    // check tx/rx  communication
  if (ui8_missed_uart_packets > 50)
  {
    // set error code
    ui8_system_state = ERROR_UART_LOST_COMMUNICATION;
  }
  else if (ui8_system_state == ERROR_UART_LOST_COMMUNICATION)
  {
    // reset error code
    ui8_system_state = NO_ERROR;
  }
  
}



void ebike_control_lights(void)
{
  #define DEFAULT_FLASH_ON_COUNTER_MAX      3
  #define DEFAULT_FLASH_OFF_COUNTER_MAX     1
  #define BRAKING_FLASH_ON_COUNTER_MAX      1
  #define BRAKING_FLASH_OFF_COUNTER_MAX     1
  
  static uint8_t ui8_default_flash_state;
  static uint8_t ui8_default_flash_state_counter; // increments every function call -> 100 ms
  static uint8_t ui8_braking_flash_state;
  static uint8_t ui8_braking_flash_state_counter; // increments every function call -> 100 ms
  
  
  /****************************************************************************/
  
  
  // increment flash counters
  ++ui8_default_flash_state_counter;
  ++ui8_braking_flash_state_counter;
  
  
  /****************************************************************************/
  
  
  // set default flash state
  if ((ui8_default_flash_state) && (ui8_default_flash_state_counter > DEFAULT_FLASH_ON_COUNTER_MAX))
  {
    // reset flash state counter
    ui8_default_flash_state_counter = 0;
    
    // toggle flash state
    ui8_default_flash_state = 0;
  }
  else if ((!ui8_default_flash_state) && (ui8_default_flash_state_counter > DEFAULT_FLASH_OFF_COUNTER_MAX))
  {
    // reset flash state counter
    ui8_default_flash_state_counter = 0;
    
    // toggle flash state
    ui8_default_flash_state = 1;
  }
  
  
  /****************************************************************************/
  
  
  // set braking flash state
  if ((ui8_braking_flash_state) && (ui8_braking_flash_state_counter > BRAKING_FLASH_ON_COUNTER_MAX))
  {
    // reset flash state counter
    ui8_braking_flash_state_counter = 0;
    
    // toggle flash state
    ui8_braking_flash_state = 0;
  }
  else if ((!ui8_braking_flash_state) && (ui8_braking_flash_state_counter > BRAKING_FLASH_OFF_COUNTER_MAX))
  {
    // reset flash state counter
    ui8_braking_flash_state_counter = 0;
    
    // toggle flash state
    ui8_braking_flash_state = 1;
  }
  
  
  /****************************************************************************/
  
  
  // select lights configuration
  switch (ui8_lights_configuration)
  {
    case 0:
    
      // set lights
      lights_set_state(ui8_lights_state);
      
    break;

    case 1:
      
      // check lights state
      if (ui8_lights_state)
      {
        // set lights
        lights_set_state(ui8_default_flash_state);
      }
      else
      {
        // set lights
        lights_set_state(ui8_lights_state);
      }
      
    break;
    
    case 2:
      
      // check light and brake state
      if (ui8_lights_state && ui8_brake_state)
      {
        // set lights
        lights_set_state(ui8_braking_flash_state);
      }
      else
      {
        // set lights
        lights_set_state(ui8_lights_state);
      }
      
    break;
    
    case 3:
      
      // check light and brake state
      if (ui8_lights_state && ui8_brake_state)
      {
        // set lights
        lights_set_state(ui8_brake_state);
      }
      else if (ui8_lights_state)
      {
        // set lights
        lights_set_state(ui8_default_flash_state);
      }
      else
      {
        // set lights
        lights_set_state(ui8_lights_state);
      }
      
    break;
    
    case 4:
      
      // check light and brake state
      if (ui8_lights_state && ui8_brake_state)
      {
        // set lights
        lights_set_state(ui8_braking_flash_state);
      }
      else if (ui8_lights_state)
      {
        // set lights
        lights_set_state(ui8_default_flash_state);
      }
      else
      {
        // set lights
        lights_set_state(ui8_lights_state);
      }
      
    break;
    
    case 5:
      
      // check brake state
      if (ui8_brake_state)
      {
        // set lights
        lights_set_state(ui8_brake_state);
      }
      else
      {
        // set lights
        lights_set_state(ui8_lights_state);
      }
      
    break;
    
    case 6:
      
      // check brake state
      if (ui8_brake_state)
      {
        // set lights
        lights_set_state(ui8_braking_flash_state);
      }
      else
      {
        // set lights
        lights_set_state(ui8_lights_state);
      }
      
    break;
    
    case 7:
      
      // check brake state
      if (ui8_brake_state)
      {
        // set lights
        lights_set_state(ui8_brake_state);
      }
      else if (ui8_lights_state)
      {
        // set lights
        lights_set_state(ui8_default_flash_state);
      }
      else
      {
        // set lights
        lights_set_state(ui8_lights_state);
      }
      
    break;
    
    case 8:
      
      // check brake state
      if (ui8_brake_state)
      {
        // set lights
        lights_set_state(ui8_braking_flash_state);
      }
      else if (ui8_lights_state)
      {
        // set lights
        lights_set_state(ui8_default_flash_state);
      }
      else
      {
        // set lights
        lights_set_state(ui8_lights_state);
      }
      
    break;
    
    default:
    
      // set lights
      lights_set_state(ui8_lights_state);
      
    break;
  }
  
  /*------------------------------------------------------------------------------------------------------------------

    NOTE: regarding the various light modes
    
    (0) lights ON when enabled
    (1) lights FLASHING when enabled
    
    (2) lights ON when enabled and BRAKE-FLASHING when braking
    (3) lights FLASHING when enabled and ON when braking
    (4) lights FLASHING when enabled and BRAKE-FLASHING when braking
    
    (5) lights ON when enabled, but ON when braking regardless if lights are enabled
    (6) lights ON when enabled, but BRAKE-FLASHING when braking regardless if lights are enabled
    
    (7) lights FLASHING when enabled, but ON when braking regardless if lights are enabled
    (8) lights FLASHING when enabled, but BRAKE-FLASHING when braking regardless if lights are enabled
    
  ------------------------------------------------------------------------------------------------------------------*/
}



// This is the interrupt that happens when UART2 receives data. We need it to be the fastest possible and so
// we do: receive every byte and assembly as a package, finally, signal that we have a package to process (on main slow loop)
// and disable the interrupt. The interrupt should be enable again on main loop, after the package being processed

void UART2_TX_IRQHandler(void) __interrupt(UART2_TX_IRQHANDLER)
{
  if (UART2->SR & 0x80) // save a few cycles
  {
    if(ui8_tx_buffer_index <= (UART_NUMBER_DATA_BYTES_TO_SEND + 3))  // bytes to send
    {
      // clearing the TXE bit is always performed by a write to the data register
      UART2->DR = (ui8_tx_buffer[ui8_tx_buffer_index]);
      ++ui8_tx_buffer_index;
      if(ui8_tx_buffer_index > (UART_NUMBER_DATA_BYTES_TO_SEND + 3))
      {
        // buffer empty
        // disable TIEN (TXE)
		ui8_tx_buffer_index = 0;
		// UART2_ITConfig(UART2_IT_TXE, DISABLE);
        UART2->CR2 &= 0x7F; // disable TIEN (TXE)
      }
    }
  }
  else
  {
    // TXE interrupt should never occur if there is nothing to send in the buffer
    // send a zero to clear TXE and disable the interrupt
	// UART2_SendData8(0);
		UART2->DR = 0;
	// UART2_ITConfig(UART2_IT_TXE, DISABLE);
        UART2->CR2 &= 0x7F;
  }
}


void UART2_RX_IRQHandler(void) __interrupt(UART2_RX_IRQHANDLER)
{
  //(UART2_GetFlagStatus(UART2_FLAG_TXE) == SET)
  if (UART2->SR & 0x20)
  {
    UART2->SR &= (uint8_t)~(UART2_FLAG_RXNE); // this may be redundant

    ui8_byte_received = UART2_ReceiveData8 ();

    switch (ui8_state_machine)
    {
      case 0:
      if (ui8_byte_received == 0x59) // see if we get start package byte
      {
        ui8_rx_buffer [ui8_rx_counter] = ui8_byte_received;
        ui8_rx_counter++;
        ui8_state_machine = 1;
      }
      else
      {
        ui8_rx_counter = 0;
        ui8_state_machine = 0;
      }
      break;

      case 1:
      ui8_rx_buffer [ui8_rx_counter] = ui8_byte_received;
      
      // increment index for next byte
      ui8_rx_counter++;

      // reset if it is the last byte of the package and index is out of bounds
      if (ui8_rx_counter >= UART_NUMBER_DATA_BYTES_TO_RECEIVE + 3)
      {
        ui8_rx_counter = 0;
        ui8_state_machine = 0;
        ui8_received_package_flag = 1; // signal that we have a full package to be processed
        UART2->CR2 &= ~(1 << 5); // disable UART2 receive interrupt
      }
      break;

      default:
      break;
    }
  }
}


// sending/receiving values every 120ms
static void communications_controller (void)
{
#ifndef DEBUG_UART
 

  if (ui8_missed_uart_packets > 4)
  ui8_riding_mode = OFF_MODE;

  uart_receive_package ();
  uart_send_package ();


#endif
}

static void uart_receive_package(void)
{
  if (ui8_received_package_flag)
  {
    // validation of the package data
    ui16_crc_rx = 0xffff;
    
    for (ui8_i = 0; ui8_i <= UART_NUMBER_DATA_BYTES_TO_RECEIVE; ui8_i++)
    {
      crc16 (ui8_rx_buffer[ui8_i], &ui16_crc_rx);
    }

    // if CRC is correct read the package (16 bit value and therefore last two bytes)
    if (((((uint16_t) ui8_rx_buffer [UART_NUMBER_DATA_BYTES_TO_RECEIVE + 2]) << 8) + ((uint16_t) ui8_rx_buffer [UART_NUMBER_DATA_BYTES_TO_RECEIVE + 1])) == ui16_crc_rx)
    {
      //packet type
	  ui8_packet_type = ui8_rx_buffer[1];
	  // message ID
      ui8_message_ID = ui8_rx_buffer [2];
      // riding mode
      ui8_riding_mode = ui8_rx_buffer [3];
      // riding mode parameter
	  if (ui8_riding_mode == POWER_ASSIST_MODE){
	  if (ui8_message_ID % 2 == 0){
      ui8_riding_mode_parameter_power = ui8_rx_buffer [4];
      }else{ui8_riding_mode_parameter_torque = ui8_rx_buffer [4];}
	  }
	  else{
	  ui8_riding_mode_parameter = ui8_rx_buffer [4];
	  }
	  
	  // lights state /torque_linearization/field_weakening/hybrid mode
      uint8_t ui8_temp = ui8_rx_buffer [5];
	  
	  ui8_lights_state = ui8_temp & 1; 
	  ui8_torque_linearization_enabled = (ui8_temp & 2) >> 1;
	  ui8_field_weakening_enabled = (ui8_temp & 4) >> 2;
	  ui8_hybrid_mode_enabled = (ui8_temp & 8) >> 3;
	  ui8_soft_start_feature_enabled = (ui8_temp & 16) >> 4;
	  
	  if(ui8_packet_type == UART_PACKET_REGULAR){
          
          // wheel max speed
          m_configuration_variables.ui8_wheel_speed_max = ui8_rx_buffer [6];
         
          // motor temperature limit function or throttle
          m_configuration_variables.ui8_optional_ADC_function = ui8_rx_buffer [7];
		
          // max battery current
          ui8_battery_current_max = ui8_rx_buffer[8];
          
          // motor acceleration adjustment
          uint8_t ui8_motor_acceleration_adjustment = ui8_rx_buffer[9];
          
          // set duty cycle ramp up inverse step
          ui8_duty_cycle_ramp_up_inverse_step_default = map_ui8((uint8_t)ui8_motor_acceleration_adjustment,
                        (uint8_t) 0,
                        (uint8_t) 100,
                        (uint8_t) PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_DEFAULT,
                        (uint8_t) PWM_DUTY_CYCLE_RAMP_UP_INVERSE_STEP_MIN);
		  
          // battery power limit
          m_configuration_variables.ui8_target_battery_max_power_div25 = ui8_rx_buffer[10];
          
          // calculate max battery current in ADC steps from the received battery current limit
		  uint8_t ui8_adc_battery_current_max_temp_1 = (uint16_t)(ui8_battery_current_max * (uint8_t)100) / BATTERY_CURRENT_PER_10_BIT_ADC_STEP_X100;
          
          // calculate max battery current in ADC steps from the received power limit
          uint32_t ui32_battery_current_max_x100 = ((uint32_t) m_configuration_variables.ui8_target_battery_max_power_div25 * 2500000) / ui16_battery_voltage_filtered_x1000;
          uint8_t ui8_adc_battery_current_max_temp_2 = ui32_battery_current_max_x100 / BATTERY_CURRENT_PER_10_BIT_ADC_STEP_X100;

          //  correct 0 Max Power setting on display
		  if (m_configuration_variables.ui8_target_battery_max_power_div25 == 0){ui8_adc_battery_current_max_temp_2 = ui8_adc_battery_current_max_temp_1;}

		   // set max battery current
          ui8_adc_battery_current_max = ui8_min(ui8_adc_battery_current_max_temp_1, ui8_adc_battery_current_max_temp_2);

	} 
	
	  if(ui8_packet_type == UART_PACKET_CONFIG){ // recieved only on power up so after changing values user must restart bike (power down/up bike)
	  
      switch (ui8_message_ID)
      {
        case 0:

		ui16_torque_sensor_linear_values[0] = ui8_rx_buffer [6];
        ui16_torque_sensor_linear_values[1] = (((uint16_t) ui8_rx_buffer [8]) << 8) + ((uint16_t) ui8_rx_buffer [7]);  
        m_configuration_variables.ui8_hall_ref_angles[0] = ui8_rx_buffer[9];
		m_configuration_variables.ui8_hall_counter_offsets[0] = ui8_rx_buffer[10];
		
		break;

        case 1:
        
        ui16_torque_sensor_linear_values[2] = (((uint16_t) ui8_rx_buffer [7]) << 8) + ((uint16_t) ui8_rx_buffer [6]);
		ui16_torque_sensor_linear_values[3] = ui8_rx_buffer [8];
		m_configuration_variables.ui8_hall_ref_angles[1] = ui8_rx_buffer[9];
		m_configuration_variables.ui8_hall_counter_offsets[1] = ui8_rx_buffer[10];

        break;

        case 2:
		
        ui16_torque_sensor_linear_values[4] = (((uint16_t) ui8_rx_buffer [7]) << 8) + ((uint16_t) ui8_rx_buffer [6]);
		ui16_torque_sensor_linear_values[5] = ui8_rx_buffer [8];
		m_configuration_variables.ui8_hall_ref_angles[2] = ui8_rx_buffer[9];
		m_configuration_variables.ui8_hall_counter_offsets[2] = ui8_rx_buffer[10];
        
		break;

        case 3:
        
        ui16_torque_sensor_linear_values[6] = (((uint16_t) ui8_rx_buffer [7]) << 8) + ((uint16_t) ui8_rx_buffer [6]);
		ui16_torque_sensor_linear_values[7] = ui8_rx_buffer [8];
		m_configuration_variables.ui8_hall_ref_angles[3] = ui8_rx_buffer[9];
		m_configuration_variables.ui8_hall_counter_offsets[3] = ui8_rx_buffer[10];
          
        break;

        case 4:
          
        ui16_torque_sensor_linear_values[8] = (((uint16_t) ui8_rx_buffer [7]) << 8) + ((uint16_t) ui8_rx_buffer [6]);
		ui16_torque_sensor_linear_values[9] = ui8_rx_buffer [8];
		m_configuration_variables.ui8_hall_ref_angles[4] = ui8_rx_buffer[9];
		m_configuration_variables.ui8_hall_counter_offsets[4] = ui8_rx_buffer[10];
                                                             
        break;

        case 5:
		
        ui16_torque_sensor_linear_values[10] = (((uint16_t) ui8_rx_buffer [7]) << 8) + ((uint16_t) ui8_rx_buffer [6]);
		ui16_torque_sensor_linear_values[11] = ui8_rx_buffer [8];
        m_configuration_variables.ui8_hall_ref_angles[5] = ui8_rx_buffer[9];
		m_configuration_variables.ui8_hall_counter_offsets[5] = ui8_rx_buffer[10];		
        break;
        
        case 6:
		// first power level for soft start
		ui8_riding_mode_parameter_power_soft_start = ui8_rx_buffer [6];
		ui8_riding_mode_parameter_torque_soft_start = ui8_rx_buffer [7];
		
		// battery low voltage cut off x10
		m_configuration_variables.ui16_battery_low_voltage_cut_off_x10 = (((uint16_t) ui8_rx_buffer[9]) << 8) + ((uint16_t) ui8_rx_buffer[8]);
		
		// set low voltage cutoff (8 bit)
        ui16_adc_voltage_cut_off = ((uint32_t) m_configuration_variables.ui16_battery_low_voltage_cut_off_x10 * 25U) / BATTERY_VOLTAGE_PER_10_BIT_ADC_STEP_X1000;
        
		// type of motor (36 volt, 48 volt or some experimental type)
        m_configuration_variables.ui8_motor_type = ui8_rx_buffer[10];
		  
		  if(m_configuration_variables.ui8_motor_type == 0)
		  {
			// 48 V motor
			i16_cruise_pid_kp = 12;
			i16_cruise_pid_ki = 1;
		  }
		  else
		  {
			// 36 V motor
			i16_cruise_pid_kp = 14;
			i16_cruise_pid_ki = 0.7;
		  }
		
		break;

		case 7:
		
		  // wheel perimeter
          m_configuration_variables.ui16_wheel_perimeter = (((uint16_t) ui8_rx_buffer [7]) << 8) + ((uint16_t) ui8_rx_buffer [6]);
		
    	  // pedal torque conversion
          m_configuration_variables.ui8_pedal_torque_per_10_bit_ADC_step_x100 = ui8_rx_buffer[8];
	
    	  // field weakening current - ADC value 1 ADC = 0.156  A
    	  ui8_field_weakening_current_adc = ui8_rx_buffer[9];	
 	
    	  // lights configuration
          ui8_lights_configuration = ui8_rx_buffer[10];		  
		
		break;
		
		case 8:
		
          // motor over temperature min value limit
          ui8_motor_temperature_min_value_to_limit = ui8_rx_buffer[6];
          
          // motor over temperature max value limit
          ui8_motor_temperature_max_value_to_limit = ui8_rx_buffer[7];
		  
		  // assist without pedal rotation threshold
          ui8_assist_without_pedal_rotation_threshold = ui8_rx_buffer[8];
		  
		  // check if assist without pedal rotation threshold is valid (safety)
          if (ui8_assist_without_pedal_rotation_threshold > 100) { ui8_assist_without_pedal_rotation_threshold = 0; }

         break;
		
		default:
          // nothing, should display error code
        break;
      }
    }    
  }
    
    // signal that we processed the full package
    ui8_received_package_flag = 0;
    ui8_missed_uart_packets = 0;
    // enable UART2 receive interrupt as we are now ready to receive a new package
    UART2->CR2 |= (1 << 5);
  }
}

static void uart_send_package(void)
{
  uint16_t ui16_temp;

  // start up byte
  ui8_tx_buffer[0] = 0x43;

  // battery voltage filtered x1000
  ui16_temp = ui16_battery_voltage_filtered_x1000;
  ui8_tx_buffer[1] = (uint8_t) (ui16_temp & 0xff);
  ui8_tx_buffer[2] = (uint8_t) (ui16_temp >> 8);
  
  // battery current filtered x10
  ui8_tx_buffer[3] = ui8_battery_current_filtered_x10;

  // wheel speed x10
  ui8_tx_buffer[4] = (uint8_t) (ui16_wheel_speed_x10 & 0xff);
  ui8_tx_buffer[5] = (uint8_t) (ui16_wheel_speed_x10 >> 8);

  // brake state
  ui8_tx_buffer[6] = ui8_brake_state;

  // optional ADC channel value
  ui8_tx_buffer[7] = (uint8_t)(ui16_adc_throttle >> 2);
  
  // throttle or temperature control
  switch (m_configuration_variables.ui8_optional_ADC_function)
  {
    case THROTTLE_CONTROL:
      
      // throttle value with offset applied and mapped from 0 to 255
      ui8_tx_buffer[8] = ui8_adc_throttle;
    
    break;
    
    case TEMPERATURE_CONTROL:
    
      // temperature
      ui8_tx_buffer[8] = ui8_motor_temperature_filtered;
    
    break;
  }
  // pedal cadence
  ui8_tx_buffer[9] = ui8_pedal_cadence_RPM;
    
  if (ui8_riding_mode == MOTOR_CALIBRATION_MODE) {
        ui16_temp = ui16_hall_calib_cnt[0];
        ui8_tx_buffer[10] = (uint8_t) (ui16_temp & 0xff);
        ui8_tx_buffer[11] = (uint8_t) (ui16_temp >> 8);
        ui16_temp = ui16_hall_calib_cnt[1];
        ui8_tx_buffer[12] = (uint8_t) (ui16_temp & 0xff);
        ui8_tx_buffer[13] = (uint8_t) (ui16_temp >> 8);
        ui16_temp = ui16_hall_calib_cnt[2];
        ui8_tx_buffer[14] = (uint8_t) (ui16_temp & 0xff);
        ui8_tx_buffer[15] = (uint8_t) (ui16_temp >> 8);
        ui16_temp = ui16_hall_calib_cnt[3];
        ui8_tx_buffer[16] = (uint8_t) (ui16_temp & 0xff);
        ui8_tx_buffer[17] = (uint8_t) (ui16_temp >> 8);
        ui16_temp = ui16_hall_calib_cnt[4];
        ui8_tx_buffer[18] = (uint8_t) (ui16_temp & 0xff);
        ui8_tx_buffer[19] = (uint8_t) (ui16_temp >> 8);
        ui16_temp = ui16_hall_calib_cnt[5];
        ui8_tx_buffer[20] = (uint8_t) (ui16_temp & 0xff);
        ui8_tx_buffer[21] = (uint8_t) (ui16_temp >> 8);
   } else {  
   // ADC torque sensor
  ui16_temp = ui16_adc_pedal_torque;
  ui8_tx_buffer[10] = (uint8_t) (ui16_temp & 0xff);
  ui8_tx_buffer[11] = (uint8_t) (ui16_temp >> 8);

  // PWM duty_cycle
  // convert duty-cycle to 0 - 100 %

  ui16_temp = (uint16_t) ui8_g_duty_cycle;
  ui16_temp = (ui16_temp * 100) / PWM_DUTY_CYCLE_MAX;
  ui8_tx_buffer[12] = (uint8_t) ui16_temp;
  
  // motor speed in ERPS
  ui16_temp = ui16_motor_speed_erps;
  ui8_tx_buffer[13] = (uint8_t) (ui16_temp & 0xff);
  ui8_tx_buffer[14] = (uint8_t) (ui16_temp >> 8);
  
  // FOC angle
  ui8_tx_buffer[15] = ui8_g_foc_angle;
  
  // system state
  ui8_tx_buffer[16] = ui8_system_state;
  
  // wheel_speed_sensor_tick_counter
  ui8_tx_buffer[17] = (uint8_t) (ui32_wheel_speed_sensor_ticks_total & 0xff);
  ui8_tx_buffer[18] = (uint8_t) ((ui32_wheel_speed_sensor_ticks_total >> 8) & 0xff);
  ui8_tx_buffer[19] = (uint8_t) ((ui32_wheel_speed_sensor_ticks_total >> 16) & 0xff);

  // pedal torque x100
  ui16_temp = ui16_pedal_torque_x100;
  ui8_tx_buffer[20] = (uint8_t) (ui16_temp & 0xff);
  ui8_tx_buffer[21] = (uint8_t) (ui16_temp >> 8);
  } 

  // prepare crc of the package
  ui16_crc_tx = 0xffff;
  
  for (ui8_i = 0; ui8_i <= UART_NUMBER_DATA_BYTES_TO_SEND; ui8_i++)
  {
    crc16 (ui8_tx_buffer[ui8_i], &ui16_crc_tx);
  }
  
  ui8_tx_buffer[UART_NUMBER_DATA_BYTES_TO_SEND + 1] = (uint8_t) (ui16_crc_tx & 0xff);
  ui8_tx_buffer[UART_NUMBER_DATA_BYTES_TO_SEND + 2] = (uint8_t) (ui16_crc_tx >> 8) & 0xff;
  
  ui8_missed_uart_packets++;
  // start transmition
  UART2_ITConfig(UART2_IT_TXE, ENABLE);
}