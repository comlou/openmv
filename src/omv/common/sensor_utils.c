/*
 * This file is part of the OpenMV project.
 *
 * Copyright (c) 2013-2021 Ibrahim Abdelkader <iabdalkader@openmv.io>
 * Copyright (c) 2013-2021 Kwabena W. Agyeman <kwagyeman@openmv.io>
 *
 * This work is licensed under the MIT license, see the file LICENSE for details.
 *
 * This file contains image sensor driver utility functions and some default (weak)
 * implementations of common functions that can be replaced by port-specific drivers.
 */
#if MICROPY_PY_SENSOR
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "py/mphal.h"
#include "sensor.h"
#include "ov2640.h"
#include "ov5640.h"
#include "ov7725.h"
#include "ov7670.h"
#include "ov7690.h"
#include "ov9650.h"
#include "mt9v0xx.h"
#include "mt9m114.h"
#include "lepton.h"
#include "hm01b0.h"
#include "hm0360.h"
#include "pag7920.h"
#include "paj6100.h"
#include "frogeye2020.h"
#include "gc2145.h"
#include "framebuffer.h"
#include "unaligned_memcpy.h"
#include "omv_boardconfig.h"
#include "omv_gpio.h"
#include "omv_i2c.h"

#ifndef OMV_CSI_MAX_DEVICES
#define OMV_CSI_MAX_DEVICES (5)
#endif

#ifndef OMV_CSI_RESET_DELAY
#define OMV_CSI_RESET_DELAY (10)
#endif

#ifndef OMV_CSI_POWER_DELAY
#define OMV_CSI_POWER_DELAY (10)
#endif

#ifndef __weak
#define __weak    __attribute__((weak))
#endif

// Sensor frame size/resolution table.
const int resolution[][2] = {
    {0,    0   },
    // C/SIF Resolutions
    {88,   72  },    /* QQCIF     */
    {176,  144 },    /* QCIF      */
    {352,  288 },    /* CIF       */
    {88,   60  },    /* QQSIF     */
    {176,  120 },    /* QSIF      */
    {352,  240 },    /* SIF       */
    // VGA Resolutions
    {40,   30  },    /* QQQQVGA   */
    {80,   60  },    /* QQQVGA    */
    {160,  120 },    /* QQVGA     */
    {320,  240 },    /* QVGA      */
    {640,  480 },    /* VGA       */
    {30,   20  },    /* HQQQQVGA  */
    {60,   40  },    /* HQQQVGA   */
    {120,  80  },    /* HQQVGA    */
    {240,  160 },    /* HQVGA     */
    {480,  320 },    /* HVGA      */
    // FFT Resolutions
    {64,   32  },    /* 64x32     */
    {64,   64  },    /* 64x64     */
    {128,  64  },    /* 128x64    */
    {128,  128 },    /* 128x128   */
    // Himax Resolutions
    {160,  160 },    /* 160x160   */
    {320,  320 },    /* 320x320   */
    // Other
    {128,  160 },    /* LCD       */
    {128,  160 },    /* QQVGA2    */
    {720,  480 },    /* WVGA      */
    {752,  480 },    /* WVGA2     */
    {800,  600 },    /* SVGA      */
    {1024, 768 },    /* XGA       */
    {1280, 768 },    /* WXGA      */
    {1280, 1024},    /* SXGA      */
    {1280, 960 },    /* SXGAM     */
    {1600, 1200},    /* UXGA      */
    {1280, 720 },    /* HD        */
    {1920, 1080},    /* FHD       */
    {2560, 1440},    /* QHD       */
    {2048, 1536},    /* QXGA      */
    {2560, 1600},    /* WQXGA     */
    {2592, 1944},    /* WQXGA2    */
};

__weak void sensor_init0() {
    // Reset the sensor state
    memset(&sensor, 0, sizeof(sensor_t));
}

__weak int sensor_init() {
    // Reset the sensor state
    memset(&sensor, 0, sizeof(sensor_t));
    return SENSOR_ERROR_CTL_UNSUPPORTED;
}

__weak int sensor_abort(bool fifo_flush, bool in_irq) {
    return SENSOR_ERROR_CTL_UNSUPPORTED;
}

__weak int sensor_reset() {
    // Disable any ongoing frame capture.
    sensor_abort(true, false);

    // Reset the sensor state
    sensor.sde = 0;
    sensor.pixformat = 0;
    sensor.framesize = 0;
    sensor.framerate = 0;
    sensor.first_line = false;
    sensor.drop_frame = false;
    sensor.last_frame_ms = 0;
    sensor.last_frame_ms_valid = false;
    sensor.gainceiling = 0;
    sensor.hmirror = false;
    sensor.vflip = false;
    sensor.transpose = false;
    #if MICROPY_PY_IMU
    sensor.auto_rotation = (sensor.chip_id == OV7690_ID);
    #else
    sensor.auto_rotation = false;
    #endif // MICROPY_PY_IMU
    sensor.vsync_callback = NULL;
    sensor.frame_callback = NULL;

    // Reset default color palette.
    sensor.color_palette = rainbow_table;

    sensor.disable_full_flush = false;

    // Restore shutdown state on reset.
    sensor_shutdown(false);

    // Disable the bus before reset.
    omv_i2c_enable(&sensor.i2c_bus, false);

    #if defined(OMV_CSI_RESET_PIN)
    // Hard-reset the sensor
    if (sensor.reset_pol == ACTIVE_HIGH) {
        omv_gpio_write(OMV_CSI_RESET_PIN, 1);
        mp_hal_delay_ms(10);
        omv_gpio_write(OMV_CSI_RESET_PIN, 0);
    } else {
        omv_gpio_write(OMV_CSI_RESET_PIN, 0);
        mp_hal_delay_ms(10);
        omv_gpio_write(OMV_CSI_RESET_PIN, 1);
    }
    #endif

    mp_hal_delay_ms(OMV_CSI_RESET_DELAY);

    // Re-enable the bus.
    omv_i2c_enable(&sensor.i2c_bus, true);

    // Call sensor-specific reset function
    if (sensor.reset != NULL
        && sensor.reset(&sensor) != 0) {
        return SENSOR_ERROR_CTL_FAILED;
    }

    // Reset framebuffers
    framebuffer_flush_buffers(true);

    return 0;
}

