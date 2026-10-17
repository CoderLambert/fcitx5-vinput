#pragma once

#include <curl/curl.h>
#include <curl/easy.h>
#include <mutex>
#include <string>
#include <utility>

// One cache per PostProcessor; a lease locks the handle for an entire request.
struct CurlConnectionIdentity {
  std::string endpoint;
  std::string credential;
  std::string proxy_policy;

  bool operator==(const CurlConnectionIdentity&) const = default;
};

class CurlConnectionCache {
public:
  struct Lease {
    std::unique_lock<std::mutex> lock;
    CURL* handle = nullptr;
  };

  CurlConnectionCache() = default;
  CurlConnectionCache(const CurlConnectionCache&) = delete;
  CurlConnectionCache& operator=(const CurlConnectionCache&) = delete;
  // The cache owns a raw CURL handle and a mutex; moving it would leave the
  // source with a dangling handle, so move operations are deleted as well
  // (Rule of 5).
  CurlConnectionCache(CurlConnectionCache&&) = delete;
  CurlConnectionCache& operator=(CurlConnectionCache&&) = delete;

  ~CurlConnectionCache() {
    const std::scoped_lock lock(mutex_);
    if (curl_ != nullptr) {
      curl_easy_cleanup(curl_);
    }
  }

  Lease Acquire(CurlConnectionIdentity identity) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (curl_ == nullptr || identity != identity_) {
      if (curl_ != nullptr) {
        curl_easy_cleanup(curl_);
      }
      curl_ = curl_easy_init();
      identity_ = std::move(identity);
    }
    if (curl_ != nullptr) {
      // Preserve the connection cache while resetting previous request options.
      curl_easy_reset(curl_);
    }
    return {.lock = std::move(lock), .handle = curl_};
  }

private:
  std::mutex mutex_;
  CURL* curl_ = nullptr;
  CurlConnectionIdentity identity_;
};
