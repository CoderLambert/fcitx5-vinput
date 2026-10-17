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

void AssertTrue(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << '\n';
    std::abort();
  }
}

size_t OnCurlWriteString(char* data, size_t size, size_t nmemb, void* context) {
  auto* response = static_cast<std::string*>(context);
  response->append(data, size * nmemb);
  return size * nmemb;
}

// Mirrors the production body provider in post_processor.cpp: a read
// callback with deliberately no seek callback, so libcurl cannot rewind
// and automatically resend a POST.
struct CurlBodyReader {
  std::string_view body;
  std::size_t offset = 0;
};

size_t OnCurlReadBody(char* buffer, size_t size, size_t nmemb, void* userdata) {
  auto* reader = static_cast<CurlBodyReader*>(userdata);
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

int OnCurlAbortProgress(void* /*clientp*/, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
                        curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
  return 1;
}

enum class MockServerBehavior : std::uint8_t {
  KeepAlive,
  CloseEachConnection,
  DropSecondRequestBody
};

class MockHttpServer {
public:
  explicit MockHttpServer(MockServerBehavior behavior)
      : behavior_(behavior), listen_fd_(socket(AF_INET, SOCK_STREAM, 0)) {
    AssertTrue(listen_fd_ >= 0, "create listening socket");
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sockaddr sa{};
    std::memcpy(&sa, &sin, sizeof(sin));
    AssertTrue(bind(listen_fd_, &sa, sizeof(sin)) == 0, "bind socket to loopback");
    socklen_t len = sizeof(sa);
    AssertTrue(getsockname(listen_fd_, &sa, &len) == 0, "getsockname");
    std::memcpy(&sin, &sa, sizeof(sin));
    port_ = ntohs(sin.sin_port);
    AssertTrue(listen(listen_fd_, 8) == 0, "listen on socket");
    worker_thread_ = std::thread([this] { RunServerLoop(); });
  }

  MockHttpServer(const MockHttpServer&) = delete;
  MockHttpServer& operator=(const MockHttpServer&) = delete;
  MockHttpServer(MockHttpServer&&) = delete;
  MockHttpServer& operator=(MockHttpServer&&) = delete;

  ~MockHttpServer() {
    Stop();
    close(listen_fd_);
  }

  void Stop() {
    stop_requested_.store(true);
    if (worker_thread_.joinable()) {
      worker_thread_.join();
    }
  }

  [[nodiscard]] std::uint16_t Port() const { return port_; }
  [[nodiscard]] int AcceptedConnectionCount() const { return accepted_connections_.load(); }
  [[nodiscard]] const std::vector<std::string>& CapturedAuthHeaders() const {
    return captured_auth_headers_;
  }
  [[nodiscard]] const std::vector<std::string>& CapturedRequestBodies() const {
    return captured_request_bodies_;
  }

private:
  bool ReadHttpRequest(int client_fd, std::string& out_body, std::string& out_header) const {
    std::string buffer;
    while (!stop_requested_.load()) {
      const std::size_t head_end = buffer.find("\r\n\r\n");
      if (head_end != std::string::npos) {
        const std::string length_key = "Content-Length: ";
        const auto len_pos = buffer.find(length_key);
        const std::size_t body_size = len_pos == std::string::npos
                                          ? 0
                                          : std::stoul(buffer.substr(len_pos + length_key.size()));
        if (buffer.size() >= head_end + 4 + body_size) {
          out_header = buffer.substr(0, head_end);
          out_body = buffer.substr(head_end + 4, body_size);
          return true;
        }
      }
      pollfd pfd{.fd = client_fd, .events = POLLIN, .revents = 0};
      if (poll(&pfd, 1, 100) <= 0) {
        continue;
      }
      char chunk[4096];
      const ssize_t bytes_read = recv(client_fd, chunk, sizeof(chunk), 0);
      if (bytes_read <= 0) {
        return false;
      }
      buffer.append(chunk, static_cast<std::size_t>(bytes_read));
    }
    return false;
  }

  void RunServerLoop() {
    while (!stop_requested_.load()) {
      pollfd pfd{.fd = listen_fd_, .events = POLLIN, .revents = 0};
      if (poll(&pfd, 1, 100) <= 0) {
        continue;
      }
      const int client_fd = accept(listen_fd_, nullptr, nullptr);
      if (client_fd < 0) {
        continue;
      }
      ++accepted_connections_;
      while (!stop_requested_.load()) {
        std::string request_body;
        std::string request_header;
        if (!ReadHttpRequest(client_fd, request_body, request_header)) {
          break;
        }
        const std::size_t auth_pos = request_header.find("Authorization: ");
        const std::size_t auth_end = request_header.find("\r\n", auth_pos);
        captured_auth_headers_.push_back(
            auth_pos == std::string::npos ? ""
                                          : request_header.substr(auth_pos, auth_end - auth_pos));
        captured_request_bodies_.push_back(request_body);
        if (behavior_ == MockServerBehavior::DropSecondRequestBody &&
            captured_request_bodies_.size() == 2) {
          break; // Close without any response: simulate connection drop scenario.
        }
        const std::string response_payload =
            behavior_ == MockServerBehavior::CloseEachConnection
                ? "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK"
                : "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nOK";
        if (send(client_fd, response_payload.data(), response_payload.size(), MSG_NOSIGNAL) !=
            static_cast<ssize_t>(response_payload.size())) {
          break;
        }
        if (behavior_ == MockServerBehavior::CloseEachConnection) {
          break;
        }
        // Keep-alive: wait briefly for the next request on this connection.
        pollfd next_poll{.fd = client_fd, .events = POLLIN, .revents = 0};
        if (poll(&next_poll, 1, 300) <= 0) {
          break;
        }
      }
      close(client_fd);
    }
  }

  const MockServerBehavior behavior_;
  const int listen_fd_;
  std::uint16_t port_ = 0;
  std::thread worker_thread_;
  std::atomic<int> accepted_connections_{0};
  std::atomic<bool> stop_requested_{false};
  std::vector<std::string> captured_auth_headers_;
  std::vector<std::string> captured_request_bodies_;
};

struct RequestExecutionMetrics {
  CURLcode code = CURLE_OK;
  long num_connects = 0;
  double connect_ms = 0.0;
  double total_ms = 0.0;
  std::string response;
};

RequestExecutionMetrics ExecuteCachedRequest(CurlConnectionCache& cache, const std::string& url,
                                             const std::string& secret, const std::string& policy,
                                             const std::string& body, bool abort_progress = false) {
  auto lease = cache.Acquire(
      CurlConnectionIdentity{.endpoint = url, .credential = secret, .proxy_policy = policy});
  AssertTrue(lease.handle != nullptr, "acquire valid easy handle");
  CURL* curl = lease.handle;
  curl_slist* headers = nullptr;
  const std::string authorization = "Authorization: Bearer " + secret;
  headers = curl_slist_append(headers, authorization.c_str());
  std::string response_buffer;
  CurlBodyReader reader{.body = body, .offset = 0};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_READFUNCTION, OnCurlReadBody);
  curl_easy_setopt(curl, CURLOPT_READDATA, &reader);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnCurlWriteString);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buffer);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3000L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  if (abort_progress) {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, OnCurlAbortProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, nullptr);
  }
  // Keep the loopback test independent of host/CI proxy environment variables.
  curl_easy_setopt(curl, CURLOPT_PROXY, "");
  RequestExecutionMetrics metrics;
  metrics.code = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &metrics.num_connects);
  double connect_s = 0.0;
  double total_s = 0.0;
  curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &connect_s);
  curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total_s);
  metrics.connect_ms = connect_s * 1000.0;
  metrics.total_ms = total_s * 1000.0;
  metrics.response = response_buffer;
  curl_easy_reset(curl); // Detach per-request memory before destroying locals.
  curl_slist_free_all(headers);
  return metrics;
}