static int sensor_detect() {
    uint8_t devs_list[OMV_CSI_MAX_DEVICES];
    int n_devs = omv_i2c_scan(&sensor.i2c_bus, devs_list, OMV_ARRAY_SIZE(devs_list));

    for (int i = 0; i < OMV_MIN(n_devs, OMV_CSI_MAX_DEVICES); i++) {
        uint8_t slv_addr = devs_list[i];
        switch (slv_addr) {
            #if (OMV_OV2640_ENABLE == 1)
            case OV2640_SLV_ADDR: // Or OV9650.
                omv_i2c_readb(&sensor.i2c_bus, slv_addr, OV_CHIP_ID, &sensor.chip_id);
                return slv_addr;
            #endif // (OMV_OV2640_ENABLE == 1)

            #if (OMV_OV5640_ENABLE == 1) || (OMV_GC2145_ENABLE == 1)
            // OV5640 and GC2145 share the same I2C address
            case OV5640_SLV_ADDR:   // Or GC2145
                // Try to read GC2145 chip ID first
                omv_i2c_readb(&sensor.i2c_bus, slv_addr, GC_CHIP_ID, &sensor.chip_id);
                if (sensor.chip_id != GC2145_ID) {
                    // If it fails, try reading OV5640 chip ID.
                    omv_i2c_readb2(&sensor.i2c_bus, slv_addr, OV5640_CHIP_ID, &sensor.chip_id);
                }
                return slv_addr;
            #endif // (OMV_OV5640_ENABLE == 1) || (OMV_GC2145_ENABLE == 1)

            #if (OMV_OV7725_ENABLE == 1) || (OMV_OV7670_ENABLE == 1) || (OMV_OV7690_ENABLE == 1)
            case OV7725_SLV_ADDR: // Or OV7690 or OV7670.
                omv_i2c_readb(&sensor.i2c_bus, slv_addr, OV_CHIP_ID, &sensor.chip_id);
                return slv_addr;
            #endif //(OMV_OV7725_ENABLE == 1) || (OMV_OV7670_ENABLE == 1) || (OMV_OV7690_ENABLE == 1)

            #if (OMV_MT9V0XX_ENABLE == 1)
            case MT9V0XX_SLV_ADDR:
                omv_i2c_readw(&sensor.i2c_bus, slv_addr, ON_CHIP_ID, &sensor.chip_id_w);
                return slv_addr;
            #endif //(OMV_MT9V0XX_ENABLE == 1)

            #if (OMV_MT9M114_ENABLE == 1)
            case MT9M114_SLV_ADDR:
                omv_i2c_readw2(&sensor.i2c_bus, slv_addr, ON_CHIP_ID, &sensor.chip_id_w);
                return slv_addr;
            #endif // (OMV_MT9M114_ENABLE == 1)

            #if (OMV_LEPTON_ENABLE == 1)
            case LEPTON_SLV_ADDR:
                sensor.chip_id = LEPTON_ID;
                return slv_addr;
            #endif // (OMV_LEPTON_ENABLE == 1)

            #if (OMV_HM01B0_ENABLE == 1) || (OMV_HM0360_ENABLE == 1)
            case HM0XX0_SLV_ADDR:
                omv_i2c_readb2(&sensor.i2c_bus, slv_addr, HIMAX_CHIP_ID, &sensor.chip_id);
                return slv_addr;
            #endif // (OMV_HM01B0_ENABLE == 1) || (OMV_HM0360_ENABLE == 1)

            #if (OMV_FROGEYE2020_ENABLE == 1)
            case FROGEYE2020_SLV_ADDR:
                sensor.chip_id_w = FROGEYE2020_ID;
                return slv_addr;
            #endif // (OMV_FROGEYE2020_ENABLE == 1)

            #if (OMV_PAG7920_ENABLE == 1)
            case PAG7920_SLV_ADDR:
                omv_i2c_readw(&sensor.i2c_bus, slv_addr, ON_CHIP_ID, &sensor.chip_id_w);
                sensor.chip_id_w = (sensor.chip_id_w << 8) | (sensor.chip_id_w >> 8);
                return slv_addr;
            #endif // (OMV_PAG7920_ENABLE == 1)
        }
    }

    return 0;
}

