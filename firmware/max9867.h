/******************************************************************************
 *
 * Copyright (C) 2022-2023 Maxim Integrated Products, Inc. (now owned by
 * Analog Devices, Inc.),
 * Copyright (C) 2023-2024 Analog Devices, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ******************************************************************************/

#ifndef AUDIO_PLAYER_MAX9867_H_
#define AUDIO_PLAYER_MAX9867_H_

#include <i2c_regs.h>

/**
 * @brief  Initialise the MAX9867 stereo audio codec.
 *
 * @param  i2c_inst  I2C peripheral instance (e.g. MXC_I2C1)
 * @param  mclk      Master-clock frequency in Hz fed to the codec
 * @param  lrclk     Desired DAC/ADC sample rate in Hz (e.g. 44100, 48000)
 * @return E_NO_ERROR on success, negative error code otherwise
 */
int max9867_init(mxc_i2c_regs_t *i2c_inst, int mclk, int lrclk);

/**
 * @brief  Power-down the MAX9867 codec.
 * @return E_NO_ERROR on success, negative error code otherwise
 */
int max9867_shutdown(void);

/**
 * @brief  Read the MAX9867 status register.
 * @return Status register value, or negative error code
 */
int max9867_status(void);

/**
 * @brief  Set volume level on MAX9867 (0 = Mute/Low, 31 = Max).
 * @param  vol Volume level from 0 to 31
 * @return E_NO_ERROR on success
 */
int max9867_set_volume(int vol);

#endif /* AUDIO_PLAYER_MAX9867_H_ */
