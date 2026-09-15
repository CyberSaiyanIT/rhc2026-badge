#include <Tactility/Tactility.h>
#include <Tactility/file/File.h>
#include <Tactility/network/Http.h>

#include <tactility/log.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <vector>

#ifdef ESP_PLATFORM
#include <Tactility/network/EspHttpClient.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <miniz.h>
#include <esp_http_client.h>
#include <strings.h>
#endif

namespace tt::network::http {

constexpr auto* TAG = "HTTP";

namespace {

std::atomic<int> activeDownloads { 0 };

/**
 * Scoped rather than set and cleared by hand: download() returns from a dozen places, and a count
 * left behind by one of them would inhibit the screensaver permanently.
 */
struct DownloadScope {
    DownloadScope() { activeDownloads.fetch_add(1, std::memory_order_relaxed); }
    ~DownloadScope() { activeDownloads.fetch_sub(1, std::memory_order_relaxed); }
};

}

bool isDownloading() {
    return activeDownloads.load(std::memory_order_relaxed) > 0;
}

#ifdef ESP_PLATFORM

/** Bytes pulled from the socket per read. Deliberately small, see STAGING_BUFFER_SIZE. */
constexpr int READ_SIZE = 512;

/**
 * How much is staged before it reaches the filesystem. Writing each 512-byte read straight through
 * turns a 237 kB download into ~3.7 MB of flash traffic. A multiple of the flash sector; read size
 * is not raised with it, being what paces the loop against the NeoPixel render task.
 */
constexpr size_t STAGING_BUFFER_SIZE = 32768;
constexpr size_t FLASH_SECTOR_SIZE = 4096;

/**
 * Strips the gzip framing tinfl does not handle, speaking raw deflate and zlib rather than RFC
 * 1952. Fed byte by byte: the header can span reads and its optional fields are NUL-terminated.
 */
class GzipHeaderParser {
    enum class Field : uint8_t { Fixed, Extra, ExtraData, Name, Comment, Crc, Done, Failed };

    Field field = Field::Fixed;
    uint8_t flags = 0;
    uint16_t remaining = 10; // the fixed header, then whatever the flags select
    uint16_t extraLength = 0;
    uint8_t fixedSeen = 0;

public:
    bool failed() const { return field == Field::Failed; }
    bool done() const { return field == Field::Done; }

    /** @return bytes consumed from @a data; stops as soon as the header ends. */
    size_t consume(const uint8_t* data, size_t size) {
        size_t used = 0;
        while (used < size && field != Field::Done && field != Field::Failed) {
            const uint8_t byte = data[used++];
            switch (field) {
                case Field::Fixed:
                    if ((fixedSeen == 0 && byte != 0x1F) || (fixedSeen == 1 && byte != 0x8B) ||
                        (fixedSeen == 2 && byte != 0x08)) { // magic, then deflate as the method
                        field = Field::Failed;
                        break;
                    }
                    if (fixedSeen == 3) {
                        flags = byte;
                    }
                    fixedSeen++;
                    if (--remaining == 0) {
                        advance();
                    }
                    break;
                case Field::Extra:
                    extraLength = (remaining == 2) ? byte : static_cast<uint16_t>(extraLength | (byte << 8));
                    if (--remaining == 0) {
                        remaining = extraLength;
                        field = Field::ExtraData;
                        if (remaining == 0) {
                            flags &= ~GZIP_FLAG_EXTRA;
                            advance();
                        }
                    }
                    break;
                case Field::ExtraData:
                    if (--remaining == 0) {
                        flags &= ~GZIP_FLAG_EXTRA;
                        advance();
                    }
                    break;
                case Field::Name:
                    if (byte == 0) {
                        flags &= ~GZIP_FLAG_NAME;
                        advance();
                    }
                    break;
                case Field::Comment:
                    if (byte == 0) {
                        flags &= ~GZIP_FLAG_COMMENT;
                        advance();
                    }
                    break;
                case Field::Crc:
                    if (--remaining == 0) {
                        flags &= ~GZIP_FLAG_CRC;
                        advance();
                    }
                    break;
                default:
                    break;
            }
        }
        return used;
    }

private:
    static constexpr uint8_t GZIP_FLAG_CRC = 0x02;
    static constexpr uint8_t GZIP_FLAG_EXTRA = 0x04;
    static constexpr uint8_t GZIP_FLAG_NAME = 0x08;
    static constexpr uint8_t GZIP_FLAG_COMMENT = 0x10;