int sensor_probe_init(uint32_t bus_id, uint32_t bus_speed) {
    int init_ret = 0;

    #if defined(OMV_CSI_POWER_PIN)
    sensor.power_pol = ACTIVE_HIGH;
    // Do a power cycle
    omv_gpio_write(OMV_CSI_POWER_PIN, 1);
    mp_hal_delay_ms(10);

    omv_gpio_write(OMV_CSI_POWER_PIN, 0);
    mp_hal_delay_ms(OMV_CSI_POWER_DELAY);
    #endif

    #if defined(OMV_CSI_RESET_PIN)
    sensor.reset_pol = ACTIVE_HIGH;
    // Reset the sensor
    omv_gpio_write(OMV_CSI_RESET_PIN, 1);
    mp_hal_delay_ms(10);

    omv_gpio_write(OMV_CSI_RESET_PIN, 0);
    mp_hal_delay_ms(OMV_CSI_RESET_DELAY);
    #endif

    // Initialize the camera bus.
    omv_i2c_init(&sensor.i2c_bus, bus_id, bus_speed);
    mp_hal_delay_ms(10);

    // Scan the bus multiple times using different reset and power-down
    // polarities, until a supported sensor is detected.
    if ((sensor.slv_addr = sensor_detect()) == 0) {
        // No devices were detected, try scanning the bus
        // again with different reset/power-down polarities.
        #if defined(OMV_CSI_RESET_PIN)
        sensor.reset_pol = ACTIVE_LOW;
        omv_gpio_write(OMV_CSI_RESET_PIN, 1);
        mp_hal_delay_ms(OMV_CSI_RESET_DELAY);
        #endif

        if ((sensor.slv_addr = sensor_detect()) == 0) {
            #if defined(OMV_CSI_POWER_PIN)
            sensor.power_pol = ACTIVE_LOW;
            omv_gpio_write(OMV_CSI_POWER_PIN, 1);
            mp_hal_delay_ms(OMV_CSI_POWER_DELAY);
            #endif

            if ((sensor.slv_addr = sensor_detect()) == 0) {
                #if defined(OMV_CSI_RESET_PIN)
                sensor.reset_pol = ACTIVE_HIGH;
                omv_gpio_write(OMV_CSI_RESET_PIN, 0);
                mp_hal_delay_ms(OMV_CSI_RESET_DELAY);
                #endif
                sensor.slv_addr = sensor_detect();
            }
        }

        // If no devices were detected on the I2C bus, try the SPI bus.
        if (sensor.slv_addr == 0) {
            if (0) {
            #if (OMV_PAJ6100_ENABLE == 1)
            } else if (paj6100_detect(&sensor)) {
                // Found PixArt PAJ6100
                sensor.chip_id_w = PAJ6100_ID;
                sensor.power_pol = ACTIVE_LOW;
                sensor.reset_pol = ACTIVE_LOW;
            #endif
            } else {
                return SENSOR_ERROR_ISC_UNDETECTED;
            }
        }
    }

    // A supported sensor was detected, try to initialize it.
    switch (sensor.chip_id_w) {
        #if (OMV_OV2640_ENABLE == 1)
        case OV2640_ID:
            if (sensor_set_xclk_frequency(OMV_OV2640_XCLK_FREQ) != 0) {
                return SENSOR_ERROR_TIM_INIT_FAILED;
            }
            init_ret = ov2640_init(&sensor);
            break;
        #endif // (OMV_OV2640_ENABLE == 1)

        #if (OMV_OV5640_ENABLE == 1)
        case OV5640_ID: {
            int freq = OMV_OV5640_XCLK_FREQ;
            #if (OMV_OV5640_REV_Y_CHECK == 1)
            if (HAL_GetREVID() < 0x2003) {
                // Is this REV Y?
                freq = OMV_OV5640_REV_Y_FREQ;
            }
            #endif
            if (sensor_set_xclk_frequency(freq) != 0) {
                return SENSOR_ERROR_TIM_INIT_FAILED;
            }
            init_ret = ov5640_init(&sensor);
            break;
        }
        #endif // (OMV_OV5640_ENABLE == 1)

        #if (OMV_OV7670_ENABLE == 1)
        case OV7670_ID:
            if (sensor_set_xclk_frequency(OMV_OV7670_XCLK_FREQ) != 0) {
                return SENSOR_ERROR_TIM_INIT_FAILED;
            }
            init_ret = ov7670_init(&sensor);
            break;
        #endif // (OMV_OV7670_ENABLE == 1)

        #if (OMV_OV7690_ENABLE == 1)
        case OV7690_ID:
            if (sensor_set_xclk_frequency(OMV_OV7690_XCLK_FREQ) != 0) {
                return SENSOR_ERROR_TIM_INIT_FAILED;
            }
            init_ret = ov7690_init(&sensor);
            break;
        #endif // (OMV_OV7690_ENABLE == 1)

        #if (OMV_OV7725_ENABLE == 1)
        case OV7725_ID:
            init_ret = ov7725_init(&sensor);
            break;
        #endif // (OMV_OV7725_ENABLE == 1)

        #if (OMV_OV9650_ENABLE == 1)
        case OV9650_ID:
            init_ret = ov9650_init(&sensor);
            break;
        #endif // (OMV_OV9650_ENABLE == 1)

        #if (OMV_MT9V0XX_ENABLE == 1)
        case MT9V0X2_ID_V_1:
        case MT9V0X2_ID_V_2:
            // Force old versions to the newest.
            sensor.chip_id_w = MT9V0X2_ID;
        case MT9V0X2_ID:
        case MT9V0X4_ID:
            if (sensor_set_xclk_frequency(OMV_MT9V0XX_XCLK_FREQ) != 0) {
                return SENSOR_ERROR_TIM_INIT_FAILED;
            }
            init_ret = mt9v0xx_init(&sensor);
            break;
        #endif //(OMV_MT9V0XX_ENABLE == 1)

        #if (OMV_MT9M114_ENABLE == 1)
        case MT9M114_ID:
            if (sensor_set_xclk_frequency(OMV_MT9M114_XCLK_FREQ) != 0) {
                return SENSOR_ERROR_TIM_INIT_FAILED;
            }
            init_ret = mt9m114_init(&sensor);
            break;
        #endif //(OMV_MT9M114_ENABLE == 1)

        #if (OMV_LEPTON_ENABLE == 1)
        case LEPTON_ID:
            if (sensor_set_xclk_frequency(OMV_LEPTON_XCLK_FREQ) != 0) {
                return SENSOR_ERROR_TIM_INIT_FAILED;
            }
            init_ret = lepton_init(&sensor);
            break;
        #endif // (OMV_LEPTON_ENABLE == 1)

        #if (OMV_HM01B0_ENABLE == 1)
        case HM01B0_ID:
            if (sensor_set_xclk_frequency(OMV_HM01B0_XCLK_FREQ) != 0) {
                return SENSOR_ERROR_TIM_INIT_FAILED;
            }
            init_ret = hm01b0_init(&sensor);
            break;
        #endif //(OMV_HM01B0_ENABLE == 1)

        #if (OMV_HM0360_ENABLE == 1)
        case HM0360_ID:
            if (sensor_set_xclk_frequency(OMV_HM0360_XCLK_FREQ) != 0) {
                return SENSOR_ERROR_TIM_INIT_FAILED;
            }
            init_ret = hm0360_init(&sensor);
            break;
        #endif //(OMV_HM0360_ENABLE == 1)

        #if (OMV_GC2145_ENABLE == 1)
        case GC2145_ID:
            if (sensor_set_xclk_frequency(OMV_GC2145_XCLK_FREQ) != 0) {
                return SENSOR_ERROR_TIM_INIT_FAILED;
            }
            init_ret = gc2145_init(&sensor);
            break;
        #endif //(OMV_GC2145_ENABLE == 1)

        #if (OMV_PAG7920_ENABLE == 1)
        case PAG7920_ID:
            if (sensor_set_xclk_frequency(OMV_PAG7920_XCLK_FREQ) != 0) {
                return SENSOR_ERROR_TIM_INIT_FAILED;
            }
            init_ret = pag7920_init(&sensor);
            break;
        #endif // (OMV_PAG7920_ENABLE == 1)

        #if (OMV_PAJ6100_ENABLE == 1)
        case PAJ6100_ID:
            if (sensor_set_xclk_frequency(OMV_PAJ6100_XCLK_FREQ) != 0) {
                return SENSOR_ERROR_TIM_INIT_FAILED;
            }
            init_ret = paj6100_init(&sensor);
            break;
        #endif // (OMV_PAJ6100_ENABLE == 1)

        #if (OMV_FROGEYE2020_ENABLE == 1)
        case FROGEYE2020_ID:
            if (sensor_set_xclk_frequency(OMV_FROGEYE2020_XCLK_FREQ) != 0) {
                return SENSOR_ERROR_TIM_INIT_FAILED;
            }
            init_ret = frogeye2020_init(&sensor);
            break;
        #endif // (OMV_FROGEYE2020_ENABLE == 1)

        default:
            return SENSOR_ERROR_ISC_UNSUPPORTED;
            break;
    }

    if (init_ret != 0) {
        // Sensor init failed.
        return SENSOR_ERROR_ISC_INIT_FAILED;
    }

    return 0;
}

