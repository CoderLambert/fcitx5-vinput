#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>
#include <curl/easy.h>
#include <curl/system.h>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "daemon/postprocess/curl_connection_cache.h"

namespace {
void Require(bool ok, const char* what) {
  if (!ok) {
    std::cerr << "failed: " << what << '\n';
    std::abort();
  }
}

size_t Write(char* data, size_t size, size_t nmemb, void* context) {
  auto* response = static_cast<std::string*>(context);
  response->append(data, size * nmemb);
  return size * nmemb;
}

// Mirrors the production body provider in post_processor.cpp: a read
// callback with deliberately no seek callback, so libcurl cannot rewind
// and automatically resend a POST.
struct BodyReader {
  std::string_view body;
  std::size_t offset = 0;
};

size_t ReadBody(char* buffer, size_t size, size_t nmemb, void* userdata) {
  auto* reader = static_cast<BodyReader*>(userdata);
  if (reader == nullptr) {
    return CURL_READFUNC_ABORT;
  }
  const std::size_t remaining = reader->body.size() - reader->offset;
  const std::size_t to_copy = std::min(remaining, size * nmemb);
  if (to_copy > 0) {
    std::memcpy(buffer, reader->body.data() + reader->offset, to_copy);
    reader->offset += to_copy;
  }
  return to_copy;
}

int AbortProgress(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  return 1;
}

enum class ServerMode : std::uint8_t { KeepAlive, CloseEach, DropSecondBody };

struct HttpFixture {
  ServerMode mode;
  int listener;
  std::uint16_t port = 0;
  std::thread worker;
  std::atomic<int> connections{0};
  std::atomic<bool> stop{false};
  std::vector<std::string> auth;
  std::vector<std::string> bodies;

  explicit HttpFixture(ServerMode server_mode)
      : mode(server_mode), listener(socket(AF_INET, SOCK_STREAM, 0)) {
    Require(listener >= 0, "socket");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Require(bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "bind");
    socklen_t len = sizeof(addr);
    Require(getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len) == 0, "getsockname");
    port = ntohs(addr.sin_port);
    Require(listen(listener, 8) == 0, "listen");
    worker = std::thread([this] { Run(); });
  }

  HttpFixture(const HttpFixture&) = delete;
  HttpFixture& operator=(const HttpFixture&) = delete;
  HttpFixture(HttpFixture&&) = delete;
  HttpFixture& operator=(HttpFixture&&) = delete;

  ~HttpFixture() {
    Stop();
    close(listener);
  }

  void Stop() {
    stop.store(true);
    if (worker.joinable()) {
      worker.join();
    }
  }

  bool ReadRequest(int client, std::string& body, std::string& head) const {
    std::string buffer;
    while (!stop.load()) {
      const std::size_t head_end = buffer.find("\r\n\r\n");
      if (head_end != std::string::npos) {
        const std::string length_key = "Content-Length: ";
        const auto len_pos = buffer.find(length_key);
        const std::size_t body_size = len_pos == std::string::npos
                                          ? 0
                                          : std::stoul(buffer.substr(len_pos + length_key.size()));
        if (buffer.size() >= head_end + 4 + body_size) {
          head = buffer.substr(0, head_end);
          body = buffer.substr(head_end + 4, body_size);
          return true;
        }
      }
      pollfd pfd{.fd = client, .events = POLLIN, .revents = 0};
      if (poll(&pfd, 1, 100) <= 0) {
        continue;
      }
      char chunk[4096];
      const ssize_t n = recv(client, chunk, sizeof(chunk), 0);
      if (n <= 0) {
        return false;
      }
      buffer.append(chunk, static_cast<std::size_t>(n));
    }
    return false;
  }

