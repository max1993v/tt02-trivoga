#include "radio_player.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>
#include <vector>

#include "application.h"
#include "board.h"
#include "settings.h"

#include "esp_ae_rate_cvt.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_types.h"
#include "esp_aac_dec.h"
#include "esp_mp3_dec.h"

#define TAG "Radio"

namespace {

const char* kApiHosts[] = {"de1.api.radio-browser.info", "fi1.api.radio-browser.info",
                           "de2.api.radio-browser.info", "at1.api.radio-browser.info"};
constexpr char kUserAgent[] = "TT02-Radio/1.0";
constexpr size_t kInBufSize = 4096;
constexpr int kMaxFailures = 4;

std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else if (c == ' ') out += "%20";
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

std::string Trim(std::string s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
    return s.substr(i);
}

bool CodecOk(const std::string& codec) {
    std::string c;
    for (char ch : codec) c += (char)toupper((unsigned char)ch);
    return c == "MP3" || c == "AAC" || c == "AAC+" || c == "AACP" || c == "HE-AAC";
}

bool IsAac(const std::string& codec_or_type) {
    std::string c;
    for (char ch : codec_or_type) c += (char)tolower((unsigned char)ch);
    return c.find("aac") != std::string::npos;
}

bool Conversing(DeviceState s) {
    return s == kDeviceStateListening || s == kDeviceStateSpeaking || s == kDeviceStateConnecting;
}

}  // namespace

RadioPlayer& RadioPlayer::GetInstance() {
    static RadioPlayer inst;
    return inst;
}

void RadioPlayer::LoadLast() {
    if (loaded_) return;
    loaded_ = true;
    Settings st("radio", false);
    station_.name = st.GetString("name", "");
    station_.url = st.GetString("url", "");
    station_.codec = st.GetString("codec", "MP3");
    station_.bitrate = st.GetInt("bitrate", 0);
}

void RadioPlayer::SaveLast() {
    Settings st("radio", true);
    st.SetString("name", station_.name);
    st.SetString("url", station_.url);
    st.SetString("codec", station_.codec);
    st.SetInt("bitrate", station_.bitrate);
}

bool RadioPlayer::SearchApi(const std::string& path, Station& out) {
    auto network = Board::GetInstance().GetNetwork();
    for (const char* host : kApiHosts) {
        auto http = network->CreateHttp(3);
        http->SetTimeout(6000);
        http->SetHeader("User-Agent", kUserAgent);
        std::string url = std::string("https://") + host + path;
        if (!http->Open("GET", url)) {
            ESP_LOGW(TAG, "api %s: connect failed", host);
            continue;
        }
        int code = http->GetStatusCode();
        std::string body = code == 200 ? http->ReadAll() : "";
        http->Close();
        if (code != 200) {
            ESP_LOGW(TAG, "api %s: HTTP %d", host, code);
            continue;
        }
        cJSON* root = cJSON_Parse(body.c_str());
        if (!cJSON_IsArray(root)) {
            cJSON_Delete(root);
            continue;
        }
        bool found = false;
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, root) {
            auto str = [&](const char* k) {
                cJSON* v = cJSON_GetObjectItem(item, k);
                return cJSON_IsString(v) ? std::string(v->valuestring) : std::string();
            };
            std::string u = str("url_resolved");
            if (u.empty()) u = str("url");
            std::string codec = str("codec");
            cJSON* ok = cJSON_GetObjectItem(item, "lastcheckok");
            if (u.empty() || !CodecOk(codec)) continue;
            if (cJSON_IsNumber(ok) && ok->valueint != 1) continue;
            if (u.find(".m3u8") != std::string::npos || u.find(".pls") != std::string::npos ||
                u.find(".m3u") != std::string::npos) continue;
            out.name = Trim(str("name"));
            out.url = u;
            out.codec = codec;
            cJSON* br = cJSON_GetObjectItem(item, "bitrate");
            out.bitrate = cJSON_IsNumber(br) ? br->valueint : 0;
            found = true;
            break;
        }
        cJSON_Delete(root);
        return found;     // сервер відповів — інші не питаємо
    }
    return false;
}