__weak int sensor_config(sensor_config_t config) {
    return 0;
}

__weak int sensor_get_id() {
    return sensor.chip_id_w;
}

__weak uint32_t sensor_get_xclk_frequency() {
    return SENSOR_ERROR_CTL_UNSUPPORTED;
}

__weak int sensor_set_xclk_frequency(uint32_t frequency) {
    return SENSOR_ERROR_CTL_UNSUPPORTED;
}

__weak bool sensor_is_detected() {
    return sensor.detected;
}

__weak int sensor_sleep(int enable) {
    // Disable any ongoing frame capture.
    sensor_abort(true, false);

    // Check if the control is supported.
    if (sensor.sleep == NULL) {
        return SENSOR_ERROR_CTL_UNSUPPORTED;
    }

    // Call the sensor specific function.
    if (sensor.sleep(&sensor, enable) != 0) {
        return SENSOR_ERROR_CTL_FAILED;
    }

    return 0;
}

__weak int sensor_shutdown(int enable) {
    int ret = 0;

    // Disable any ongoing frame capture.
    sensor_abort(true, false);

    #if defined(OMV_CSI_POWER_PIN)
    if (enable) {
        if (sensor.power_pol == ACTIVE_HIGH) {
            omv_gpio_write(OMV_CSI_POWER_PIN, 1);
        } else {
            omv_gpio_write(OMV_CSI_POWER_PIN, 0);
        }
    } else {
        if (sensor.power_pol == ACTIVE_HIGH) {
            omv_gpio_write(OMV_CSI_POWER_PIN, 0);
        } else {
            omv_gpio_write(OMV_CSI_POWER_PIN, 1);
        }
    }
    #endif

    mp_hal_delay_ms(10);

    return ret;
}

__weak int sensor_read_reg(uint16_t reg_addr) {
    int ret;

    // Check if the control is supported.
    if (sensor.read_reg == NULL) {
        return SENSOR_ERROR_CTL_UNSUPPORTED;
    }

    // Call the sensor specific function.
    if ((ret = sensor.read_reg(&sensor, reg_addr)) == -1) {
        return SENSOR_ERROR_IO_ERROR;
    }

    return ret;
}

__weak int sensor_write_reg(uint16_t reg_addr, uint16_t reg_data) {
    // Check if the control is supported.
    if (sensor.write_reg == NULL) {
        return SENSOR_ERROR_CTL_UNSUPPORTED;
    }

    // Call the sensor specific function.
    if (sensor.write_reg(&sensor, reg_addr, reg_data) == -1) {
        return SENSOR_ERROR_IO_ERROR;
    }

    return 0;
}

__weak int sensor_set_pixformat(pixformat_t pixformat) {
    // Check if the value has changed.
    if (sensor.pixformat == pixformat) {
        return 0;
    }

    // Some sensor drivers automatically switch to BAYER to reduce the frame size if it does not fit in RAM.
    // If the current format is BAYER (1BPP), and the target format is color and (2BPP), and the frame does not
    // fit in RAM it will just be switched back again to BAYER, so we keep the current format unchanged.
    uint32_t size = framebuffer_get_buffer_size();
    if ((sensor.pixformat == PIXFORMAT_BAYER) &&
        ((pixformat == PIXFORMAT_RGB565) || (pixformat == PIXFORMAT_YUV422)) &&
        (MAIN_FB()->u * MAIN_FB()->v * 2 > size) &&
        (MAIN_FB()->u * MAIN_FB()->v * 1 <= size)) {
        return 0;
    }

    // Cropping and transposing (and thus auto rotation) don't work in JPEG mode.
    if (((pixformat == PIXFORMAT_YUV422) && (sensor.transpose || sensor.auto_rotation)) ||
        ((pixformat == PIXFORMAT_JPEG) && (sensor_get_cropped() || sensor.transpose || sensor.auto_rotation))) {
        return SENSOR_ERROR_PIXFORMAT_UNSUPPORTED;
    }

    // Disable any ongoing frame capture.
    sensor_abort(true, false);

    // Flush previous frame.
    framebuffer_update_jpeg_buffer();

    // Check if the control is supported.
    if (sensor.set_pixformat == NULL) {
        return SENSOR_ERROR_CTL_UNSUPPORTED;
    }

    // Call the sensor specific function.
    if (sensor.set_pixformat(&sensor, pixformat) != 0) {
        return SENSOR_ERROR_CTL_FAILED;
    }

    if (!sensor.disable_delays) {
        mp_hal_delay_ms(100); // wait for the camera to settle
    }

    // Set pixel format
    sensor.pixformat = pixformat;

    // Reset pixel format to skip the first frame.
    MAIN_FB()->pixfmt = PIXFORMAT_INVALID;

    // Auto-adjust the number of frame buffers.
    sensor_set_framebuffers(-1);

    // Reconfigure the hardware if needed.
    return sensor_config(SENSOR_CONFIG_PIXFORMAT);
}