  void Run() {
    while (!stop.load()) {
      pollfd pfd{.fd = listener, .events = POLLIN, .revents = 0};
      if (poll(&pfd, 1, 100) <= 0) {
        continue;
      }
      const int client = accept(listener, nullptr, nullptr);
      if (client < 0) {
        continue;
      }
      ++connections;
      while (!stop.load()) {
        std::string body;
        std::string head;
        if (!ReadRequest(client, body, head)) {
          break;
        }
        const std::size_t auth_pos = head.find("Authorization: ");
        const std::size_t auth_end = head.find("\r\n", auth_pos);
        auth.push_back(auth_pos == std::string::npos ? ""
                                                     : head.substr(auth_pos, auth_end - auth_pos));
        bodies.push_back(body);
        if (mode == ServerMode::DropSecondBody && bodies.size() == 2) {
          break; // Close without any response: the drop scenario.
        }
        const std::string answer =
            mode == ServerMode::CloseEach
                ? "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK"
                : "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nOK";
        if (send(client, answer.data(), answer.size(), MSG_NOSIGNAL) !=
            static_cast<ssize_t>(answer.size())) {
          break;
        }
        if (mode == ServerMode::CloseEach) {
          break;
        }
        // Keep-alive: wait briefly for the next request on this connection.
        pollfd next{.fd = client, .events = POLLIN, .revents = 0};
        if (poll(&next, 1, 300) <= 0) {
          break;
        }
      }
      close(client);
    }
  }
};

struct Metrics {
  CURLcode code = CURLE_OK;
  long num_connects = 0;
  double connect_ms = 0.0;
  double total_ms = 0.0;
  std::string response;
};

Metrics Request(CurlConnectionCache& cache, const std::string& url, const std::string& secret,
                const std::string& policy, const std::string& body, bool abort = false) {
  auto lease = cache.Acquire(
      CurlConnectionIdentity{.endpoint = url, .credential = secret, .proxy_policy = policy});
  Require(lease.handle != nullptr, "easy handle");
  CURL* curl = lease.handle;
  curl_slist* headers = nullptr;
  const std::string authorization = "Authorization: Bearer " + secret;
  headers = curl_slist_append(headers, authorization.c_str());
  std::string response;
  BodyReader reader{.body = body, .offset = 0};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_READFUNCTION, ReadBody);
  curl_easy_setopt(curl, CURLOPT_READDATA, &reader);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3000L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  if (abort) {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, AbortProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, nullptr);
  }
  // Keep the loopback fixture independent of CI proxy environment variables.
  curl_easy_setopt(curl, CURLOPT_PROXY, "");
  Metrics metrics;
  metrics.code = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &metrics.num_connects);
  double connect_s = 0.0;
  double total_s = 0.0;
  curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &connect_s);
  curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total_s);
  metrics.connect_ms = connect_s * 1000.0;
  metrics.total_ms = total_s * 1000.0;
  metrics.response = response;
  curl_easy_reset(curl); // Detach per-request memory before destroying locals.
  curl_slist_free_all(headers);
  return metrics;
}

std::string Url(const HttpFixture& fixture) {
  return "http://127.0.0.1:" + std::to_string(fixture.port) + "/chat";
}

void TestIsolationAcrossIdentityChanges() {
  HttpFixture fixture(ServerMode::KeepAlive);
  CurlConnectionCache cache;
  const std::string url = Url(fixture);
  Require(Request(cache, url, "A", "proxy1", "one").code == CURLE_OK, "request one");
  Require(Request(cache, url, "A", "proxy1", "two").code == CURLE_OK, "request two");
  Require(Request(cache, url, "B", "proxy1", "three").code == CURLE_OK, "request three");
  Require(Request(cache, url, "B", "proxy2", "four").code == CURLE_OK, "request four");
  fixture.Stop();
  Require(fixture.connections == 3, "one connection reused, two credentials/policy isolated");
  Require(fixture.auth ==
              std::vector<std::string>({"Authorization: Bearer A", "Authorization: Bearer A",
                                        "Authorization: Bearer B", "Authorization: Bearer B"}),
          "auth isolated");
  Require(fixture.bodies == std::vector<std::string>({"one", "two", "three", "four"}),
          "request content isolated");
  std::cout << "Four HTTP POSTs over three connections; isolation verified\n";
}

