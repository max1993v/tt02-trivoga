#ifndef _ALERT_MONITOR_H_
#define _ALERT_MONITOR_H_

// Повітряні тривоги для TT-02 (AiKoder S3 LCD).
// Джерело даних — ubilling.net.ua/aerialalerts (без токена), як у лаунчері «Вітальня».
// Фоновий опит кожні 20 с; тривога в домашньому регіоні → сирена + екран з мапою,
// відбій → звук відбою. Кнопка живлення: клік — показати/сховати мапу,
// подвійний клік — тестова тривога.

#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>

// має збігатися з UA_REGION_COUNT у ua_map.h (перевіряється static_assert)
#ifndef UA_REGION_COUNT
#define UA_REGION_COUNT 25
#endif

class Display;

class AlertMonitor {
public:
    static AlertMonitor& GetInstance();

    void Start(Display* display, std::function<void()> wake_cb);

    enum { kModeMap = 0, kModeClock = 1, kModeFace = 2, kModeAuto = 3 };   // auto: мапа ↔ годинник кожні 10 с
    void CycleMode();          // клік кнопки живлення: мапа → годинник → обличчя
    void SetMode(int mode, bool save = true);
    bool SetModeByName(const std::string& name);
    const char* ModeName() const;
    void StartTest();          // подвійний клік: тестова тривога ~25 с

    // для голосового асистента (MCP)
    std::string StatusText();
    std::string TimeText();
    void SetSoundEnabled(bool on);
    bool SoundEnabled() const { return sound_; }
    bool SetHomeRegion(const std::string& id);
    std::string HomeRegion() const { return home_; }

private:
    AlertMonitor() = default;

    enum class Header { Calm, Alarm, AllClear, NoData, Test };

    static void TaskEntry(void* arg);
    void Run();
    bool Fetch(bool alerts[UA_REGION_COUNT + 1], std::string& src);
    void Apply(const bool alerts[UA_REGION_COUNT + 1], const std::string& src);
    void OnHomeAlarmStart(bool play_siren);
    void OnHomeAlarmEnd();

    // UI (усе під DisplayLockGuard)
    void EnsureUi();
    void RenderMap();
    void UpdateTexts();
    void Show(int what);
    int Target();
    void ApplyTarget();
    void UpdateClock();
    static void UiTaskEntry(void* arg);
    void UiLoop();

    int HomeIndex() const;     // індекс у бітмапі (1..N) або 0 для kyiv_city
    bool HomeAlert() const;
    bool OblastAlert() const;
    std::string HomeName() const;
    int ActiveCount() const;

    Display* display_ = nullptr;
    std::function<void()> wake_cb_;
    std::mutex mutex_;

    bool alerts_[UA_REGION_COUNT + 1] = {};   // [0] = м. Київ, [1..N] = області
    bool have_data_ = false;
    int64_t last_ok_us_ = 0;
    int64_t alarm_since_us_ = 0;
    int64_t allclear_until_us_ = 0;
    std::string src_;
    std::string home_ = "kyiv_city";
    bool sound_ = true;
    std::atomic<bool> visible_{false};
    std::atomic<int> mode_{kModeAuto};
    int shown_ = -1;
    std::atomic<int64_t> force_map_until_us_{0};
    bool city_seen_ = false;
    std::atomic<int64_t> test_until_us_{0};
    int src_idx_ = 0;
    bool use_http_ = false;

    // LVGL
    lv_obj_t* root_ = nullptr;
    lv_obj_t* header_ = nullptr;
    lv_obj_t* title_ = nullptr;
    lv_obj_t* canvas_ = nullptr;
    lv_obj_t* kyiv_dot_ = nullptr;
    lv_obj_t* foot_left_ = nullptr;
    lv_obj_t* foot_right_ = nullptr;
    lv_obj_t* clock_root_ = nullptr;
    lv_obj_t* clk_time_ = nullptr;
    lv_obj_t* clk_date_ = nullptr;
    lv_obj_t* clk_pill_ = nullptr;
    lv_obj_t* clk_status_ = nullptr;
    lv_obj_t* clk_foot_ = nullptr;
    uint16_t* canvas_buf_ = nullptr;
};

#endif
