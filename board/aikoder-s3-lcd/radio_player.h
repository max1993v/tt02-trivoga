#ifndef _RADIO_PLAYER_H_
#define _RADIO_PLAYER_H_

// Інтернет-радіо для TT-02.
// Пошук станцій — безкоштовний каталог Radio Browser (radio-browser.info), без ключа.
// Потік MP3 / AAC декодується на пристрої (esp_audio_codec) і грає через ту саму чергу,
// що й голос асистента. Під час розмови радіо стає на паузу, після відповіді — продовжує.

#include <atomic>
#include <mutex>
#include <string>

class RadioPlayer {
public:
    static RadioPlayer& GetInstance();

    // Знайти станцію й увімкнути. Повертає текст для асистента (що саме грає або що не знайдено).
    std::string Play(const std::string& query, bool genre);
    void Stop();
    void Toggle();                       // вимкнути / увімкнути останню станцію
    bool IsActive() const { return active_; }
    std::string NowPlaying();            // назва станції або ""

private:
    RadioPlayer() = default;

    struct Station {
        std::string name;
        std::string url;
        std::string codec;
        int bitrate = 0;
    };

    bool Lookup(const std::string& query, bool genre, Station& out);
    bool SearchApi(const std::string& path, Station& out);
    void EnsureTask();
    static void TaskEntry(void* arg);
    void Loop();
    enum class Result { Paused, Stopped, Failed, Ended };
    Result StreamOnce(const Station& st, uint32_t gen);
    void SaveLast();
    void LoadLast();

    std::mutex mutex_;
    Station station_;
    std::atomic<bool> active_{false};
    std::atomic<uint32_t> generation_{0};
    bool task_started_ = false;
    bool loaded_ = false;
};

#endif