std::string BuildMockServerUrl(const MockHttpServer& server) {
  return "http://127.0.0.1:" + std::to_string(server.Port()) + "/chat";
}

void TestIsolationAcrossIdentityChanges() {
  MockHttpServer server(MockServerBehavior::KeepAlive);
  CurlConnectionCache cache;
  const std::string url = BuildMockServerUrl(server);
  AssertTrue(ExecuteCachedRequest(cache, url, "A", "proxy1", "one").code == CURLE_OK,
             "request one");
  AssertTrue(ExecuteCachedRequest(cache, url, "A", "proxy1", "two").code == CURLE_OK,
             "request two");
  AssertTrue(ExecuteCachedRequest(cache, url, "B", "proxy1", "three").code == CURLE_OK,
             "request three");
  AssertTrue(ExecuteCachedRequest(cache, url, "B", "proxy2", "four").code == CURLE_OK,
             "request four");
  server.Stop();
  AssertTrue(server.AcceptedConnectionCount() == 3,
             "one connection reused, two credentials/policy isolated");
  AssertTrue(server.CapturedAuthHeaders() ==
                 std::vector<std::string>({"Authorization: Bearer A", "Authorization: Bearer A",
                                           "Authorization: Bearer B", "Authorization: Bearer B"}),
             "auth isolated");
  AssertTrue(server.CapturedRequestBodies() ==
                 std::vector<std::string>({"one", "two", "three", "four"}),
             "request content isolated");
  std::cout << "Four HTTP POSTs over three connections; isolation verified\n";
}

