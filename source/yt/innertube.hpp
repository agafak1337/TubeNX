// yt/innertube.hpp — клиент InnerTube (внутренний API YouTube), без API-ключей.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tnx::yt {

struct VideoItem {
    std::string id;            // videoId
    std::string title;
    std::string channel;
    std::string channelId;
    std::string meta;          // "1,2 млн просмотров • 3 дня назад"
    std::string duration;      // "12:34" или "LIVE"
    std::string thumbnailUrl;
    int64_t viewCount = 0;
    int lengthSeconds = 0;
    bool live = false;
};

struct Section {
    std::string title;
    std::string subtitle;
    std::vector<VideoItem> items;
    bool chips = false;        // ряд фильтров
    std::vector<std::string> chipLabels;
};

struct StreamFormat {
    int itag = 0;
    std::string url;
    std::string mime;
    std::string qualityLabel;   // "720p"
    std::string audioQuality;   // "AUDIO_QUALITY_MEDIUM"
    std::string codecs;
    int64_t contentLength = 0;
    int bitrate = 0;
    int width = 0, height = 0;
    bool hasVideo = false, hasAudio = false;
};

struct VideoDetails {
    std::string id, title, description, channel, channelId, channelThumb;
    std::string thumbnailUrl, publishedText, viewText, likeText, shareUrl;
    int64_t viewCount = 0;
    int lengthSeconds = 0;
    std::vector<StreamFormat> formats;
    std::vector<VideoItem> related;
};

struct Feed {
    std::vector<Section> sections;
    std::string continuation;
    bool ok = false;
    std::string error;
};

class InnerTube {
public:
    bool init(const std::string& region = "RU", const std::string& hl = "ru");

    Feed home();                                        // главная (browse FEwhat_to_watch)
    Feed trending();                                    // в тренде
    Feed search(const std::string& q, const std::string& continuation = "");
    Feed related(const std::string& videoId);
    Feed channel(const std::string& channelId);
    std::vector<std::string> suggestions(const std::string& q);
    bool video(const std::string& id, VideoDetails& out);
    Feed continueFeed(const std::string& continuation);

    // Выбор потоков. preferHeight — желаемая высота адаптивного видео
    // (720 по умолчанию: 1080p на 4x A57 в software-декодере не тянет).
    static bool pickStreams(const VideoDetails& v, StreamFormat* muxed, StreamFormat* video,
                            StreamFormat* audio, int preferHeight = 720);

    const std::string& error() const { return error_; }
    void setClientVersion(int idx);   // 0 android_vr, 1 ios, 2 web
    int clientVersion() const { return clientVer_; }

private:
    std::string region_ = "RU", hl_ = "ru", visitorData_, apiKey_, error_;
    int clientVer_ = 0;
    std::string player(const std::string& videoId);
    Feed parseFeed(const std::string& json, bool isSearch);
    bool ensureSession();
};

std::string formatViews(int64_t n);
std::string formatDuration(int seconds);
std::string thumbUrl(const std::string& videoId, int quality = 2);   // 0=default 1=mq 2=hq 3=sd

} // namespace tnx::yt