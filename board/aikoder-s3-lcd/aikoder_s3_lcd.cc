#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <esp_sleep.h>

#include "alert_monitor.h"
#include "application.h"
#include "mcp_server.h"
#include "button.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "display/lcd_display.h"
#include "esp_lcd_jd9853.h"
#include "power_manager.h"
#include "power_save_timer.h"
#include "robot_face_display.h"
#include "system_reset.h"
#include "wifi_board.h"

#define TAG "AiKoderS3Lcd"

class AiKoderS3LcdBoard : public WifiBoard {
   private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Button power_button_;
    LcdDisplay* display_ = nullptr;
    PowerManager* power_manager_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Es8311AudioCodec* audio_codec_ = nullptr;

    void InitializePowerControl() {
        // Self-latch: keep the board powered by driving this pin HIGH
        // immediately after boot. Driving it LOW cuts power instantly.
        // GPIO40 is not in the RTC domain on ESP32-S3, so plain GPIO is used.
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = (1ULL << POWER_CONTROL_PIN);
        gpio_config(&io_conf);
        gpio_set_level(POWER_CONTROL_PIN, 1);
    }

    void InitializePowerManager() {
        power_manager_ = new PowerManager(BATTERY_CHARGING_DETECT_PIN);
    }

    void InitializePowerSaveTimer() {
        // Nunca desliga sozinho por inatividade (-1 desativa o shutdown) -
        // só entra em modo de espera (tela com brilho reduzido) após 60s.
        power_save_timer_ = new PowerSaveTimer(-1, 60, -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            // Não apaga totalmente - só reduz, pra dar pra ver que ainda
            // está ligado sem precisar apertar nada.
            GetBacklight()->SetBrightness(20);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() { Shutdown(); });
        power_save_timer_->SetEnabled(true);
    }

    void Shutdown() {
        ESP_LOGI(TAG, "Shutting down");
        if (panel_) {
            esp_lcd_panel_disp_on_off(panel_, false);
        }
        gpio_set_level(POWER_CONTROL_PIN, 0);
        esp_deep_sleep_start();
    }

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeAudioCodec() {
        audio_codec_ = new Es8311AudioCodec(
            i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR, true);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
        buscfg.miso_io_num = DISPLAY_SPI_MISO_PIN;
        buscfg.sclk_io_num = DISPLAY_SPI_SCLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeJd9853Display() {
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_SPI_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_SPI_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        ESP_LOGD(TAG, "Install JD9853 panel driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_SPI_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_jd9853(panel_io, &panel_config, &panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, DISPLAY_INVERT_COLOR));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_, DISPLAY_SWAP_XY));
        // Landscape (swapped) orientation leaves a black strip on the right
        // without this - the panel's addressable column window needs
        // shifting to line up with our 296-wide logical buffer.
        ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_, 24, 0));

        auto face_display = new RobotFaceDisplay(panel_io, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                     DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        face_display->SetAudioCodecForSync(audio_codec_);
        display_ = face_display;
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        boot_button_.OnLongPress([this]() {
            power_save_timer_->WakeUp();
            auto& app = Application::GetInstance();
            app.SetDeviceState(kDeviceStateWifiConfiguring);
            EnterWifiConfigMode();
        });

        // Клік живлення — показати/сховати мапу тривог; подвійний клік — тестова тривога
        power_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            AlertMonitor::GetInstance().ToggleMap();
        });
        power_button_.OnDoubleClick([this]() {
            power_save_timer_->WakeUp();
            AlertMonitor::GetInstance().StartTest();
        });
        power_button_.OnLongPress([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() != kDeviceStateStarting) {
                Shutdown();
            }
        });
    }

    void InitializeAlerts() {
        AlertMonitor::GetInstance().Start(display_, [this]() { power_save_timer_->WakeUp(); });

        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.air_alert.get_status",
            "Повітряні тривоги в Україні зараз: чи є тривога в домашньому регіоні користувача, як довго триває, "
            "та в яких областях тривога. Use whenever the user asks about air raid alerts / тривога / сирена.",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                return AlertMonitor::GetInstance().StatusText();
            });
        mcp.AddTool("self.air_alert.show_map",
            "Show or hide the air-alert map of Ukraine on the device screen.",
            PropertyList({Property("visible", kPropertyTypeBoolean)}),
            [](const PropertyList& p) -> ReturnValue {
                auto& m = AlertMonitor::GetInstance();
                if (p["visible"].value<bool>() != m.IsMapVisible()) m.ToggleMap();
                return true;
            });
        mcp.AddTool("self.air_alert.set_sound",
            "Enable or disable the siren / all-clear sounds for air alerts.",
            PropertyList({Property("enabled", kPropertyTypeBoolean)}),
            [](const PropertyList& p) -> ReturnValue {
                AlertMonitor::GetInstance().SetSoundEnabled(p["enabled"].value<bool>());
                return true;
            });
        mcp.AddTool("self.air_alert.set_home_region",
            "Set the user's home region for air alerts. Use an id: kyiv_city (м. Київ), kyiv (Київська обл.), "
            "vol, riv, zht, chn, sum, lvi, ter, khm, vin, chk, pol, khr, lug, zak, ifr, chv, kir, dnp, don, ode, myk, zap, khe, crimea. "
            "A Ukrainian region name also works.",
            PropertyList({Property("region", kPropertyTypeString)}),
            [](const PropertyList& p) -> ReturnValue {
                return AlertMonitor::GetInstance().SetHomeRegion(p["region"].value<std::string>());
            });
        mcp.AddTool("self.air_alert.test",
            "Play a test air-alert (siren + red map for ~25 s, then all-clear sound).",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                AlertMonitor::GetInstance().StartTest();
                return true;
            });
    }

   public:
    AiKoderS3LcdBoard()
        : boot_button_(BOOT_BUTTON_GPIO), power_button_(POWER_BUTTON_GPIO) {
        InitializePowerControl();
        InitializePowerManager();
        InitializePowerSaveTimer();
        InitializeI2c();
        InitializeAudioCodec();
        InitializeSpi();
        InitializeJd9853Display();
        InitializeButtons();
        InitializeAlerts();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override { return audio_codec_; }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        level = power_manager_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(AiKoderS3LcdBoard);