void TestTenConsecutiveRequestsReuseOneConnection() {
  MockHttpServer server(MockServerBehavior::KeepAlive);
  CurlConnectionCache cache;
  const std::string url = BuildMockServerUrl(server);
  long total_new_connections = 0;
  for (int i = 0; i < 10; ++i) {
    const RequestExecutionMetrics metrics =
        ExecuteCachedRequest(cache, url, "A", "proxy1", "req-" + std::to_string(i));
    AssertTrue(metrics.code == CURLE_OK, "ten-request workload succeeds");
    AssertTrue(metrics.response == "OK", "ten-request response");
    std::cout << "workload req " << i << ": NUM_CONNECTS=" << metrics.num_connects
              << " connect_ms=" << metrics.connect_ms << " total_ms=" << metrics.total_ms << '\n';
    total_new_connections += metrics.num_connects;
  }
  server.Stop();
  AssertTrue(server.AcceptedConnectionCount() == 1, "ten requests share one server connection");
  AssertTrue(total_new_connections == 1, "only the first of ten requests opens a connection");
  AssertTrue(server.CapturedRequestBodies().size() == 10, "server received ten bodies");
}

void TestConnectionCloseReconnectsCleanly() {
  MockHttpServer server(MockServerBehavior::CloseEachConnection);
  CurlConnectionCache cache;
  const std::string url = BuildMockServerUrl(server);
  const RequestExecutionMetrics first =
      ExecuteCachedRequest(cache, url, "A", "proxy1", "close-one");
  const RequestExecutionMetrics second =
      ExecuteCachedRequest(cache, url, "A", "proxy1", "close-two");
  server.Stop();
  AssertTrue(first.code == CURLE_OK && second.code == CURLE_OK,
             "requests after Connection: close succeed");
  AssertTrue(second.num_connects == 1, "request after Connection: close opens a new connection");
  AssertTrue(server.AcceptedConnectionCount() == 2, "server saw two connections after close");
  AssertTrue(server.CapturedRequestBodies() == std::vector<std::string>({"close-one", "close-two"}),
             "close bodies");
}

void TestDroppedResponseIsNotResent() {
  // Evidence (libcurl 8.22, loopback): with CURLOPT_POSTFIELDS the server
  // received the dropped body twice while the client reported success.
  // With the no-seek read callback the transfer fails instead and the
  // server receives the body exactly once; the next request then recovers
  // on a fresh connection with its own body/credentials.
  MockHttpServer server(MockServerBehavior::DropSecondRequestBody);
  CurlConnectionCache cache;
  const std::string url = BuildMockServerUrl(server);
  AssertTrue(ExecuteCachedRequest(cache, url, "A", "proxy1", "first-ok").code == CURLE_OK,
             "first request");
  const RequestExecutionMetrics dropped =
      ExecuteCachedRequest(cache, url, "A", "proxy1", "drop-body");
  AssertTrue(dropped.code != CURLE_OK, "dropped response must surface an error, not silent resend");
  const RequestExecutionMetrics recovered =
      ExecuteCachedRequest(cache, url, "B", "proxy1", "after-drop");
  server.Stop();
  AssertTrue(recovered.code == CURLE_OK, "request after drop recovers");
  AssertTrue(server.CapturedRequestBodies() ==
                 std::vector<std::string>({"first-ok", "drop-body", "after-drop"}),
             "dropped body received exactly once, no duplicate POST");
  std::cout << "Dropped-response request failed with curl code " << static_cast<int>(dropped.code)
            << " and was not resent\n";
}

void TestCancelThenNextRequestIsIsolated() {
  MockHttpServer server(MockServerBehavior::KeepAlive);
  CurlConnectionCache cache;
  const std::string url = BuildMockServerUrl(server);
  const RequestExecutionMetrics cancelled =
      ExecuteCachedRequest(cache, url, "A", "proxy1", "cancel-body", true);
  AssertTrue(cancelled.code == CURLE_ABORTED_BY_CALLBACK, "progress callback aborts transfer");
  const RequestExecutionMetrics next =
      ExecuteCachedRequest(cache, url, "A", "proxy1", "after-cancel");
  server.Stop();
  AssertTrue(next.code == CURLE_OK && next.response == "OK", "request after cancel succeeds");
  int after_cancel_count = 0;
  for (const std::string& body : server.CapturedRequestBodies()) {
    if (body == "after-cancel") {
      ++after_cancel_count;
    }
  }
  AssertTrue(after_cancel_count == 1, "post-cancel body delivered exactly once");
}

} // namespace

int main() {
  AssertTrue(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK, "curl_global_init");
  TestIsolationAcrossIdentityChanges();
  TestTenConsecutiveRequestsReuseOneConnection();
  TestConnectionCloseReconnectsCleanly();
  TestDroppedResponseIsNotResent();
  TestCancelThenNextRequestIsIsolated();
  curl_global_cleanup();
  std::cout << "Curl connection cache tests passed\n";
}
