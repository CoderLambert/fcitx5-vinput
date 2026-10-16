#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <curl/curl.h>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
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

struct HttpFixture {
  int listener = -1;
  uint16_t port = 0;
  std::thread worker;
  std::atomic<int> connections{0};
  std::vector<std::string> auth;
  std::vector<std::string> bodies;
  std::atomic<bool> ok{true};

  HttpFixture() {
    listener = socket(AF_INET, SOCK_STREAM, 0);
    Require(listener >= 0, "socket");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Require(bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "bind");
    socklen_t len = sizeof(addr);
    Require(getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len) == 0, "getsockname");
    port = ntohs(addr.sin_port);
    Require(listen(listener, 4) == 0, "listen");
    worker = std::thread([this] { Run(); });
  }

  ~HttpFixture() {
    if (worker.joinable()) {
      worker.join();
    }
    close(listener);
  }

  void Run() {
    int client = -1;
    std::string buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (bodies.size() < 4 && std::chrono::steady_clock::now() < deadline) {
      pollfd fd{client < 0 ? listener : client, POLLIN, 0};
      if (poll(&fd, 1, 100) <= 0) {
        continue;
      }
      if (client < 0) {
        client = accept(listener, nullptr, nullptr);
        if (client >= 0) {
          ++connections;
          buffer.clear();
        }
        continue;
      }
      if (!(fd.revents & (POLLIN | POLLHUP | POLLERR))) {
        continue;
      }
      char chunk[4096];
      const ssize_t n = recv(client, chunk, sizeof(chunk), 0);
      if (n <= 0) {
        close(client);
        client = -1;
        buffer.clear();
        continue;
      }
      buffer.append(chunk, static_cast<std::size_t>(n));
      const std::size_t head_end = buffer.find("\r\n\r\n");
      if (head_end == std::string::npos) {
        continue;
      }
      const std::string head = buffer.substr(0, head_end);
      const std::string length_key = "Content-Length: ";
      const auto len_pos = head.find(length_key);
      const std::size_t body_size =
          len_pos == std::string::npos ? 0 : std::stoul(head.substr(len_pos + length_key.size()));
      if (buffer.size() < head_end + 4 + body_size) {
        continue;
      }
      const std::size_t auth_pos = head.find("Authorization: ");
      const std::size_t auth_end = head.find("\r\n", auth_pos);
      auth.push_back(auth_pos == std::string::npos ? "" : head.substr(auth_pos, auth_end - auth_pos));
      bodies.push_back(buffer.substr(head_end + 4, body_size));
      buffer.erase(0, head_end + 4 + body_size);
      const std::string answer =
          "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nOK";
      if (send(client, answer.data(), answer.size(), MSG_NOSIGNAL) !=
          static_cast<ssize_t>(answer.size())) {
        ok = false;
        break;
      }
    }
    if (client >= 0) {
      close(client);
    }
    if (bodies.size() != 4) {
      ok = false;
    }
  }
};

void Request(CurlConnectionCache& cache, const std::string& url, std::string secret,
             std::string policy, const std::string& body) {
  auto lease = cache.Acquire({url, secret, policy});
  Require(lease.handle != nullptr, "easy handle");
  CURL* curl = lease.handle;
  curl_slist* headers = nullptr;
  const std::string authorization = "Authorization: Bearer " + secret;
  headers = curl_slist_append(headers, authorization.c_str());
  std::string response;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3000L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  const CURLcode code = curl_easy_perform(curl);
  curl_easy_reset(curl); // Detach per-request memory before destroying locals.
  curl_slist_free_all(headers);
  Require(code == CURLE_OK, "curl_easy_perform");
  Require(response == "OK", "response content");
}
} // namespace

int main() {
  Require(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK, "curl_global_init");
  {
    HttpFixture fixture;
    CurlConnectionCache cache;
    const std::string url = "http://127.0.0.1:" + std::to_string(fixture.port) + "/chat";
    Request(cache, url, "A", "proxy1", "one");
    Request(cache, url, "A", "proxy1", "two");
    Request(cache, url, "B", "proxy1", "three");
    Request(cache, url, "B", "proxy2", "four");
    fixture.worker.join();
    Require(fixture.ok, "fixture success");
    Require(fixture.connections == 3, "one connection reused, two credentials/policy isolated");
    Require(fixture.auth == std::vector<std::string>({
                "Authorization: Bearer A", "Authorization: Bearer A",
                "Authorization: Bearer B", "Authorization: Bearer B"}), "auth isolated");
    Require(fixture.bodies == std::vector<std::string>({"one", "two", "three", "four"}),
            "request content isolated");
    std::cout << "Four HTTP POSTs over three connections; isolation verified\n";
  }
  curl_global_cleanup();
}
