#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tt::network::http {
    /**
     * Download a file from a URL.
     * The server must send the Content-Length header.
     * @param url download source URL
     * @param certFilePath the path to the .pem file
     * @param downloadFilePath The path to downloadd the file to. The parent directories must exist.
     * @param onSuccess the success result callback
     * @param onError the error result callback
     */
    void download(
    const std::string& url,
    const std::string& certFilePath,
    const std::string &downloadFilePath,
    const std::function<void()>& onSuccess,
    const std::function<void(const char* errorMessage)>& onError
);

/**
 * Download a file only when the server says it changed since @a ifModifiedSince.
 * An unchanged file costs one request and no write at all, which matters because writing the
 * wear-levelled data partition is far slower than the transfer.
 * @param ifModifiedSince a Last-Modified value from a previous download, or empty to always fetch
 * @param onSuccess receives the new Last-Modified value to pass back on the next check
 * @param onNotModified the stored file is already current
 * @param onError the error result callback
 */
void downloadIfNewer(
    const std::string& url,
    const std::string& certFilePath,
    const std::string& downloadFilePath,
    const std::string& ifModifiedSince,
    const std::function<void(const std::string& lastModified)>& onSuccess,
    const std::function<void()>& onNotModified,
    const std::function<void(const char* errorMessage)>& onError
);

/**
 * Download into memory instead of a file, so nothing reaches flash that the caller does not
 * choose to keep. Writing the wear-levelled data partition costs far more than the transfer.
 * @param url download source URL
 * @param certFilePath the path to the .pem file, or empty for the default CRT bundle
 * @param onSuccess receives the complete body, already decompressed if the server gzipped it
 * @param onError the error result callback
 */
void downloadToMemory(
    const std::string& url,
    const std::string& certFilePath,
    const std::function<void(std::shared_ptr<std::vector<uint8_t>> data)>& onSuccess,
    const std::function<void(const char* errorMessage)>& onError
);

enum class Method {
    Get,
    Post
};

/**
 * A complete response, including the ones the download helpers reject outright.
 * @a body is empty when the server sent none, and is never NUL-terminated.
 */
struct Response {
    int statusCode = 0;
    std::vector<uint8_t> body;
};

/**
 * Perform a request and keep the whole response in memory.
 *
 * Unlike downloadToMemory(), a non-2xx status is delivered to @a onSuccess with its body intact
 * rather than collapsed into an error. APIs that report application-level outcomes through status
 * codes need to read that body, and only the caller knows which codes are failures.
 * @a onError is reserved for the request not completing at all.
 *
 * A 3xx is delivered like any other status rather than being followed: this client drives the
 * connection by hand, and esp_http_client only acts on a redirect inside esp_http_client_perform().
 *
 * @param url the full request URL, query string included
 * @param certFilePath the path to the .pem file, or empty for the default CRT bundle
 * @param body the request body, empty for none
 * @param contentType the body's media type, ignored when @a body is empty
 */
void request(
    const std::string& url,
    Method method,
    const std::string& certFilePath,
    const std::string& body,
    const std::string& contentType,
    const std::function<void(const Response& response)>& onSuccess,
    const std::function<void(const char* errorMessage)>& onError
);

/**
 * Whether at least one download() is currently transferring.
 * Safe to call from any task.
 */
bool isDownloading();

}