void TestTenConsecutiveRequestsReuseOneConnection() {
  HttpFixture fixture(ServerMode::KeepAlive);
  CurlConnectionCache cache;
  const std::string url = Url(fixture);
  long total_new_connections = 0;
  for (int i = 0; i < 10; ++i) {
    const Metrics metrics = Request(cache, url, "A", "proxy1", "req-" + std::to_string(i));
    Require(metrics.code == CURLE_OK, "ten-request workload succeeds");
    Require(metrics.response == "OK", "ten-request response");
    std::cout << "workload req " << i << ": NUM_CONNECTS=" << metrics.num_connects
              << " connect_ms=" << metrics.connect_ms << " total_ms=" << metrics.total_ms << '\n';
    total_new_connections += metrics.num_connects;
  }
  fixture.Stop();
  Require(fixture.connections == 1, "ten requests share one server connection");
  Require(total_new_connections == 1, "only the first of ten requests opens a connection");
  Require(fixture.bodies.size() == 10, "server received ten bodies");
}

void TestConnectionCloseReconnectsCleanly() {
  HttpFixture fixture(ServerMode::CloseEach);
  CurlConnectionCache cache;
  const std::string url = Url(fixture);
  const Metrics first = Request(cache, url, "A", "proxy1", "close-one");
  const Metrics second = Request(cache, url, "A", "proxy1", "close-two");
  fixture.Stop();
  Require(first.code == CURLE_OK && second.code == CURLE_OK,
          "requests after Connection: close succeed");
  Require(second.num_connects == 1, "request after Connection: close opens a new connection");
  Require(fixture.connections == 2, "server saw two connections after close");
  Require(fixture.bodies == std::vector<std::string>({"close-one", "close-two"}), "close bodies");
}

void TestDroppedResponseIsNotResent() {
  // Evidence (libcurl 8.22, loopback): with CURLOPT_POSTFIELDS the server
  // received the dropped body twice while the client reported success.
  // With the no-seek read callback the transfer fails instead and the
  // server receives the body exactly once; the next request then recovers
  // on a fresh connection with its own body/credentials.
  HttpFixture fixture(ServerMode::DropSecondBody);
  CurlConnectionCache cache;
  const std::string url = Url(fixture);
  Require(Request(cache, url, "A", "proxy1", "first-ok").code == CURLE_OK, "first request");
  const Metrics dropped = Request(cache, url, "A", "proxy1", "drop-body");
  Require(dropped.code != CURLE_OK, "dropped response must surface an error, not silent resend");
  const Metrics recovered = Request(cache, url, "B", "proxy1", "after-drop");
  fixture.Stop();
  Require(recovered.code == CURLE_OK, "request after drop recovers");
  Require(fixture.bodies == std::vector<std::string>({"first-ok", "drop-body", "after-drop"}),
          "dropped body received exactly once, no duplicate POST");
  std::cout << "Dropped-response request failed with curl code " << static_cast<int>(dropped.code)
            << " and was not resent\n";
}

void TestCancelThenNextRequestIsIsolated() {
  HttpFixture fixture(ServerMode::KeepAlive);
  CurlConnectionCache cache;
  const std::string url = Url(fixture);
  const Metrics cancelled = Request(cache, url, "A", "proxy1", "cancel-body", true);
  Require(cancelled.code == CURLE_ABORTED_BY_CALLBACK, "progress callback aborts transfer");
  const Metrics next = Request(cache, url, "A", "proxy1", "after-cancel");
  fixture.Stop();
  Require(next.code == CURLE_OK && next.response == "OK", "request after cancel succeeds");
  int after_cancel_count = 0;
  for (const std::string& body : fixture.bodies) {
    if (body == "after-cancel") {
      ++after_cancel_count;
    }
  }
  Require(after_cancel_count == 1, "post-cancel body delivered exactly once");
}

} // namespace

int main() {
  Require(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK, "curl_global_init");
  TestIsolationAcrossIdentityChanges();
  TestTenConsecutiveRequestsReuseOneConnection();
  TestConnectionCloseReconnectsCleanly();
  TestDroppedResponseIsNotResent();
  TestCancelThenNextRequestIsIsolated();
  curl_global_cleanup();
  std::cout << "Curl connection cache tests passed\n";
}