__weak int sensor_set_framesize(framesize_t framesize) {
    if (sensor.framesize == framesize) {
        // No change
        return 0;
    }

    // Disable any ongoing frame capture.
    sensor_abort(true, false);

    // Flush previous frame.
    framebuffer_update_jpeg_buffer();

    // Call the sensor specific function
    if (sensor.set_framesize == NULL) {
        return SENSOR_ERROR_CTL_UNSUPPORTED;
    }

    if (sensor.set_framesize(&sensor, framesize) != 0) {
        return SENSOR_ERROR_CTL_FAILED;
    }

    if (!sensor.disable_delays) {
        mp_hal_delay_ms(100); // wait for the camera to settle
    }

    // Set framebuffer size
    sensor.framesize = framesize;

    // Set x and y offsets.
    MAIN_FB()->x = 0;
    MAIN_FB()->y = 0;
    // Set width and height.
    MAIN_FB()->w = resolution[framesize][0];
    MAIN_FB()->h = resolution[framesize][1];
    // Set backup width and height.
    MAIN_FB()->u = resolution[framesize][0];
    MAIN_FB()->v = resolution[framesize][1];
    // Reset pixel format to skip the first frame.
    MAIN_FB()->pixfmt = PIXFORMAT_INVALID;

    // Auto-adjust the number of frame buffers.
    sensor_set_framebuffers(-1);

    // Reconfigure the hardware if needed.
    return sensor_config(SENSOR_CONFIG_FRAMESIZE);
}

__weak int sensor_set_framerate(int framerate) {
    if (sensor.framerate == framerate) {
        // No change
        return 0;
    }

    if (framerate < 0) {
        return SENSOR_ERROR_INVALID_ARGUMENT;
    }

    // If the sensor implements framerate control use it.
    if (sensor.set_framerate != NULL
        && sensor.set_framerate(&sensor, framerate) != 0) {
        return SENSOR_ERROR_CTL_FAILED;
    } else {
        // Otherwise use software framerate control.
        sensor.framerate = framerate;
    }
    return 0;
}

__weak void sensor_throttle_framerate() {
    if (!sensor.first_line) {
        sensor.first_line = true;
        uint32_t tick = mp_hal_ticks_ms();
        uint32_t framerate_ms = IM_DIV(1000, sensor.framerate);

        if (sensor.last_frame_ms_valid && ((tick - sensor.last_frame_ms) < framerate_ms)) {
            // Drop the current frame to match the requested frame rate. Note that if the frame
            // is marked to be dropped, it should not be copied to SRAM/SDRAM to save CPU time.
            sensor.drop_frame = true;
        } else if (sensor.last_frame_ms_valid) {
            sensor.last_frame_ms += framerate_ms;
        } else {
            sensor.last_frame_ms = tick;
            sensor.last_frame_ms_valid = true;
        }
    }
}

__weak bool sensor_get_cropped() {
    if (sensor.framesize != FRAMESIZE_INVALID) {
        return (MAIN_FB()->x != 0) ||
               (MAIN_FB()->y != 0) ||
               (MAIN_FB()->u != resolution[sensor.framesize][0]) ||
               (MAIN_FB()->v != resolution[sensor.framesize][1]);
    }
    return false;
}

__weak uint32_t sensor_get_src_bpp() {
    if (sensor.raw_output) {
        return 1;
    }
    switch (sensor.pixformat) {
        case PIXFORMAT_BAYER:
        case PIXFORMAT_JPEG:
            return 1;
        case PIXFORMAT_RGB565:
        case PIXFORMAT_YUV422:
            return 2;
        case PIXFORMAT_GRAYSCALE:
            return sensor.mono_bpp;
        default:
            return 0;
    }
}

__weak uint32_t sensor_get_dst_bpp() {
    switch (sensor.pixformat) {
        case PIXFORMAT_GRAYSCALE:
        case PIXFORMAT_BAYER:
            return 1;
        case PIXFORMAT_RGB565:
        case PIXFORMAT_YUV422:
            return 2;
        default:
            return 0;
    }
}

__weak int sensor_set_windowing(int x, int y, int w, int h) {
    // Check if the value has changed.
    if ((MAIN_FB()->x == x) && (MAIN_FB()->y == y) &&
        (MAIN_FB()->u == w) && (MAIN_FB()->v == h)) {
        return 0;
    }

    if (sensor.pixformat == PIXFORMAT_JPEG) {
        return SENSOR_ERROR_PIXFORMAT_UNSUPPORTED;
    }

    // Disable any ongoing frame capture.
    sensor_abort(true, false);

    // Flush previous frame.
    framebuffer_update_jpeg_buffer();

    // Set x and y offsets.
    MAIN_FB()->x = x;
    MAIN_FB()->y = y;
    // Set width and height.
    MAIN_FB()->w = w;
    MAIN_FB()->h = h;
    // Set backup width and height.
    MAIN_FB()->u = w;
    MAIN_FB()->v = h;
    // Reset pixel format to skip the first frame.
    MAIN_FB()->pixfmt = PIXFORMAT_INVALID;

    // Auto-adjust the number of frame buffers.
    sensor_set_framebuffers(-1);

    // Reconfigure the hardware if needed.
    return sensor_config(SENSOR_CONFIG_WINDOWING);
}

__weak int sensor_set_contrast(int level) {
    // Check if the control is supported.
    if (sensor.set_contrast == NULL) {
        return SENSOR_ERROR_CTL_UNSUPPORTED;
    }

    // Call the sensor specific function.
    if (sensor.set_contrast(&sensor, level) != 0) {
        return SENSOR_ERROR_CTL_FAILED;
    }

    return 0;
}

__weak int sensor_set_brightness(int level) {
    // Check if the control is supported.
    if (sensor.set_brightness == NULL) {
        return SENSOR_ERROR_CTL_UNSUPPORTED;
    }

    // Call the sensor specific function.
    if (sensor.set_brightness(&sensor, level) != 0) {
        return SENSOR_ERROR_CTL_FAILED;
    }

    return 0;
}

__weak int sensor_set_saturation(int level) {
    // Check if the control is supported.
    if (sensor.set_saturation == NULL) {
        return SENSOR_ERROR_CTL_UNSUPPORTED;
    }

    // Call the sensor specific function.
    if (sensor.set_saturation(&sensor, level) != 0) {
        return SENSOR_ERROR_CTL_FAILED;
    }

    return 0;
}