bool RadioPlayer::Lookup(const std::string& query, bool genre, Station& out) {
    std::string q = UrlEncode(Trim(query));
    const std::string tail = "&order=clickcount&reverse=true&hidebroken=true&limit=15";
    if (genre) {
        return SearchApi("/json/stations/search?tag=" + q + tail, out);
    }
    // спершу серед українських станцій, потім — у всьому світі
    if (SearchApi("/json/stations/search?name=" + q + "&countrycode=UA" + tail, out)) return true;
    return SearchApi("/json/stations/search?name=" + q + tail, out);
}

// Викликається з головного потоку (команда асистента) — тут нічого мережевого, лише запит:
// пошук і підключення робить потік радіо з більшим стеком.
std::string RadioPlayer::Play(const std::string& query, bool genre) {
    std::string q = Trim(query);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        LoadLast();
        request_.query = q;
        request_.genre = genre;
        pending_ = true;
        error_.clear();
    }
    generation_++;
    active_ = true;
    ArmResume();
    EnsureTask();
    ESP_LOGI(TAG, "request '%s' genre=%d", q.c_str(), genre);
    if (q.empty()) return "Вмикаю радіо. Відповідай одним коротким реченням.";
    return std::string("Шукаю й вмикаю ") + (genre ? "радіо в жанрі «" : "радіо «") + q +
           "». Музика почнеться, щойно ти закінчиш говорити. Відповідай одним коротким реченням.";
}

void RadioPlayer::Stop() {
    if (active_) ESP_LOGI(TAG, "stop");
    {
        std::lock_guard<std::mutex> lk(mutex_);
        pending_ = false;
    }
    active_ = false;
    generation_++;
}

void RadioPlayer::Toggle() {
    if (active_) Stop();
    else Play("", false);
}

std::string RadioPlayer::NowPlaying() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!active_) return error_until_us_ > esp_timer_get_time() ? error_ : "";
    if (pending_) return "пошук…";
    return station_.name;
}

void RadioPlayer::EnsureTask() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (task_started_) return;
    task_started_ = true;
    xTaskCreate(TaskEntry, "radio", 12288, this, 4, nullptr);
}

void RadioPlayer::TaskEntry(void* arg) {
    static_cast<RadioPlayer*>(arg)->Loop();
    vTaskDelete(nullptr);
}

