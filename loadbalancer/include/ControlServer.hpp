#pragma once

#include <arpa/inet.h>
#include <functional>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

// Minimal HTTP control plane on a background thread.
//
// POST /register   {"ip":"x.x.x.x","port":6000}  → on_register callback
// POST /deregister {"ip":"x.x.x.x","port":6000}  → on_deregister callback

class ControlServer {
   int                                          port_;
   std::function<void(std::string_view, int)>   on_register_;
   std::function<void(std::string_view, int)>   on_deregister_;
   std::thread                                  thread_;

   // ── minimal JSON field extraction ──────────────────────────────────────
   // Handles flat objects like {"ip":"1.2.3.4","port":6000}

   static std::string extract_string(std::string_view json, std::string_view key) {
      auto kpos = json.find(key);
      if (kpos == std::string_view::npos) return {};
      auto colon = json.find(':', kpos);
      if (colon == std::string_view::npos) return {};
      auto q1 = json.find('"', colon);
      if (q1 == std::string_view::npos) return {};
      auto q2 = json.find('"', q1 + 1);
      if (q2 == std::string_view::npos) return {};
      return std::string(json.substr(q1 + 1, q2 - q1 - 1));
   }

   static int extract_int(std::string_view json, std::string_view key) {
      auto kpos = json.find(key);
      if (kpos == std::string_view::npos) return -1;
      auto colon = json.find(':', kpos);
      if (colon == std::string_view::npos) return -1;
      auto start = colon + 1;
      while (start < json.size() && (json[start] == ' ' || json[start] == '"'))
         start++;
      try { return std::stoi(std::string(json.substr(start))); }
      catch (...) { return -1; }
   }

   // ── HTTP helpers ────────────────────────────────────────────────────────

   static void respond(int fd, int status, std::string_view body) {
      std::string reason = (status == 200) ? "OK" : "Bad Request";
      std::string resp   = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n"
                         + "Content-Length: " + std::to_string(body.size()) + "\r\n"
                         + "Connection: close\r\n\r\n"
                         + std::string(body);
      send(fd, resp.c_str(), resp.size(), 0);
   }

   void handle(int client) {
      char buf[2048] = {};
      ssize_t n = recv(client, buf, sizeof(buf) - 1, 0);
      if (n <= 0) { close(client); return; }

      std::string_view req(buf, n);

      // find path (first line: "POST /register HTTP/1.1")
      auto path_start = req.find(' ');
      auto path_end   = (path_start != std::string_view::npos)
                            ? req.find(' ', path_start + 1)
                            : std::string_view::npos;

      std::string_view path = (path_start != std::string_view::npos && path_end != std::string_view::npos)
                                  ? req.substr(path_start + 1, path_end - path_start - 1)
                                  : "";

      // body is after the blank line
      auto body_start = req.find("\r\n\r\n");
      std::string_view body = (body_start != std::string_view::npos)
                                  ? req.substr(body_start + 4)
                                  : "";

      std::string ip   = extract_string(body, "ip");
      int         port = extract_int(body, "port");

      if (ip.empty() || port <= 0) {
         respond(client, 400, "missing ip or port");
         close(client);
         return;
      }

      if (path == "/register") {
         on_register_(ip, port);
         respond(client, 200, "registered");
      } else if (path == "/deregister") {
         on_deregister_(ip, port);
         respond(client, 200, "deregistered");
      } else {
         respond(client, 400, "unknown path");
      }

      close(client);
   }

   void serve() {
      int server_fd = socket(AF_INET, SOCK_STREAM, 0);
      int opt = 1;
      setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

      sockaddr_in addr{};
      addr.sin_family      = AF_INET;
      addr.sin_addr.s_addr = INADDR_ANY;
      addr.sin_port        = htons(port_);
      bind(server_fd, (sockaddr *)&addr, sizeof(addr));
      listen(server_fd, 8);

      while (true) {
         int client = accept(server_fd, nullptr, nullptr);
         if (client < 0) continue;
         handle(client);
      }

      close(server_fd);
   }

public:
   ControlServer(int port,
                 std::function<void(std::string_view, int)> on_register,
                 std::function<void(std::string_view, int)> on_deregister)
       : port_(port),
         on_register_(std::move(on_register)),
         on_deregister_(std::move(on_deregister)) {}

   void start() {
      thread_ = std::thread([this] { serve(); });
      thread_.detach();
   }
};
