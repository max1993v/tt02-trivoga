#ifndef _RADIO_PLAYER_H_
#define _RADIO_PLAYER_H_

// Інтернет-радіо для TT-02.
// Пошук станцій — безкоштовний каталог Radio Browser (radio-browser.info), без ключа.
// Потік MP3 / AAC декодується на пристрої (esp_audio_codec) і грає через ту саму чергу,
// що й голос асистента. Під час розмови радіо стає на паузу, після відповіді — продовжує.

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

class RadioPlayer {
public:
    static RadioPlayer& GetInstance();
    void Init();                         // підписка на зміни стану (викликати при старті плати)

    // Знайти станцію й увімкнути. Повертає текст для асистента (що саме грає або що не знайдено).
    std::string Play(const std::string& query, bool genre);
    void Stop();
    void Toggle();                       // вимкнути / увімкнути останню станцію
    bool IsActive() const { return active_; }
    void ArmResume();                    // після виконаної команди повернути музику
    std::string NowPlaying();            // назва станції / «пошук…» / помилка, або ""

private:
    RadioPlayer() = default;

    struct Station {
        std::string name;
        std::string url;
        std::string codec;
        int bitrate = 0;
    };

    bool Lookup(const std::string& query, bool genre, std::vector<Station>& out);
    bool SearchApi(const std::string& path, std::vector<Station>& out);
    void EnsureTask();
    static void TaskEntry(void* arg);
    void Loop();
    enum class Result { Playing, Paused, Stopped, Failed };
    Result StreamOnce(const Station& st, uint32_t gen);
    void SaveLast();
    void LoadLast();

    struct Request { std::string query; bool genre = false; };
    bool pending_ = false;
    Request request_;
    std::string error_;
    int64_t error_until_us_ = 0;
    std::mutex mutex_;
    Station station_;
    std::vector<Station> candidates_;
    int cand_idx_ = 0;
    std::atomic<bool> active_{false};
    std::atomic<uint32_t> generation_{0};
    std::atomic<bool> resume_armed_{false};
    std::atomic<bool> reply_done_{false};
    std::atomic<int64_t> arm_time_us_{0};
    bool task_started_ = false;
    bool loaded_ = false;
};

#endif