void RadioPlayer::Loop() {
    esp_mp3_dec_register();
    esp_aac_dec_register();

    auto& app = Application::GetInstance();
    int failures = 0;

    while (true) {
        if (!active_) {
            failures = 0;
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        // новий запит: пошук станції (одразу, навіть поки асистент ще говорить)
        bool has_req = false;
        Request req;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (pending_) { req = request_; has_req = true; }
        }
        if (has_req) {
            uint32_t gen = generation_;
            Station st;
            bool ok = false;
            if (req.query.empty()) {
                std::lock_guard<std::mutex> lk(mutex_);
                if (!station_.url.empty()) { st = station_; ok = true; }
            }
            if (!ok) ok = Lookup(req.query.empty() ? "Hit FM" : req.query, req.genre, st);
            std::lock_guard<std::mutex> lk(mutex_);
            if (generation_ != gen) continue;          // поки шукали, прийшов новий запит / стоп
            pending_ = false;
            if (ok) {
                ESP_LOGI(TAG, "found %s (%s %dk) %s", st.name.c_str(), st.codec.c_str(), st.bitrate, st.url.c_str());
                station_ = st;
                SaveLast();
                failures = 0;
            } else {
                ESP_LOGW(TAG, "not found: %s", req.query.c_str());
                error_ = "не знайдено «" + req.query + "»";
                error_until_us_ = esp_timer_get_time() + 20LL * 1000 * 1000;
                active_ = false;
                continue;
            }
        }

        auto state = app.GetDeviceState();
        if (state != kDeviceStateIdle) {
            // Розмова: музика на паузі. Повертаємо її лише після того, як асистент ВИКОНАВ команду
            // (виклик інструмента) і договорив відповідь (подія Speaking → Listening з колбека стану).
            // Привітання після слова-активації та звичайні запитання розмову не обривають.
            if (resume_armed_ && state == kDeviceStateListening &&
                (reply_done_ || esp_timer_get_time() - arm_time_us_ > 12LL * 1000 * 1000)) {
                resume_armed_ = false;
                reply_done_ = false;
                vTaskDelay(pdMS_TO_TICKS(400));
                if (active_ && app.GetDeviceState() == kDeviceStateListening) {
                    ESP_LOGI(TAG, "command done, closing conversation to resume radio");
                    app.ToggleChatState();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }
        resume_armed_ = false;
        reply_done_ = false;

        Station st;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            st = station_;
        }
        uint32_t gen = generation_;
        auto r = StreamOnce(st, gen);
        if (r == Result::Failed || r == Result::Ended) {
            if (++failures >= kMaxFailures) {
                ESP_LOGW(TAG, "too many failures, stopping");
                active_ = false;
                failures = 0;
            } else {
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
        } else {
            failures = 0;
        }
    }
}

RadioPlayer::Result RadioPlayer::StreamOnce(const Station& st, uint32_t gen) {
    auto& app = Application::GetInstance();
    auto network = Board::GetInstance().GetNetwork();
    auto codec = Board::GetInstance().GetAudioCodec();
    const int out_rate = codec->output_sample_rate();

    // ---- підключення (з обробкою переадресацій) ----
    std::string url = st.url;
    std::unique_ptr<Http> http;
    int code = 0;
    for (int hop = 0; hop < 5; hop++) {
        http = network->CreateHttp(2);
        http->SetTimeout(8000);
        http->SetHeader("User-Agent", kUserAgent);
        http->SetHeader("Icy-MetaData", "0");
        if (!http->Open("GET", url)) {
            ESP_LOGW(TAG, "connect failed: %s", url.c_str());
            return Result::Failed;
        }
        code = http->GetStatusCode();
        if (code >= 301 && code <= 308) {
            std::string loc = http->GetResponseHeader("Location");
            if (loc.empty()) loc = http->GetResponseHeader("location");
            http->Close();
            if (loc.empty()) return Result::Failed;
            url = loc;
            continue;
        }
        break;
    }
    if (code != 200) {
        ESP_LOGW(TAG, "HTTP %d for %s", code, url.c_str());
        if (http) http->Close();
        return Result::Failed;
    }
    std::string ctype = http->GetResponseHeader("Content-Type");
    if (ctype.empty()) ctype = http->GetResponseHeader("content-type");
    bool aac = IsAac(st.codec) || IsAac(ctype);
    ESP_LOGI(TAG, "streaming %s, content-type '%s', %s", st.name.c_str(), ctype.c_str(), aac ? "AAC" : "MP3");

    // ---- декодер ----
    esp_audio_simple_dec_cfg_t dec_cfg = {};
    dec_cfg.dec_type = aac ? ESP_AUDIO_SIMPLE_DEC_TYPE_AAC : ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    esp_audio_simple_dec_handle_t dec = nullptr;
    if (esp_audio_simple_dec_open(&dec_cfg, &dec) != ESP_AUDIO_ERR_OK || !dec) {
        ESP_LOGE(TAG, "decoder open failed");
        http->Close();
        return Result::Failed;
    }

    auto* in_buf = (uint8_t*)heap_caps_malloc(kInBufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t out_cap = 4608 * 2;
    auto* out_buf = (uint8_t*)heap_caps_malloc(out_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_ae_rate_cvt_handle_t resampler = nullptr;
    int src_rate = 0, channels = 0;
    bool got_info = false;
    Result result = Result::Ended;
    int64_t total = 0;

    while (in_buf && out_buf) {
        if (!active_ || generation_ != gen) { result = Result::Stopped; break; }
        if (app.GetDeviceState() != kDeviceStateIdle) { result = Result::Paused; break; }

        int n = http->Read((char*)in_buf, kInBufSize);
        if (n <= 0) {
            ESP_LOGW(TAG, "stream ended (%d), %lld bytes", n, total);
            result = total > 64 * 1024 ? Result::Ended : Result::Failed;
            if (total > 64 * 1024) result = Result::Paused;   // довго грало — просто перепідключимось
            break;
        }
        total += n;

        esp_audio_simple_dec_raw_t raw = {};
        raw.buffer = in_buf;
        raw.len = (uint32_t)n;
        raw.eos = false;
        bool fatal = false;
        while (raw.len > 0) {
            esp_audio_simple_dec_out_t out = {};
            out.buffer = out_buf;
            out.len = out_cap;
            auto ret = esp_audio_simple_dec_process(dec, &raw, &out);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                auto* nb = (uint8_t*)heap_caps_realloc(out_buf, out.needed_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!nb) { fatal = true; break; }
                out_buf = nb;
                out_cap = out.needed_size;
                continue;
            }
            if (ret != ESP_AUDIO_ERR_OK) {
                // пошкоджений кадр — пропускаємо цей шматок і йдемо далі
                ESP_LOGD(TAG, "decode error %d", ret);
                break;
            }
            if (out.decoded_size > 0) {
                if (!got_info) {
                    esp_audio_simple_dec_info_t info = {};
                    esp_audio_simple_dec_get_info(dec, &info);
                    src_rate = info.sample_rate;
                    channels = info.channel ? info.channel : 1;
                    got_info = true;
                    ESP_LOGI(TAG, "audio %d Hz, %d ch, %d bit", src_rate, channels, info.bits_per_sample);
                    if (src_rate != out_rate) {
                        esp_ae_rate_cvt_cfg_t cfg = {};
                        cfg.src_rate = (uint32_t)src_rate;
                        cfg.dest_rate = (uint32_t)out_rate;
                        cfg.channel = 1;
                        cfg.bits_per_sample = ESP_AUDIO_BIT16;
                        cfg.complexity = 2;
                        cfg.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
                        esp_ae_rate_cvt_open(&cfg, &resampler);
                    }
                }
                // стерео → моно
                auto* s16 = (int16_t*)out_buf;
                int frames = out.decoded_size / sizeof(int16_t) / channels;
                std::vector<int16_t> mono(frames);
                for (int i = 0; i < frames; i++) {
                    int acc = 0;
                    for (int c = 0; c < channels; c++) acc += s16[i * channels + c];
                    mono[i] = (int16_t)(acc / channels);
                }
                std::vector<int16_t> pcm;
                if (resampler) {
                    uint32_t max_out = 0;
                    esp_ae_rate_cvt_get_max_out_sample_num(resampler, frames, &max_out);
                    pcm.resize(max_out);
                    uint32_t actual = max_out;
                    esp_ae_rate_cvt_process(resampler, (esp_ae_sample_t)mono.data(), frames,
                                            (esp_ae_sample_t)pcm.data(), &actual);
                    pcm.resize(actual);
                } else {
                    pcm = std::move(mono);
                }
                // черга відтворення блокує, доки є місце, — так тримаємо темп потоку
                while (!app.GetAudioService().PushPcmToPlaybackQueue(std::move(pcm), 500)) {
                    if (!active_ || generation_ != gen || app.GetDeviceState() != kDeviceStateIdle) break;
                }
            }
            if (raw.consumed == 0 && out.decoded_size == 0) break;   // потрібні нові дані
            raw.len -= raw.consumed;
            raw.buffer += raw.consumed;
        }
        if (fatal) { result = Result::Failed; break; }
    }

    if (resampler) esp_ae_rate_cvt_close(resampler);
    esp_audio_simple_dec_close(dec);
    if (in_buf) heap_caps_free(in_buf);
    if (out_buf) heap_caps_free(out_buf);
    http->Close();
    ESP_LOGI(TAG, "stream closed (%d)", (int)result);
    return result;
}

void RadioPlayer::ArmResume() {
    if (!active_) return;
    reply_done_ = false;
    arm_time_us_ = esp_timer_get_time();
    resume_armed_ = true;
}

void RadioPlayer::Init() {
    Application::GetInstance().GetStateMachine().AddStateChangeListener([this](DeviceState from, DeviceState to) {
        if (resume_armed_ && from == kDeviceStateSpeaking && to == kDeviceStateListening) reply_done_ = true;
    });
}

// Хук із McpServer: будь-яка команда асистенту (гучність, екран, станція…) під час радіо —
// після відповіді повертаємо музику. «Вимкни радіо» робить active_=false, тож розмова не обривається.
void OnMcpToolCalled(const std::string& tool_name) {
    RadioPlayer::GetInstance().ArmResume();
}
