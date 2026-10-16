#pragma once

#include <curl/curl.h>
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

  ~CurlConnectionCache() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (curl_) {
      curl_easy_cleanup(curl_);
    }
  }

  Lease Acquire(CurlConnectionIdentity identity) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!curl_ || identity != identity_) {
      if (curl_) {
        curl_easy_cleanup(curl_);
      }
      curl_ = curl_easy_init();
      identity_ = std::move(identity);
    }
    if (curl_) {
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
