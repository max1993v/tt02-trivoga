#include "ua_map.h"
#include "alert_monitor.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <cstring>
#include <ctime>

#include "alert_sounds.h"
#include "radio_player.h"
#include "application.h"
#include "board.h"
#include "display/display.h"
#include "settings.h"

#define TAG "Alerts"

LV_FONT_DECLARE(font_ua_bold);
LV_FONT_DECLARE(font_ua_med);
LV_FONT_DECLARE(font_ua_date);
LV_FONT_DECLARE(font_ua_clock);

static_assert(UA_REGION_COUNT == 25, "region count mismatch");

namespace {

constexpr int kPollSeconds = 20;
constexpr int kHeaderH = UA_MAP_Y;
constexpr int64_t kStaleUs = 150LL * 1000 * 1000;
constexpr int64_t kAllClearShowUs = 20LL * 1000 * 1000;
constexpr int64_t kTestUs = 25LL * 1000 * 1000;
constexpr int64_t kRotateUs = 10LL * 1000 * 1000;
constexpr int64_t kForceMapUs = 90LL * 1000 * 1000;

// повні назви (у тому ж порядку, що kUaRegionIds)
struct RegionName { const char* id; const char* name; };
const RegionName kNames[] = {
    {"kyiv_city", "м. Київ"},
    {"vol", "Волинська обл."}, {"riv", "Рівненська обл."}, {"zht", "Житомирська обл."},
    {"kyiv", "Київська обл."}, {"chn", "Чернігівська обл."}, {"sum", "Сумська обл."},
    {"lvi", "Львівська обл."}, {"ter", "Тернопільська обл."}, {"khm", "Хмельницька обл."},
    {"vin", "Вінницька обл."}, {"chk", "Черкаська обл."}, {"pol", "Полтавська обл."},
    {"khr", "Харківська обл."}, {"lug", "Луганська обл."}, {"zak", "Закарпатська обл."},
    {"ifr", "Івано-Франківська обл."}, {"chv", "Чернівецька обл."}, {"kir", "Кіровоградська обл."},
    {"dnp", "Дніпропетровська обл."}, {"don", "Донецька обл."}, {"ode", "Одеська обл."},
    {"myk", "Миколаївська обл."}, {"zap", "Запорізька обл."}, {"khe", "Херсонська обл."},
    {"crimea", "АР Крим"},
};

// основи назв (як STEMS у «Вітальні»): порядок важливий — «київськ» раніше за «київ»
struct Stem { const char* stem; const char* id; };
const Stem kStems[] = {
    {"київськ", "kyiv"}, {"київ", "kyiv_city"}, {"крим", "crimea"}, {"севастоп", "crimea"},
    {"волин", "vol"}, {"вінниц", "vin"}, {"дніпро", "dnp"}, {"донец", "don"},
    {"житомир", "zht"}, {"закарпат", "zak"}, {"запоріз", "zap"}, {"франків", "ifr"},
    {"кіровоград", "kir"}, {"кропивниц", "kir"}, {"луган", "lug"}, {"львів", "lvi"},
    {"микола", "myk"}, {"одес", "ode"}, {"полтав", "pol"}, {"рівнен", "riv"},
    {"сумськ", "sum"}, {"терноп", "ter"}, {"харків", "khr"}, {"херсон", "khe"},
    {"хмельниц", "khm"}, {"черкас", "chk"}, {"чернівец", "chv"}, {"чернігів", "chn"},
};

const char* kUbSources[] = {"", "aiu", "ual", "jaam", "skog", "klimenko"};
constexpr int kUbSourceCount = sizeof(kUbSources) / sizeof(kUbSources[0]);

// нижній регістр для UTF-8 кирилиці (укр. + рос. літери) та ASCII
std::string Lower(const char* s) {
    std::string out;
    const auto* p = reinterpret_cast<const unsigned char*>(s);
    while (*p) {
        unsigned char c = *p;
        if (c < 0x80) { out += (char)((c >= 'A' && c <= 'Z') ? c + 32 : c); p++; continue; }
        if (c == 0xD0 && p[1]) {
            unsigned char d = p[1];
            if (d >= 0x90 && d <= 0x9F) { out += (char)0xD0; out += (char)(d + 0x20); }
            else if (d >= 0xA0 && d <= 0xAF) { out += (char)0xD1; out += (char)(d - 0x20); }
            else if (d >= 0x80 && d <= 0x8F) { out += (char)0xD1; out += (char)(d + 0x10); } // Ѐ..Џ (Є, І, Ї)
            else { out += (char)c; out += (char)d; }
            p += 2; continue;
        }
        if (c == 0xD2 && p[1] == 0x90) { out += (char)0xD2; out += (char)0x91; p += 2; continue; } // Ґ
        out += (char)c; p++;
    }
    return out;
}

const char* RegionIdOf(const char* name) {
    std::string n = Lower(name);
    for (auto& s : kStems) if (n.find(s.stem) != std::string::npos) return s.id;
    return nullptr;
}

// 0 = kyiv_city, 1..N = індекс у бітмапі, -1 = невідомо
int SlotOf(const char* id) {
    if (!id) return -1;
    if (strcmp(id, "kyiv_city") == 0) return 0;
    for (int i = 0; i < UA_REGION_COUNT; i++) if (strcmp(kUaRegionIds[i], id) == 0) return i + 1;
    return -1;
}

const char* NameOf(const std::string& id) {
    for (auto& r : kNames) if (id == r.id) return r.name;
    return id.c_str();
}

std::string Hm(time_t t) {
    struct tm tmv;
    localtime_r(&t, &tmv);
    char b[24];
    snprintf(b, sizeof(b), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    return b;
}

bool ClockValid() { return time(nullptr) > 1700000000; }

std::string Minutes(int64_t since_us) {
    int m = (int)((esp_timer_get_time() - since_us) / 60000000LL);
    if (m < 60) return std::to_string(m) + " хв";
    char b[32];
    snprintf(b, sizeof(b), "%d год %02d хв", m / 60, m % 60);
    return b;
}

}  // namespace

AlertMonitor& AlertMonitor::GetInstance() {
    static AlertMonitor inst;
    return inst;
}

void AlertMonitor::Start(Display* display, std::function<void()> wake_cb) {
    display_ = display;
    wake_cb_ = std::move(wake_cb);
    {
        Settings st("alerts", false);
        home_ = st.GetString("home", "kyiv_city");
        sound_ = st.GetBool("sound", true);
        mode_ = st.GetInt("screen", kModeAuto);
        if (mode_ < kModeMap || mode_ > kModeAuto) mode_ = kModeAuto;
        if (SlotOf(home_.c_str()) < 0) home_ = "kyiv_city";
    }
    // сервер xiaozhi вже виставляє годинник у місцевому часі (з урахуванням зсуву),
    // тому тут без додаткового часового поясу
    setenv("TZ", "UTC0", 1);
    tzset();
    xTaskCreate(TaskEntry, "alerts", 8192, this, 3, nullptr);
    xTaskCreate(UiTaskEntry, "alerts_ui", 4096, this, 2, nullptr);
}

void AlertMonitor::UiTaskEntry(void* arg) {
    static_cast<AlertMonitor*>(arg)->UiLoop();
    vTaskDelete(nullptr);
}

// Що показувати: -1 — нічого (екран асистента), 0 — мапа, 1 — годинник.
// Під час тривоги / відбою / тесту завжди мапа; системні екрани (Wi-Fi, код активації,
// оновлення, помилка) не перекриваємо.
int AlertMonitor::Target() {
    auto st = Application::GetInstance().GetDeviceState();
    if (st == kDeviceStateWifiConfiguring || st == kDeviceStateActivating ||
        st == kDeviceStateUpgrading || st == kDeviceStateFatalError || st == kDeviceStateAudioTesting) return -1;
    int64_t now = esp_timer_get_time();
    // мапа примусово лише на початку тривоги (90 с), на відбої (20 с) і під час тесту;
    // далі — той екран, який вибрав користувач (на годиннику тривогу видно червоною плашкою)
    if (force_map_until_us_ > now || test_until_us_ > now) return 0;
    int m = mode_;
    if (m == kModeAuto) return (now / kRotateUs) % 2 ? 1 : 0;   // по черзі: мапа → годинник
    return m == kModeFace ? -1 : m;
}

void AlertMonitor::ApplyTarget() {
    int t = Target();
    if (t != shown_) Show(t);
    else if (shown_ == 0) { RenderMap(); UpdateTexts(); }
    else if (shown_ == 1) UpdateClock();
}

void AlertMonitor::UiLoop() {
    vTaskDelay(pdMS_TO_TICKS(1500));   // даємо дисплею завершити SetupUI
    int64_t last_refresh = 0;
    int last_state = -1;
    while (true) {
        int target = Target();
        int st = (int)Application::GetInstance().GetDeviceState();
        int64_t now = esp_timer_get_time();
        int64_t period = shown_ == 1 ? 1000000LL : 5000000LL;
        if (display_ && (target != shown_ || (shown_ >= 0 && (now - last_refresh > period || st != last_state)))) {
            DisplayLockGuard lock(display_);
            ApplyTarget();
            last_refresh = now;
        }
        last_state = st;
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

void AlertMonitor::TaskEntry(void* arg) {
    static_cast<AlertMonitor*>(arg)->Run();
    vTaskDelete(nullptr);
}

void AlertMonitor::Run() {
    auto& app = Application::GetInstance();
    // чекаємо, поки пристрій підключиться до Wi-Fi і стартує
    while (true) {
        auto s = app.GetDeviceState();
        if (s == kDeviceStateIdle || s == kDeviceStateListening || s == kDeviceStateSpeaking ||
            s == kDeviceStateConnecting) break;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "start polling, home=%s", home_.c_str());

    while (true) {
        if (app.GetDeviceState() != kDeviceStateUpgrading) {
            bool a[UA_REGION_COUNT + 1] = {};
            std::string src;
            if (Fetch(a, src)) {
                Apply(a, src);
            } else {
                // немає даних — оновлюємо екран (стан «немає даних» після 2.5 хв)
                if (display_) {
                    DisplayLockGuard lock(display_);
                    UpdateTexts();
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(kPollSeconds * 1000));
    }
}

bool AlertMonitor::Fetch(bool alerts[UA_REGION_COUNT + 1], std::string& src) {
    auto network = Board::GetInstance().GetNetwork();
    if (!network) return false;

    // одна-дві спроби за цикл; при невдачі — наступне піджерело в наступному циклі (як у «Вітальні»)
    for (int attempt = 0; attempt < 2; attempt++) {
        const char* s = kUbSources[src_idx_];
        std::string url = std::string(use_http_ ? "http" : "https") + "://ubilling.net.ua/aerialalerts/";
        if (*s) url += std::string("?source=") + s;

        auto http = network->CreateHttp(0);
        http->SetHeader("User-Agent", "TT02-Trivoga/1.0");
        int code = 0;
        std::string body;
        if (http->Open("GET", url)) {
            code = http->GetStatusCode();
            if (code == 200) body = http->ReadAll();
            http->Close();
        }

        if (code == 200 && !body.empty()) {
            cJSON* root = cJSON_Parse(body.c_str());
            if (root) {
                cJSON* states = cJSON_GetObjectItem(root, "states");
                if (!cJSON_IsObject(states)) states = root;
                int known = 0;
                bool city_seen = false;
                cJSON* item = nullptr;
                cJSON_ArrayForEach(item, states) {
                    if (!cJSON_IsObject(item) || !item->string) continue;
                    cJSON* v = cJSON_GetObjectItem(item, "alertnow");
                    if (!v) v = cJSON_GetObjectItem(item, "alertNow");
                    if (!v) v = cJSON_GetObjectItem(item, "enabled");
                    if (!v) continue;
                    known++;
                    int slot = SlotOf(RegionIdOf(item->string));
                    if (slot == 0) city_seen = true;
                    if (slot >= 0 && cJSON_IsTrue(v)) alerts[slot] = true;
                }
                cJSON_Delete(root);
                if (known >= 20) {
                    city_seen_ = city_seen;
                    src = std::string("ubilling") + (*s ? std::string(" · ") + s : "");
                    return true;
                }
                ESP_LOGW(TAG, "bad format from '%s' (%d regions)", s, known);
            }
        } else {
            ESP_LOGW(TAG, "fetch %s -> %d", url.c_str(), code);
            if (code == 429) return false;            // ліміт — чекаємо наступного циклу
            if (code == 0 && !use_http_) { use_http_ = true; continue; }   // https не пройшов — пробуємо http
        }
        src_idx_ = (src_idx_ + 1) % kUbSourceCount;
        if (code == 0) use_http_ = !use_http_;
        vTaskDelay(pdMS_TO_TICKS(800));
    }
    return false;
}

void AlertMonitor::Apply(const bool a[UA_REGION_COUNT + 1], const std::string& src) {
    bool was_home, now_home, first;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        first = !have_data_;
        was_home = HomeAlert();
        memcpy(alerts_, a, sizeof(alerts_));
        have_data_ = true;
        last_ok_us_ = esp_timer_get_time();
        src_ = src;
        now_home = HomeAlert();
    }
    ESP_LOGI(TAG, "%s: %d active, home(%s)=%d", src.c_str(), ActiveCount(), home_.c_str(), now_home);

    if (now_home && wake_cb_) wake_cb_();     // під час тривоги екран не тьмяніє
    if (now_home && (!was_home || first)) {
        OnHomeAlarmStart(!first);     // при першому запуску під час тривоги — короткий сигнал замість сирени
    } else if (!now_home && was_home && !first) {
        OnHomeAlarmEnd();
    }
    if (display_) {
        DisplayLockGuard lock(display_);
        if (shown_ == 0) RenderMap();
        UpdateTexts();
        if (shown_ == 1) UpdateClock();
    }
}

void AlertMonitor::OnHomeAlarmStart(bool play_siren) {
    RadioPlayer::GetInstance().Stop();      // тривога важливіша за музику
    alarm_since_us_ = esp_timer_get_time();
    force_map_until_us_ = alarm_since_us_ + kForceMapUs;
    allclear_until_us_ = 0;
    if (wake_cb_) wake_cb_();
    if (display_) {
        DisplayLockGuard lock(display_);
        ApplyTarget();
    }
    if (sound_) {
        auto& app = Application::GetInstance();
        if (play_siren) app.PlaySound(std::string_view((const char*)kSirenOgg, kSirenOgg_len));
        else app.PlaySound(std::string_view((const char*)kBeepOgg, kBeepOgg_len));
    }
}

void AlertMonitor::OnHomeAlarmEnd() {
    allclear_until_us_ = esp_timer_get_time() + kAllClearShowUs;
    force_map_until_us_ = allclear_until_us_;
    if (wake_cb_) wake_cb_();
    if (display_) {
        DisplayLockGuard lock(display_);
        ApplyTarget();
    }
    if (sound_) {
        Application::GetInstance().PlaySound(std::string_view((const char*)kAllClearOgg, kAllClearOgg_len));
    }
}

int AlertMonitor::HomeIndex() const { return SlotOf(home_.c_str()); }

bool AlertMonitor::HomeAlert() const {
    if (test_until_us_ > esp_timer_get_time()) return true;
    int i = HomeIndex();
    if (i < 0) return false;
    // м. Київ: тривога в місті (якщо джерело дає місто окремо) або в Київській області
    // м. Київ — окремо від області (як у «Вітальні»); область — лише якщо джерело не дає місто
    if (i == 0) return city_seen_ ? alerts_[0] : (alerts_[0] || alerts_[SlotOf("kyiv")]);
    return alerts_[i];
}

std::string AlertMonitor::HomeName() const { return NameOf(home_); }

int AlertMonitor::ActiveCount() const {
    int n = 0;
    for (int i = 1; i <= UA_REGION_COUNT; i++) if (alerts_[i]) n++;
    if (alerts_[0] && !alerts_[SlotOf("kyiv")]) n++;
    return n;
}

// ---------------- UI ----------------

void AlertMonitor::EnsureUi() {
    if (root_) return;
    lv_obj_t* layer = lv_layer_top();
    int w = lv_display_get_horizontal_resolution(lv_display_get_default());
    int h = lv_display_get_vertical_resolution(lv_display_get_default());

    root_ = lv_obj_create(layer);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, w, h);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x0F1115), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    header_ = lv_obj_create(root_);
    lv_obj_remove_style_all(header_);
    lv_obj_set_size(header_, w, kHeaderH);
    lv_obj_set_style_bg_opa(header_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(header_, LV_OBJ_FLAG_SCROLLABLE);

    title_ = lv_label_create(header_);
    lv_obj_set_style_text_font(title_, &font_ua_bold, 0);
    lv_obj_set_style_text_color(title_, lv_color_white(), 0);
    lv_label_set_long_mode(title_, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(title_, w - 12);
    lv_obj_set_style_text_align(title_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(title_);

    canvas_buf_ = (uint16_t*)heap_caps_malloc(UA_MAP_W * UA_MAP_H * 2, MALLOC_CAP_SPIRAM);
    if (canvas_buf_) {
        canvas_ = lv_canvas_create(root_);
        lv_canvas_set_buffer(canvas_, canvas_buf_, UA_MAP_W, UA_MAP_H, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(canvas_, (w - UA_MAP_W) / 2, kHeaderH);
    } else {
        ESP_LOGE(TAG, "no PSRAM for map canvas");
    }

    kyiv_dot_ = lv_obj_create(root_);
    lv_obj_remove_style_all(kyiv_dot_);
    lv_obj_set_size(kyiv_dot_, 9, 9);
    lv_obj_set_style_radius(kyiv_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(kyiv_dot_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(kyiv_dot_, 2, 0);
    lv_obj_set_style_border_color(kyiv_dot_, lv_color_hex(0x0F1115), 0);
    lv_obj_set_pos(kyiv_dot_, (w - UA_MAP_W) / 2 + UA_KYIV_X - 4, kHeaderH + UA_KYIV_Y - 4);

    foot_left_ = lv_label_create(root_);
    lv_obj_set_style_text_font(foot_left_, &font_ua_med, 0);
    lv_obj_set_style_text_color(foot_left_, lv_color_hex(0xE8EAED), 0);
    lv_obj_align(foot_left_, LV_ALIGN_BOTTOM_LEFT, 16, -6);

    foot_right_ = lv_label_create(root_);
    lv_obj_set_style_text_font(foot_right_, &font_ua_med, 0);
    lv_obj_set_style_text_color(foot_right_, lv_color_hex(0x9AA0A6), 0);
    lv_obj_align(foot_right_, LV_ALIGN_BOTTOM_RIGHT, -16, -6);

    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);

    // ---- годинник ----
    clock_root_ = lv_obj_create(layer);
    lv_obj_remove_style_all(clock_root_);
    lv_obj_set_size(clock_root_, w, h);
    lv_obj_set_style_bg_color(clock_root_, lv_color_hex(0x0F1115), 0);
    lv_obj_set_style_bg_opa(clock_root_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(clock_root_, LV_OBJ_FLAG_SCROLLABLE);

    clk_date_ = lv_label_create(clock_root_);
    lv_obj_set_style_text_font(clk_date_, &font_ua_date, 0);
    lv_obj_set_style_text_color(clk_date_, lv_color_hex(0x9AA0A6), 0);
    lv_obj_align(clk_date_, LV_ALIGN_TOP_MID, 0, 22);

    clk_time_ = lv_label_create(clock_root_);
    lv_obj_set_style_text_font(clk_time_, &font_ua_clock, 0);
    lv_obj_set_style_text_color(clk_time_, lv_color_white(), 0);
    lv_obj_align(clk_time_, LV_ALIGN_CENTER, 0, -10);

    clk_pill_ = lv_obj_create(clock_root_);
    lv_obj_remove_style_all(clk_pill_);
    lv_obj_set_size(clk_pill_, LV_SIZE_CONTENT, 32);
    lv_obj_set_style_radius(clk_pill_, 16, 0);
    lv_obj_set_style_bg_opa(clk_pill_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(clk_pill_, 14, 0);
    lv_obj_remove_flag(clk_pill_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(clk_pill_, LV_ALIGN_BOTTOM_MID, 0, -40);
    clk_status_ = lv_label_create(clk_pill_);
    lv_obj_set_style_text_font(clk_status_, &font_ua_date, 0);
    lv_obj_set_style_text_color(clk_status_, lv_color_white(), 0);
    lv_obj_center(clk_status_);

    clk_foot_ = lv_label_create(clock_root_);
    lv_obj_set_style_text_font(clk_foot_, &font_ua_med, 0);
    lv_obj_set_style_text_color(clk_foot_, lv_color_hex(0x8AB4F8), 0);
    lv_obj_align(clk_foot_, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_add_flag(clock_root_, LV_OBJ_FLAG_HIDDEN);
}

void AlertMonitor::RenderMap() {
    if (!canvas_buf_) return;
    const uint16_t c_bg = lv_color_to_u16(lv_color_hex(0x0F1115));
    const uint16_t c_calm = lv_color_to_u16(lv_color_hex(0x3A3F4B));
    const uint16_t c_home = lv_color_to_u16(lv_color_hex(0x5B6375));
    const uint16_t c_alert = lv_color_to_u16(lv_color_hex(0xE5383B));
    const uint16_t c_home_alert = lv_color_to_u16(lv_color_hex(0xFF5A4E));
    const uint16_t c_stale = lv_color_to_u16(lv_color_hex(0x2A2D35));

    bool stale = !have_data_ || esp_timer_get_time() - last_ok_us_ > kStaleUs;
    bool testing = test_until_us_ > esp_timer_get_time();
    int home = HomeIndex();
    int home_paint = home == 0 ? SlotOf("kyiv") : home;   // місто малюємо разом з областю
    uint16_t lut[256];
    for (int i = 0; i < 256; i++) lut[i] = c_bg;
    for (int i = 1; i <= UA_REGION_COUNT; i++) {
        bool on = alerts_[i] || (testing && i == home_paint);
        bool is_home = i == home_paint;
        lut[i] = stale ? c_stale : on ? (is_home ? c_home_alert : c_alert) : (is_home ? c_home : c_calm);
    }
    // регіони + підписи міст (біле AA-напис із темним контуром)
    for (int p = 0; p < UA_MAP_W * UA_MAP_H; p++) {
        uint16_t c = lut[kUaMap[p]];
        uint8_t ao = kUaLabelO[p], at = kUaLabelA[p];
        if (ao | at) {
            int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
            int o = ao * 217 / 255;                       // контур ~85%
            r = r * (255 - o) / 255; g = g * (255 - o) / 255; b = b * (255 - o) / 255;
            r += (31 - r) * at / 255; g += (63 - g) * at / 255; b += (31 - b) * at / 255;
            c = (uint16_t)((r << 11) | (g << 5) | b);
        }
        canvas_buf_[p] = c;
    }
    lv_obj_invalidate(canvas_);

    bool city = alerts_[0] || (testing && home == 0);
    lv_obj_set_style_bg_color(kyiv_dot_, city ? lv_color_hex(0xFFD60A) : lv_color_white(), 0);
}

void AlertMonitor::UpdateTexts() {
    if (!root_) return;
    int64_t now = esp_timer_get_time();
    bool stale = !have_data_ || now - last_ok_us_ > kStaleUs;
    bool testing = test_until_us_ > now;
    bool home_alert = HomeAlert();

    uint32_t bg;
    std::string title;
    if (testing) { bg = 0xB3261E; title = "ТЕСТ · ТРИВОГА"; }
    else if (home_alert && !stale) { bg = 0xC62828; title = "ПОВІТРЯНА ТРИВОГА"; }
    else if (allclear_until_us_ > now && !stale) { bg = 0x1E8E3E; title = "ВІДБІЙ ТРИВОГИ"; }
    else if (stale) { bg = 0x8A5A00; title = have_data_ ? "НЕМАЄ ЗВ'ЯЗКУ" : "ЗАВАНТАЖЕННЯ…"; }
    else { bg = 0x1F2A24; title = "Тривоги немає"; }
    lv_obj_set_style_bg_color(header_, lv_color_hex(bg), 0);
    lv_label_set_text(title_, title.c_str());

    std::string left = home_ == "kyiv_city" ? std::string("Київ") : HomeName();
    if ((home_alert || testing) && alarm_since_us_) left += " · " + Minutes(alarm_since_us_);
    auto dst = Application::GetInstance().GetDeviceState();
    std::string radio = RadioPlayer::GetInstance().NowPlaying();
    if (!radio.empty() && !home_alert && !testing) left = "Радіо · " + radio;
    if (dst == kDeviceStateListening) left = "Слухаю…";
    else if (dst == kDeviceStateSpeaking) left = "Говорю…";
    else if (dst == kDeviceStateConnecting) left = "З'єднання…";
    lv_obj_set_style_text_color(foot_left_, lv_color_hex(dst == kDeviceStateListening || dst == kDeviceStateSpeaking ? 0x8AB4F8 : 0xE8EAED), 0);
    lv_label_set_text(foot_left_, left.c_str());

    std::string right;
    if (have_data_) {
        int n = ActiveCount();
        right = std::to_string(n) + " обл.";
        if (ClockValid()) right += " · " + Hm(time(nullptr));
    }
    lv_label_set_text(foot_right_, right.c_str());
}

void AlertMonitor::Show(int what) {
    EnsureUi();
    if (!root_ || !clock_root_) return;
    shown_ = what;
    visible_ = what == 0;
    if (what == 0) {
        RenderMap();
        UpdateTexts();
        lv_obj_add_flag(clock_root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(root_);
    } else if (what == 1) {
        UpdateClock();
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(clock_root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(clock_root_);
    } else {
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(clock_root_, LV_OBJ_FLAG_HIDDEN);
    }
}

void AlertMonitor::SetMode(int mode, bool save) {
    if (mode < kModeMap || mode > kModeAuto) return;
    mode_ = mode;
    force_map_until_us_ = 0;      // явний вибір користувача важливіший за автопоказ мапи
    if (save) {
        Settings st("alerts", true);
        st.SetInt("screen", mode);
    }
    if (display_) {
        DisplayLockGuard lock(display_);
        ApplyTarget();
    }
}

bool AlertMonitor::SetModeByName(const std::string& name) {
    std::string n = Lower(name.c_str());
    if (n.find("auto") != std::string::npos || n.find("черз") != std::string::npos || n.find("чергу") != std::string::npos ||
        n.find("rotat") != std::string::npos || n.find("обидва") != std::string::npos || n.find("both") != std::string::npos ||
        n.find("перемика") != std::string::npos) { SetMode(kModeAuto); return true; }
    if (n.find("map") != std::string::npos || n.find("мап") != std::string::npos || n.find("карт") != std::string::npos ||
        n.find("тривог") != std::string::npos) { SetMode(kModeMap); return true; }
    if (n.find("clock") != std::string::npos || n.find("time") != std::string::npos || n.find("годин") != std::string::npos ||
        n.find("час") != std::string::npos) { SetMode(kModeClock); return true; }
    if (n.find("face") != std::string::npos || n.find("облич") != std::string::npos || n.find("робот") != std::string::npos ||
        n.find("assistant") != std::string::npos || n.find("асистент") != std::string::npos) { SetMode(kModeFace); return true; }
    return false;
}

const char* AlertMonitor::ModeName() const {
    int m = mode_;
    return m == kModeMap ? "map" : m == kModeClock ? "clock" : m == kModeAuto ? "auto (мапа і годинник по черзі)" : "face";
}

void AlertMonitor::CycleMode() {
    SetMode((mode_ + 1) % 4);       // мапа → годинник → обличчя → по черзі → мапа
}

void AlertMonitor::StartTest() {
    RadioPlayer::GetInstance().Stop();
    ESP_LOGI(TAG, "test alarm");
    test_until_us_ = esp_timer_get_time() + kTestUs;
    alarm_since_us_ = esp_timer_get_time();
    if (wake_cb_) wake_cb_();
    if (display_) {
        DisplayLockGuard lock(display_);
        ApplyTarget();
    }
    Application::GetInstance().PlaySound(std::string_view((const char*)kSirenOgg, kSirenOgg_len));
    // після тесту — звук відбою
    xTaskCreate([](void* arg) {
        vTaskDelay(pdMS_TO_TICKS(kTestUs / 1000 + 300));
        auto* self = static_cast<AlertMonitor*>(arg);
        self->allclear_until_us_ = esp_timer_get_time() + 8LL * 1000 * 1000;
        Application::GetInstance().PlaySound(std::string_view((const char*)kAllClearOgg, kAllClearOgg_len));
        if (self->display_) {
            DisplayLockGuard lock(self->display_);
            self->ApplyTarget();
        }
        vTaskDelete(nullptr);
    }, "alert_test", 3072, this, 2, nullptr);
}

std::string AlertMonitor::StatusText() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!have_data_) return "Даних про тривоги ще немає (немає зв'язку з сервером).";
    std::string s;
    if (HomeAlert()) s = "Зараз повітряна тривога: " + HomeName() + ", триває " + Minutes(alarm_since_us_) + ".";
    else s = "Тривоги немає: " + HomeName() + ".";
    s += " Усього тривога в " + std::to_string(ActiveCount()) + " областях:";
    bool any = false;
    for (int i = 1; i <= UA_REGION_COUNT; i++) {
        if (alerts_[i]) { s += std::string(any ? ", " : " ") + NameOf(kUaRegionIds[i - 1]); any = true; }
    }
    if (!any) s += " ніде";
    s += ".";
    if (esp_timer_get_time() - last_ok_us_ > kStaleUs) s += " Увага: дані застарілі.";
    return s;
}

void AlertMonitor::SetSoundEnabled(bool on) {
    sound_ = on;
    Settings st("alerts", true);
    st.SetBool("sound", on);
}

bool AlertMonitor::SetHomeRegion(const std::string& id) {
    const char* rid = SlotOf(id.c_str()) >= 0 ? id.c_str() : RegionIdOf(id.c_str());
    if (SlotOf(rid) < 0) return false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        home_ = rid;
    }
    Settings st("alerts", true);
    st.SetString("home", home_);
    if (display_) {
        DisplayLockGuard lock(display_);
        ApplyTarget();
    }
    return true;
}

std::string AlertMonitor::TimeText() {
    if (!ClockValid()) return "Час ще не синхронізовано.";
    static const char* kDays[] = {"неділя", "понеділок", "вівторок", "середа", "четвер", "пʼятниця", "субота"};
    static const char* kMonths[] = {"січня", "лютого", "березня", "квітня", "травня", "червня",
                                    "липня", "серпня", "вересня", "жовтня", "листопада", "грудня"};
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char b[96];
    snprintf(b, sizeof(b), "Зараз %02d:%02d, %s, %d %s %d року (київський час).",
             tmv.tm_hour, tmv.tm_min, kDays[tmv.tm_wday], tmv.tm_mday, kMonths[tmv.tm_mon], tmv.tm_year + 1900);
    return b;
}

void AlertMonitor::UpdateClock() {
    if (!clock_root_) return;
    static const char* kDays[] = {"неділя", "понеділок", "вівторок", "середа", "четвер", "пʼятниця", "субота"};
    static const char* kMonths[] = {"січня", "лютого", "березня", "квітня", "травня", "червня",
                                    "липня", "серпня", "вересня", "жовтня", "листопада", "грудня"};
    if (ClockValid()) {
        time_t t = time(nullptr);
        struct tm tmv;
        localtime_r(&t, &tmv);
        char b[64];
        snprintf(b, sizeof(b), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        lv_label_set_text(clk_time_, b);
        snprintf(b, sizeof(b), "%s, %d %s", kDays[tmv.tm_wday], tmv.tm_mday, kMonths[tmv.tm_mon]);
        lv_label_set_text(clk_date_, b);
    } else {
        lv_label_set_text(clk_time_, "--:--");
        lv_label_set_text(clk_date_, "синхронізація часу…");
    }

    int64_t now = esp_timer_get_time();
    bool stale = !have_data_ || now - last_ok_us_ > kStaleUs;
    std::string place = home_ == "kyiv_city" ? std::string("Київ") : HomeName();
    uint32_t bg;
    std::string st;
    if (stale) { bg = 0x5C4A12; st = have_data_ ? "немає звʼязку" : "завантаження…"; }
    else if (HomeAlert()) { bg = 0xC62828; st = "ТРИВОГА · " + place + " · " + Minutes(alarm_since_us_); }
    else { bg = 0x1E5C34; st = "тривоги немає · " + place; }
    lv_obj_set_style_bg_color(clk_pill_, lv_color_hex(bg), 0);
    lv_label_set_text(clk_status_, st.c_str());

    auto dst = Application::GetInstance().GetDeviceState();
    const char* foot = "";
    if (dst == kDeviceStateListening) foot = "Слухаю…";
    else if (dst == kDeviceStateSpeaking) foot = "Говорю…";
    else if (dst == kDeviceStateConnecting) foot = "Зʼєднання…";
    else if (!RadioPlayer::GetInstance().NowPlaying().empty()) {
        static std::string rb;
        rb = "Радіо · " + RadioPlayer::GetInstance().NowPlaying();
        lv_obj_set_style_text_color(clk_foot_, lv_color_hex(0xF6C453), 0);
        lv_label_set_text(clk_foot_, rb.c_str());
        return;
    }
    else if (have_data_ && !stale) {
        static char b[48];
        snprintf(b, sizeof(b), "тривога в %d обл.", ActiveCount());
        lv_obj_set_style_text_color(clk_foot_, lv_color_hex(0x9AA0A6), 0);
        lv_label_set_text(clk_foot_, b);
        return;
    }
    lv_obj_set_style_text_color(clk_foot_, lv_color_hex(0x8AB4F8), 0);
    lv_label_set_text(clk_foot_, foot);
}