    /** Optional fields appear in flag order, so each one picks the next flag still set. */
    void advance() {
        if (flags & GZIP_FLAG_EXTRA) {
            field = Field::Extra;
            remaining = 2;
        } else if (flags & GZIP_FLAG_NAME) {
            field = Field::Name;
        } else if (flags & GZIP_FLAG_COMMENT) {
            field = Field::Comment;
        } else if (flags & GZIP_FLAG_CRC) {
            field = Field::Crc;
            remaining = 2;
        } else {
            field = Field::Done;
        }
    }
};

namespace {

/** Response headers are only delivered through this callback; the getters read the request's. */
struct ResponseInfo {
    bool gzipped = false;
    /** Echoed back as If-Modified-Since on the next check, so an unchanged file costs one 304. */
    std::string lastModified;
};

esp_err_t onHttpEvent(esp_http_client_event_t* event) {
    if (event->event_id != HTTP_EVENT_ON_HEADER || event->user_data == nullptr ||
        event->header_key == nullptr || event->header_value == nullptr) {
        return ESP_OK;
    }
    auto* info = static_cast<ResponseInfo*>(event->user_data);
    if (strcasecmp(event->header_key, "Content-Encoding") == 0) {
        info->gzipped = strcasecmp(event->header_value, "gzip") == 0;
    } else if (strcasecmp(event->header_key, "Last-Modified") == 0) {
        info->lastModified = event->header_value;
    }
    return ESP_OK;
}

/** Returns false to abort the transfer; the receiver then reports "Failed to store data". */
using BodySink = std::function<bool(const uint8_t* data, size_t size)>;

/**
 * Streams the response body to @a sink, inflating it when the server gzipped it.
 * @return nullptr on success, otherwise the error to report.
 */
const char* receiveBody(
    EspHttpClient& client,
    bool isGzipped,
    int bytesLeft,
    bool isChunked,
    int64_t& readUs,
    size_t& totalBytes,
    const BodySink& sink
) {
    std::unique_ptr<char, decltype(&heap_caps_free)> read_buffer(
        static_cast<char*>(heap_caps_malloc_prefer(
            READ_SIZE, 2,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        )),
        &heap_caps_free
    );
    if (read_buffer == nullptr) {
        return "Failed to allocate download buffer";
    }

    // tinfl is in ROM on ESP32, so only its state and sliding window cost anything.
    std::unique_ptr<tinfl_decompressor, decltype(&heap_caps_free)> decompressor(nullptr, &heap_caps_free);
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> dictionary(nullptr, &heap_caps_free);
    size_t dictionary_offset = 0;
    GzipHeaderParser gzip_header;
    bool inflate_done = false;
    if (isGzipped) {
        decompressor.reset(static_cast<tinfl_decompressor*>(heap_caps_malloc_prefer(
            sizeof(tinfl_decompressor), 2,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        )));
        dictionary.reset(static_cast<uint8_t*>(heap_caps_malloc_prefer(
            TINFL_LZ_DICT_SIZE, 2,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        )));
        if (decompressor == nullptr || dictionary == nullptr) {
            return "Failed to allocate decompressor";
        }
        tinfl_init(decompressor.get());
    }

    while (isChunked || bytesLeft > 0) {
        const int64_t read_started_us = esp_timer_get_time();
        int data_read = client.read(read_buffer.get(), READ_SIZE);
        readUs += esp_timer_get_time() - read_started_us;
        if (data_read < 0) {
            return "Failed to read data";
        }
        if (data_read == 0) {
            break;
        }
        if (!isChunked) {
            bytesLeft -= data_read;
        }
        totalBytes += data_read;

        const auto* input = reinterpret_cast<const uint8_t*>(read_buffer.get());
        size_t available = data_read;

        if (!isGzipped) {
            if (!sink(input, available)) {
                return "Failed to store data";
            }
            taskYIELD();
            continue;
        }

        if (!gzip_header.done()) {
            const size_t used = gzip_header.consume(input, available);
            input += used;
            available -= used;
            if (gzip_header.failed()) {
                return "Response is not gzip";
            }
        }

        size_t offset = 0;
        while (offset < available && !inflate_done) {
            size_t in_size = available - offset;
            size_t out_size = TINFL_LZ_DICT_SIZE - dictionary_offset;
            const tinfl_status status = tinfl_decompress(
                decompressor.get(),
                input + offset, &in_size,
                dictionary.get(), dictionary.get() + dictionary_offset, &out_size,
                TINFL_FLAG_HAS_MORE_INPUT
            );
            offset += in_size;
            if (!sink(dictionary.get() + dictionary_offset, out_size)) {
                return "Failed to store data";
            }
            dictionary_offset = (dictionary_offset + out_size) & (TINFL_LZ_DICT_SIZE - 1);
            if (status == TINFL_STATUS_DONE) {
                inflate_done = true;
            } else if (status < TINFL_STATUS_DONE) {
                LOG_E(TAG, "Inflate failed (status=%d)", (int) status);
                return "Failed to decompress data";
            }
        }
        taskYIELD();
    }

    if (isGzipped && !inflate_done) {
        return "Compressed data ended early";
    }
    if (!isChunked && bytesLeft > 0) {
        return "Connection closed before all data was received";
    }
    return nullptr;
}

/** Shared request setup; @a outInfo must outlive the client. */
std::unique_ptr<esp_http_client_config_t> makeConfig(const std::string& url, const char* certificate, size_t certificateLength, ResponseInfo& outInfo) {
    auto config = std::make_unique<esp_http_client_config_t>();
    memset(config.get(), 0, sizeof(esp_http_client_config_t));
    config->url = url.c_str();
    config->auth_type = HTTP_AUTH_TYPE_NONE;
    config->tls_version = ESP_HTTP_CLIENT_TLS_VER_TLS_1_2;
    config->method = HTTP_METHOD_GET;
    config->timeout_ms = 30000;
    config->transport_type = HTTP_TRANSPORT_OVER_SSL;
    config->event_handler = onHttpEvent;
    config->user_data = &outInfo;
    if (certificate != nullptr) {
        config->cert_pem = certificate;
        config->cert_len = certificateLength;
    } else {
        config->crt_bundle_attach = esp_crt_bundle_attach;
    }
    return config;
}

/** Outcome of one file download, so both public entry points can share the transfer. */
struct FileDownloadResult {
    const char* error = nullptr;
    bool notModified = false;
    std::string lastModified;
};

/** Runs on the dispatcher. @a ifModifiedSince empty means "always fetch". */
FileDownloadResult downloadToFile(
    const std::string& url,
    const std::string& certFilePath,
    const std::string& downloadFilePath,
    const std::string& ifModifiedSince
) {
    FileDownloadResult result;

    std::shared_ptr<uint8_t[]> certificate = nullptr;
    if (!certFilePath.empty()) {
        certificate = file::readString(certFilePath);
        if (certificate == nullptr) {
            result.error = "Failed to read certificate";
            return result;
        }
    }

    ResponseInfo response_info;
    auto config = makeConfig(
        url,
        certificate ? reinterpret_cast<const char*>(certificate.get()) : nullptr,
        certificate ? strlen(reinterpret_cast<const char*>(certificate.get())) + 1 : 0,
        response_info
    );

    auto client = std::make_unique<EspHttpClient>();
    if (!client->init(std::move(config))) {
        result.error = "Failed to initialize client";
        return result;
    }

    // Offered, not required: the response is inflated only if the server says it used it.
    client->setHeader("Accept-Encoding", "gzip");
    if (!ifModifiedSince.empty()) {
        client->setHeader("If-Modified-Since", ifModifiedSince.c_str());
    }

    if (!client->open()) {
        result.error = "Failed to open connection";
        return result;
    }
    if (!client->fetchHeaders()) {
        result.error = "Failed to get request headers";
        return result;
    }

    if (client->getStatusCode() == 304) {
        result.notModified = true;
        return result;
    }
    if (!client->isStatusCodeOk()) {
        result.error = "Server response is not OK";
        return result;
    }
    result.lastModified = response_info.lastModified;

    // Content-Length, when present, counts what is on the wire, so it stays the read budget
    // whether or not the body is compressed.
    const auto bytes_left = client->getContentLength();
    const bool is_chunked = (bytes_left <= 0);

    // Prefers PSRAM: a staging buffer is a memcpy target, never DMA, and internal RAM is the
    // scarce pool on these boards. Falls back to internal for devices without PSRAM.
    std::unique_ptr<char, decltype(&heap_caps_free)> buffer(
        static_cast<char*>(heap_caps_malloc_prefer(
            STAGING_BUFFER_SIZE, 2,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        )),
        &heap_caps_free
    );
    if (buffer == nullptr) {
        result.error = "Failed to allocate download buffer";
        return result;
    }

    // Unlocked: file::findLock() is deprecated in favour of a TactilityKernel file mutex that
    // does not exist yet, so nothing in this project holds a lock across stdio calls.
    auto* file = fopen(downloadFilePath.c_str(), "wb");
    if (file == nullptr) {
        result.error = "Failed to open file";
        return result;
    }

    int64_t read_us = 0;
    int64_t write_us = 0;
    int commits = 0;
    size_t total_bytes = 0;   // off the wire
    size_t written_bytes = 0; // into the file, which gzip makes larger than total_bytes
    size_t staged = 0;
    const int64_t started_us = esp_timer_get_time();

    /**
     * Commits whole flash sectors and keeps the tail, so every write but the last lands on a 4 kB
     * boundary; @a flush writes the tail too. The delay exists because a flash erase suspends the
     * cache on both cores, which otherwise costs the NeoPixel task its 25 ms frame deadline.
     */
    auto commitStaged = [&](bool flush) {
        size_t commit_size = flush ? staged : staged - (staged % FLASH_SECTOR_SIZE);
        if (commit_size == 0) {
            return true;
        }
        const int64_t write_started_us = esp_timer_get_time();
        const size_t written = fwrite(buffer.get(), 1, commit_size, file);
        write_us += esp_timer_get_time() - write_started_us;
        commits++;
        if (written != commit_size) {
            return false;
        }
        staged -= commit_size;
        if (staged > 0) {
            memmove(buffer.get(), buffer.get() + commit_size, staged);
        }
        vTaskDelay(1);
        return true;
    };

    const char* error = receiveBody(
        *client, response_info.gzipped, bytes_left, is_chunked, read_us, total_bytes,
        [&](const uint8_t* data, size_t size) {
            while (size > 0) {
                const size_t take = std::min(STAGING_BUFFER_SIZE - staged, size);
                memcpy(buffer.get() + staged, data, take);
                staged += take;
                data += take;
                size -= take;
                written_bytes += take;
                if (STAGING_BUFFER_SIZE - staged < READ_SIZE && !commitStaged(false)) {
                    return false;
                }
            }
            return true;
        }
    );
    if (error == nullptr && !commitStaged(true)) {
        error = "Failed to store data";
    }
    if (error != nullptr) {
        fclose(file);
        result.error = error;
        return result;
    }

    // Timed separately: FATFS flushes its sector cache, the FAT and the directory entry here,
    // which on the wear-levelled partition costs more than a data write of the same size.
    const int64_t close_started_us = esp_timer_get_time();
    fclose(file);
    const int64_t close_us = esp_timer_get_time() - close_started_us;

    const int64_t total_us = esp_timer_get_time() - started_us;
    LOG_I(
        TAG,
        "Downloaded %s to %s: %u bytes%s in %d ms (network %d ms, flash %d ms in %d writes, close %d ms, %d KB/s)",
        url.c_str(),
        downloadFilePath.c_str(),
        (unsigned) written_bytes,
        response_info.gzipped ? " (gzip)" : "",
        (int) (total_us / 1000),
        (int) (read_us / 1000),
        (int) (write_us / 1000),
        commits,
        (int) (close_us / 1000),
        total_us > 0 ? (int) ((int64_t) written_bytes * 1000000 / total_us / 1024) : 0
    );
    return result;
}

} // namespace

#endif

void download(
    const std::string& url,
    const std::string& certFilePath,
    const std::string &downloadFilePath,
    const std::function<void()>& onSuccess,
    const std::function<void(const char* errorMessage)>& onError
) {
    LOG_I(TAG, "Downloading from %s to %s", url.c_str(), downloadFilePath.c_str());
#ifdef ESP_PLATFORM
    getMainDispatcher().dispatch([url, certFilePath, downloadFilePath, onSuccess, onError] {
        DownloadScope scope;
        const auto result = downloadToFile(url, certFilePath, downloadFilePath, "");
        if (result.error != nullptr) {
            onError(result.error);
        } else {
            onSuccess();
        }
    });
#else
    getMainDispatcher().dispatch([onError] {
        onError("Not implemented");
    });
#endif
}

void downloadIfNewer(
    const std::string& url,
    const std::string& certFilePath,
    const std::string& downloadFilePath,
    const std::string& ifModifiedSince,
    const std::function<void(const std::string& lastModified)>& onSuccess,
    const std::function<void()>& onNotModified,
    const std::function<void(const char* errorMessage)>& onError
) {
    LOG_I(TAG, "Downloading from %s to %s if newer than '%s'", url.c_str(), downloadFilePath.c_str(), ifModifiedSince.c_str());
#ifdef ESP_PLATFORM
    getMainDispatcher().dispatch([url, certFilePath, downloadFilePath, ifModifiedSince, onSuccess, onNotModified, onError] {
        DownloadScope scope;
        const auto result = downloadToFile(url, certFilePath, downloadFilePath, ifModifiedSince);
        if (result.notModified) {
            LOG_I(TAG, "%s is unchanged", url.c_str());
            onNotModified();
        } else if (result.error != nullptr) {
            onError(result.error);
        } else {
            onSuccess(result.lastModified);
        }
    });
#else
    getMainDispatcher().dispatch([onError] {
        onError("Not implemented");
    });
#endif
}

void downloadToMemory(
    const std::string& url,
    const std::string& certFilePath,
    const std::function<void(std::shared_ptr<std::vector<uint8_t>> data)>& onSuccess,
    const std::function<void(const char* errorMessage)>& onError
) {
    LOG_I(TAG, "Downloading from %s to memory", url.c_str());
#ifdef ESP_PLATFORM
    getMainDispatcher().dispatch([url, certFilePath, onSuccess, onError] {
        DownloadScope scope;

        std::shared_ptr<uint8_t[]> certificate = nullptr;
        if (!certFilePath.empty()) {
            certificate = file::readString(certFilePath);
            if (certificate == nullptr) {
                onError("Failed to read certificate");
                return;
            }
        }

        ResponseInfo response_info;
        auto config = makeConfig(
            url,
            certificate ? reinterpret_cast<const char*>(certificate.get()) : nullptr,
            certificate ? strlen(reinterpret_cast<const char*>(certificate.get())) + 1 : 0,
            response_info
        );

        auto client = std::make_unique<EspHttpClient>();
        if (!client->init(std::move(config))) {
            onError("Failed to initialize client");
            return;
        }
        client->setHeader("Accept-Encoding", "gzip");
        if (!client->open()) {
            onError("Failed to open connection");
            return;
        }
        if (!client->fetchHeaders()) {
            onError("Failed to get request headers");
            return;
        }
        if (!client->isStatusCodeOk()) {
            onError("Server response is not OK");
            return;
        }

        const auto bytes_left = client->getContentLength();
        const bool is_chunked = (bytes_left <= 0);

        int64_t read_us = 0;
        size_t total_bytes = 0;
        const int64_t started_us = esp_timer_get_time();

        // Large enough that the vector's growth lands in PSRAM rather than churning the internal
        // heap: CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL keeps allocations below 16 kB internal.
        auto data = std::make_shared<std::vector<uint8_t>>();
        data->reserve(64 * 1024);

        const char* error = receiveBody(
            *client, response_info.gzipped, bytes_left, is_chunked, read_us, total_bytes,
            [&](const uint8_t* chunk, size_t size) {
                data->insert(data->end(), chunk, chunk + size);
                return true;
            }
        );
        if (error != nullptr) {
            onError(error);
            return;
        }

        const int64_t total_us = esp_timer_get_time() - started_us;
        LOG_I(
            TAG,
            "Downloaded %s to memory: %u bytes%s from %u on the wire in %d ms (network %d ms)",
            url.c_str(),
            (unsigned) data->size(),
            response_info.gzipped ? " (gzip)" : "",
            (unsigned) total_bytes,
            (int) (total_us / 1000),
            (int) (read_us / 1000)
        );
        onSuccess(data);
    });
#else
    getMainDispatcher().dispatch([onError] {
        onError("Not implemented");
    });
#endif
}

}