__weak int sensor_set_gainceiling(gainceiling_t gainceiling) {
    // Check if the value has changed.
    if (sensor.gainceiling == gainceiling) {
        return 0;
    }

    // Check if the control is supported.
    if (sensor.set_gainceiling == NULL) {
        return SENSOR_ERROR_CTL_UNSUPPORTED;
    }

    // Call the sensor specific function.
    if (sensor.set_gainceiling(&sensor, gainceiling) != 0) {
        return SENSOR_ERROR_CTL_FAILED;
    }

    // Set the new control value.
    sensor.gainceiling = gainceiling;

    return 0;
}

__weak int sensor_set_quality(int qs) {
    // Check if the control is supported.
    if (sensor.set_quality == NULL) {
        return SENSOR_ERROR_CTL_UNSUPPORTED;
    }

    // Call the sensor specific function.
    if (sensor.set_quality(&sensor, qs) != 0) {
        return SENSOR_ERROR_CTL_FAILED;
    }

    return 0;
}

__weak int sensor_set_colorbar(int enable) {
    // Check if the control is supported.
    if (sensor.set_colorbar == NULL) {
        return SENSOR_ERROR_CTL_UNSUPPORTED;
    }

    // Call the sensor specific function.
    if (sensor.set_colorbar(&sensor, enable) != 0) {
        return SENSOR_ERROR_CTL_FAILED;
    }

    return 0;
}

__weak int sensor_set_auto_gain(int enable, float gain_db, float gain_db_ceiling) {
    // Check if the control is supported.
    if (sensor.set_auto_gain == NULL) {
        return SENSOR_ERROR_CTL_UNSUPPORTED;
    }

    // Call the sensor specific function.
    if (sensor.set_auto_gain(&sensor, enable, gain_db, gain_db_ceiling) != 0) {
        return SENSOR_ERROR_CTL_FAILED;
    }

    return 0;
}

__weak int sensor_get_gain_db(float *gain_db) {
    // Check if the control is supported.
    if (sensor.get_gain_db == NULL) {
        return SENSOR_ERROR_CTL_UNSUPPORTED;
    }

    // Call the sensor specific function.
    if (sensor.get_gain_db(&sensor, gain_db) != 0) {
        return SENSOR_ERROR_CTL_FAILED;
    }

    return 0;
}

__weak int sensor_set_auto_exposure(int enable, int exposure_us) {
    // Check if the control is supported.
    if (sensor.set_auto_exposure == NULL) {
        return SENSOR_ERROR_CTL_UNSUPPORTED;
    }

    // Call the sensor specific function.
    if (sensor.set_auto_exposure(&sensor, enable, exposure_us) != 0) {
        return SENSOR_ERROR_CTL_FAILED;
    }

    return 0;
}

__weak int sensor_get_exposure_us(int *exposure_us) {
    // Check if the control is supported.
    if (sensor.get_exposure_us == NULL) {
        return SENSOR_ERROR_CTL_UNSUPPORTED;
    }

    // Call the sensor specific function.
    if (sensor.get_exposure_us(&sensor, exposure_us) != 0) {
        return SENSOR_ERROR_CTL_FAILED;
    }

    return 0;
}

__weak int sensor_set_auto_whitebal(int enable, float r_gain_db, float g_gain_db, float b_gain_db) {
    // Check if the control is supported.
    if (sensor.set_auto_whitebal == NULL) {
        return SENSOR_ERROR_CTL_UNSUPPORTED;
    }

    // Call the sensor specific function.
    if (sensor.set_auto_whitebal(&sensor, enable, r_gain_db, g_gain_db, b_gain_db) != 0) {
        return SENSOR_ERROR_CTL_FAILED;
    }

    return 0;
}

__weak int sensor_get_rgb_gain_db(float *r_gain_db, float *g_gain_db, float *b_gain_db) {
    // Check if the control is supported.
    if (sensor.get_rgb_gain_db == NULL) {
        return SENSOR_ERROR_CTL_UNSUPPORTED;
    }

    // Call the sensor specific function.
    if (sensor.get_rgb_gain_db(&sensor, r_gain_db, g_gain_db, b_gain_db) != 0) {
        return SENSOR_ERROR_CTL_FAILED;
    }

    return 0;
}

__weak int sensor_set_auto_blc(int enable, int *regs) {
    // Check if the control is supported.
    if (sensor.set_auto_blc == NULL) {
        return SENSOR_ERROR_CTL_UNSUPPORTED;
    }

    // Call the sensor specific function.
    if (sensor.set_auto_blc(&sensor, enable, regs) != 0) {
        return SENSOR_ERROR_CTL_FAILED;
    }

    return 0;
}

__weak int sensor_get_blc_regs(int *regs) {
    // Check if the control is supported.
    if (sensor.get_blc_regs == NULL) {
        return SENSOR_ERROR_CTL_UNSUPPORTED;
    }

    // Call the sensor specific function.
    if (sensor.get_blc_regs(&sensor, regs) != 0) {
        return SENSOR_ERROR_CTL_FAILED;
    }

    return 0;
}

__weak int sensor_set_hmirror(int enable) {
    // Check if the value has changed.
    if (sensor.hmirror == ((bool) enable)) {
        return 0;
    }

    // Disable any ongoing frame capture.
    sensor_abort(true, false);

    // Check if the control is supported.
    if (sensor.set_hmirror == NULL) {
        return SENSOR_ERROR_CTL_UNSUPPORTED;
    }

    // Call the sensor specific function.
    if (sensor.set_hmirror(&sensor, enable) != 0) {
        return SENSOR_ERROR_CTL_FAILED;
    }

    // Set the new control value.
    sensor.hmirror = enable;

    // Wait for the camera to settle
    if (!sensor.disable_delays) {
        mp_hal_delay_ms(100);
    }

    return 0;
}

__weak bool sensor_get_hmirror() {
    return sensor.hmirror;
}

__weak int sensor_set_vflip(int enable) {
    // Check if the value has changed.
    if (sensor.vflip == ((bool) enable)) {
        return 0;
    }

    // Disable any ongoing frame capture.
    sensor_abort(true, false);

    // Check if the control is supported.
    if (sensor.set_vflip == NULL) {
        return SENSOR_ERROR_CTL_UNSUPPORTED;
    }

    // Call the sensor specific function.
    if (sensor.set_vflip(&sensor, enable) != 0) {
        return SENSOR_ERROR_CTL_FAILED;
    }

    // Set the new control value.
    sensor.vflip = enable;

    // Wait for the camera to settle
    if (!sensor.disable_delays) {
        mp_hal_delay_ms(100);
    }

    return 0;
}

