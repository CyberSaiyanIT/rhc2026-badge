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
 * Download a file only when the server says it changed since @a ifModifiedSince. An unchanged file
 * costs one request and no write, the wear-levelled partition being slower than the transfer.
 *
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

/**
 * Whether at least one download() is currently transferring.
 * Safe to call from any task.
 */
bool isDownloading();

}
