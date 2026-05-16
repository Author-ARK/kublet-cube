/**
 * WebServer TCP listener for emulator.
 *
 * Listens on port 8080 (or EMU_WEBSERVER_PORT) for POST /notify requests.
 * Parses the HTTP body and enqueues it for the main thread to process.
 *
 * Usage:
 *   curl -X POST http://localhost:8080/notify \
 *     -H "Content-Type: application/json" \
 *     -d '{"source":"imessage","sender":"Alice","text":"Hello!"}'
 */

#include "WebServer.h"
#include "otaserver.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// Global instance
WebServer server(80);

static const int EMU_LISTEN_PORT = 8080;

static void tcpListenerThread(WebServer* srv) {
  int listenFd = socket(AF_INET, SOCK_STREAM, 0);
  if (listenFd < 0) {
    printf("[EMU] WebServer: failed to create socket\n");
    return;
  }

  int opt = 1;
  setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // localhost only
  addr.sin_port = htons(EMU_LISTEN_PORT);

  if (bind(listenFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    printf("[EMU] WebServer: failed to bind port %d (already in use?)\n", EMU_LISTEN_PORT);
    close(listenFd);
    return;
  }

  listen(listenFd, 5);
  printf("[EMU] WebServer listening on http://localhost:%d (POST /notify)\n", EMU_LISTEN_PORT);

  while (true) {
    int clientFd = accept(listenFd, nullptr, nullptr);
    if (clientFd < 0) continue;

    // Read the full HTTP request (up to 8KB)
    char buf[8192];
    int total = 0;
    while (total < (int)sizeof(buf) - 1) {
      int n = recv(clientFd, buf + total, sizeof(buf) - 1 - total, 0);
      if (n <= 0) break;
      total += n;
      buf[total] = '\0';

      // Check if we have the full body (Content-Length based)
      char* headerEnd = strstr(buf, "\r\n\r\n");
      if (headerEnd) {
        int headerLen = (int)(headerEnd - buf) + 4;
        // Find Content-Length
        char* cl = strcasestr(buf, "content-length:");
        if (cl) {
          int bodyLen = atoi(cl + 15);
          if (total >= headerLen + bodyLen) break;  // got full body
        } else {
          break;  // no content-length, assume we have everything
        }
      }
    }
    buf[total] = '\0';

    // Extract body (after \r\n\r\n)
    char* body = strstr(buf, "\r\n\r\n");
    if (body) {
      body += 4;

      // Check it's a POST to /notify
      if (strncmp(buf, "POST /notify", 12) == 0 || strncmp(buf, "POST / ", 7) == 0) {
        printf("[EMU] WebServer: received notification: %s\n", body);
        srv->enqueueNotification(std::string(body));

        // Send 200 OK
        const char* resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\n\r\n{\"ok\":true}";
        send(clientFd, resp, strlen(resp), 0);
      } else {
        // Send 404
        const char* resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        send(clientFd, resp, strlen(resp), 0);
      }
    }

    close(clientFd);
  }
}

void WebServer::_startListener() {
  std::thread(tcpListenerThread, this).detach();
}

void OTAServer::run() {
  printf("[EMU] OTAServer run (no-op)\n");
  server.begin();  // Start emulator TCP listener for POST /notify
}

// OTAServer::handle() delegates to the global WebServer to process queued notifications
void OTAServer::handle() {
  server.handleClient();
}