__weak bool sensor_get_vflip() {
    return sensor.vflip;
}

__weak int sensor_set_transpose(bool enable) {
    // Check if the value has changed.
    if (sensor.transpose == enable) {
        return 0;
    }

    // Disable any ongoing frame capture.
    sensor_abort(true, false);

    if ((sensor.pixformat == PIXFORMAT_YUV422) || (sensor.pixformat == PIXFORMAT_JPEG)) {
        return SENSOR_ERROR_PIXFORMAT_UNSUPPORTED;
    }

    // Set the new control value.
    sensor.transpose = enable;

    return 0;
}

__weak bool sensor_get_transpose() {
    return sensor.transpose;
}

__weak int sensor_set_auto_rotation(bool enable) {
    // Check if the value has changed.
    if (sensor.auto_rotation == enable) {
        return 0;
    }

    // Disable any ongoing frame capture.
    sensor_abort(true, false);

    // Operation not supported on JPEG images.
    if ((sensor.pixformat == PIXFORMAT_YUV422) || (sensor.pixformat == PIXFORMAT_JPEG)) {
        return SENSOR_ERROR_PIXFORMAT_UNSUPPORTED;
    }

    // Set the new control value.
    sensor.auto_rotation = enable;
    return 0;
}

__weak bool sensor_get_auto_rotation() {
    return sensor.auto_rotation;
}

__weak int sensor_set_framebuffers(int count) {
    // Disable any ongoing frame capture.
    sensor_abort(true, false);

    // Flush previous frame.
    framebuffer_update_jpeg_buffer();

    if (sensor.pixformat == PIXFORMAT_INVALID) {
        return SENSOR_ERROR_INVALID_PIXFORMAT;
    }

    if (sensor.framesize == FRAMESIZE_INVALID) {
        return SENSOR_ERROR_INVALID_FRAMESIZE;
    }

    uint32_t bpp = IM_MAX(sensor_get_src_bpp(), sensor_get_dst_bpp());
    #if OMV_CSI_HW_CROP_ENABLE
    // If hardware cropping is supported, use window size.
    MAIN_FB()->frame_size = MAIN_FB()->u * MAIN_FB()->v * bpp;
    #else
    // Otherwise, use the real frame size.
    MAIN_FB()->frame_size = resolution[sensor.framesize][0] * resolution[sensor.framesize][1] * bpp;
    #endif
    return framebuffer_set_buffers(count);
}

__weak int sensor_set_special_effect(sde_t sde) {
    // Check if the value has changed.
    if (sensor.sde == sde) {
        return 0;
    }

    // Check if the control is supported.
    if (sensor.set_special_effect == NULL) {
        return SENSOR_ERROR_CTL_UNSUPPORTED;
    }

    // Call the sensor specific function.
    if (sensor.set_special_effect(&sensor, sde) != 0) {
        return SENSOR_ERROR_CTL_FAILED;
    }

    // Set the new control value.
    sensor.sde = sde;

    return 0;
}

__weak int sensor_set_lens_correction(int enable, int radi, int coef) {
    // Check if the control is supported.
    if (sensor.set_lens_correction == NULL) {
        return SENSOR_ERROR_CTL_UNSUPPORTED;
    }

    // Call the sensor specific function.
    if (sensor.set_lens_correction(&sensor, enable, radi, coef) != 0) {
        return SENSOR_ERROR_CTL_FAILED;
    }

    return 0;
}

__weak int sensor_ioctl(int request, ... /* arg */) {
    // Disable any ongoing frame capture.
    sensor_abort(true, false);

    // Check if the control is supported.
    if (sensor.ioctl == NULL) {
        return SENSOR_ERROR_CTL_UNSUPPORTED;
    }

    va_list ap;
    va_start(ap, request);
    // Call the sensor specific function.
    int ret = sensor.ioctl(&sensor, request, ap);
    va_end(ap);

    return ((ret != 0) ? SENSOR_ERROR_CTL_FAILED : 0);
}

__weak int sensor_set_vsync_callback(vsync_cb_t vsync_cb) {
    sensor.vsync_callback = vsync_cb;
    return 0;
}

__weak int sensor_set_frame_callback(frame_cb_t vsync_cb) {
    sensor.frame_callback = vsync_cb;
    return 0;
}

__weak int sensor_set_color_palette(const uint16_t *color_palette) {
    sensor.color_palette = color_palette;
    return 0;
}

__weak const uint16_t *sensor_get_color_palette() {
    return sensor.color_palette;
}

__weak int sensor_check_framebuffer_size() {
    uint32_t bpp = sensor_get_dst_bpp();
    uint32_t size = framebuffer_get_buffer_size();
    return (((MAIN_FB()->u * MAIN_FB()->v * bpp) <= size) ? 0 : -1);
}

