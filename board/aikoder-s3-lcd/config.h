#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_INPUT_REFERENCE true

// Audio - ES8311 codec (confirmed by manufacturer)
#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_4
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_6
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_8
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_7
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_5

#define AUDIO_CODEC_PA_PIN        GPIO_NUM_9
#define AUDIO_CODEC_I2C_SDA_PIN   GPIO_NUM_2
#define AUDIO_CODEC_I2C_SCL_PIN   GPIO_NUM_1
#define AUDIO_CODEC_ES8311_ADDR   ES8311_CODEC_DEFAULT_ADDR

// Buttons
#define BUILTIN_LED_GPIO         GPIO_NUM_NC
#define BOOT_BUTTON_GPIO         GPIO_NUM_0
#define VOLUME_UP_BUTTON_GPIO    GPIO_NUM_NC
#define VOLUME_DOWN_BUTTON_GPIO  GPIO_NUM_NC
#define POWER_BUTTON_GPIO        GPIO_NUM_48

// Power hold (self-latch): MCU must drive this HIGH right after boot to
// keep the board powered; driving it LOW cuts power immediately.
#define POWER_CONTROL_PIN        GPIO_NUM_40

// Display - JD9853 (SPI), landscape: device physically turned 90° clockwise
// from its portrait default, so the buttons end up on top. This mirror
// combo gives the correct (non-mirrored) orientation; the leftover black
// strip on the right is compensated with set_gap in InitializeJd9853Display().
#define DISPLAY_WIDTH  296
#define DISPLAY_HEIGHT 240

#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y true
#define DISPLAY_SWAP_XY  true

#define DISPLAY_OFFSET_X 0
#define DISPLAY_OFFSET_Y 0

#define DISPLAY_BACKLIGHT_PIN          GPIO_NUM_13
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

#define DISPLAY_SPI_CS_PIN   GPIO_NUM_21
#define DISPLAY_SPI_DC_PIN   GPIO_NUM_16
#define DISPLAY_SPI_RST_PIN  GPIO_NUM_15
#define DISPLAY_SPI_MOSI_PIN GPIO_NUM_17
#define DISPLAY_SPI_MISO_PIN GPIO_NUM_NC
#define DISPLAY_SPI_SCLK_PIN GPIO_NUM_18

#define DISPLAY_SPI_MODE      0
#define DISPLAY_SPI_SCLK_HZ   (40 * 1000 * 1000)
#define DISPLAY_RGB_ORDER     LCD_RGB_ELEMENT_ORDER_RGB
#define DISPLAY_INVERT_COLOR  true

// Battery / charging (digital status pins on the charger IC)
#define BATTERY_LEVEL_DETECT_PIN    GPIO_NUM_10
#define BATTERY_CHARGING_DETECT_PIN GPIO_NUM_39

// Headphone detect
#define HEADPHONE_DETECT_PIN GPIO_NUM_3

#endif // _BOARD_CONFIG_H_