__weak int sensor_auto_crop_framebuffer() {
    uint32_t bpp = sensor_get_dst_bpp();
    uint32_t size = framebuffer_get_buffer_size();

    // If the pixformat is NULL/JPEG there we can't do anything to check if it fits before hand.
    if (!bpp) {
        return 0;
    }

    // MAIN_FB() fits, we are done.
    if ((MAIN_FB()->u * MAIN_FB()->v * bpp) <= size) {
        return 0;
    }

    if ((sensor.pixformat == PIXFORMAT_RGB565) || (sensor.pixformat == PIXFORMAT_YUV422)) {
        // Switch to bayer for the quick 2x savings.
        sensor_set_pixformat(PIXFORMAT_BAYER);
        bpp = 1;

        // MAIN_FB() fits, we are done (bpp is 1).
        if ((MAIN_FB()->u * MAIN_FB()->v) <= size) {
            return 0;
        }
    }

    int window_w = MAIN_FB()->u;
    int window_h = MAIN_FB()->v;

    // We need to shrink the frame buffer. We can do this by cropping. So, we will subtract columns
    // and rows from the frame buffer until it fits within the frame buffer.
    int max = IM_MAX(window_w, window_h);
    int min = IM_MIN(window_w, window_h);
    float aspect_ratio = max / ((float) min);
    float r = aspect_ratio, best_r = r;
    int c = 1, best_c = c;
    float best_err = FLT_MAX;

    // Find the width/height ratio that's within 1% of the aspect ratio with a loop limit.
    for (int i = 100; i; i--) {
        float err = fast_fabsf(r - fast_roundf(r));

        if (err <= best_err) {
            best_err = err;
            best_r = r;
            best_c = c;
        }

        if (best_err <= 0.01f) {
            break;
        }

        r += aspect_ratio;
        c += 1;
    }

    // Select the larger geometry to map the aspect ratio to.
    int u_sub, v_sub;

    if (window_w > window_h) {
        u_sub = fast_roundf(best_r);
        v_sub = best_c;
    } else {
        u_sub = best_c;
        v_sub = fast_roundf(best_r);
    }

    // Crop the frame buffer while keeping the aspect ratio and keeping the width/height even.
    while (((MAIN_FB()->u * MAIN_FB()->v * bpp) > size) || (MAIN_FB()->u % 2) || (MAIN_FB()->v % 2)) {
        MAIN_FB()->u -= u_sub;
        MAIN_FB()->v -= v_sub;
    }

    // Center the new window using the previous offset and keep the offset even.
    MAIN_FB()->x += (window_w - MAIN_FB()->u) / 2;
    MAIN_FB()->y += (window_h - MAIN_FB()->v) / 2;

    if (MAIN_FB()->x % 2) {
        MAIN_FB()->x -= 1;
    }
    if (MAIN_FB()->y % 2) {
        MAIN_FB()->y -= 1;
    }

    // Auto-adjust the number of frame buffers.
    sensor_set_framebuffers(-1);
    return 0;
}

#define copy_transposed_line(dstp, srcp)                   \
    for (int i = MAIN_FB()->u, h = MAIN_FB()->v; i; i--) { \
        *dstp = *srcp++;                                   \
        dstp += h;                                         \
    }

#define copy_transposed_line_rev16(dstp, srcp)             \
    for (int i = MAIN_FB()->u, h = MAIN_FB()->v; i; i--) { \
        *dstp = __REV16(*srcp++);                          \
        dstp += h;                                         \
    }

__weak int sensor_copy_line(void *dma, uint8_t *src, uint8_t *dst) {
    uint16_t *src16 = (uint16_t *) src;
    uint16_t *dst16 = (uint16_t *) dst;
    #if OMV_CSI_DMA_MEMCPY_ENABLE
    extern int sensor_dma_memcpy(void *dma, void *dst, void *src, int bpp, bool transposed);
    #endif

    switch (sensor.pixformat) {
        case PIXFORMAT_BAYER:
            #if OMV_CSI_DMA_MEMCPY_ENABLE
            if (!sensor_dma_memcpy(dma, dst, src, sizeof(uint8_t), sensor.transpose)) {
                break;
            }
            #endif
            if (!sensor.transpose) {
                unaligned_memcpy(dst, src, MAIN_FB()->u);
            } else {
                copy_transposed_line(dst, src);
            }
            break;
        case PIXFORMAT_GRAYSCALE:
            #if OMV_CSI_DMA_MEMCPY_ENABLE
            if (!sensor_dma_memcpy(dma, dst, src, sizeof(uint8_t), sensor.transpose)) {
                break;
            }
            #endif
            if (sensor.mono_bpp == 1) {
                // 1BPP GRAYSCALE.
                if (!sensor.transpose) {
                    unaligned_memcpy(dst, src, MAIN_FB()->u);
                } else {
                    copy_transposed_line(dst, src);
                }
            } else {
                // Extract Y channel from YUV.
                if (!sensor.transpose) {
                    unaligned_2_to_1_memcpy(dst, src16, MAIN_FB()->u);
                } else {
                    copy_transposed_line(dst, src16);
                }
            }
            break;
        case PIXFORMAT_RGB565:
        case PIXFORMAT_YUV422:
            #if OMV_CSI_DMA_MEMCPY_ENABLE
            if (!sensor_dma_memcpy(dma, dst16, src16, sizeof(uint16_t), sensor.transpose)) {
                break;
            }
            #endif
            if (0) {
            #if !OMV_CSI_HW_SWAP_ENABLE
            } else if ((sensor.pixformat == PIXFORMAT_RGB565 && sensor.rgb_swap) ||
                       (sensor.pixformat == PIXFORMAT_YUV422 && sensor.yuv_swap)) {
                if (!sensor.transpose) {
                    unaligned_memcpy_rev16(dst16, src16, MAIN_FB()->u);
                } else {
                    copy_transposed_line_rev16(dst16, src16);
                }
            #endif
            } else {
                if (!sensor.transpose) {
                    unaligned_memcpy(dst16, src16, MAIN_FB()->u * sizeof(uint16_t));
                } else {
                    copy_transposed_line(dst16, src16);
                }
            }
            break;
        default:
            break;
    }
    return 0;
}

__weak int sensor_snapshot(sensor_t *sensor, image_t *image, uint32_t flags) {
    return -1;
}

const char *sensor_strerror(int error) {
    static const char *sensor_errors[] = {
        "No error.",
        "Sensor control failed.",
        "The requested operation is not supported by the image sensor.",
        "Failed to detect the image sensor or image sensor is detached.",
        "The detected image sensor is not supported.",
        "Failed to initialize the image sensor.",
        "Failed to initialize the external clock.",
        "Failed to initialize the CSI DMA.",
        "Failed to initialize the CSI interface.",
        "An low level I/O error has occurred.",
        "Frame capture has failed.",
        "Frame capture has timed out.",
        "Frame size is not supported or is not set.",
        "Pixel format is not supported or is not set.",
        "Window is not supported or is not set.",
        "Frame rate is not supported or is not set.",
        "An invalid argument is used.",
        "The requested operation is not supported on the current pixel format.",
        "Frame buffer error.",
        "Frame buffer overflow, try reducing the frame size.",
        "JPEG frame buffer overflow.",
    };

    // Sensor errors are negative.
    error = ((error < 0) ? (error * -1) : error);

    if (error > (sizeof(sensor_errors) / sizeof(sensor_errors[0]))) {
        return "Unknown error.";
    } else {
        return sensor_errors[error];
    }
}
#endif //MICROPY_PY_SENSOR
