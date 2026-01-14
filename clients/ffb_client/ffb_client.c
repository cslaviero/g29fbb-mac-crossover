#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")

static void usage(const char *exe) {
    fprintf(stderr,
        "Usage:\n"
        "  %s [--host HOST] [--port PORT] const <value> [--hold ms] [--interval ms]\n"
        "  %s [--host HOST] [--port PORT] stop\n"
        "  %s [--host HOST] [--port PORT] sweep\n"
        "\n"
        "Examples:\n"
        "  %s const 40\n"
        "  %s --host 127.0.0.1 --port 21999 const -30 --hold 1500 --interval 50\n"
        "  %s sweep\n",
        exe, exe, exe, exe, exe, exe
    );
}

static int send_msg(SOCKET s, const struct sockaddr_in *addr, const char *msg) {
    int len = (int)strlen(msg);
    int r = sendto(s, msg, len, 0, (const struct sockaddr *)addr, sizeof(*addr));
    if (r == SOCKET_ERROR) {
        fprintf(stderr, "sendto failed: %d\n", WSAGetLastError());
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    int port = 21999;
    int hold_ms = 1000;
    int interval_ms = 50;

    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host = argv[i + 1];
            i += 2;
            continue;
        }
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[i + 1]);
            i += 2;
            continue;
        }
        if (strcmp(argv[i], "--hold") == 0 && i + 1 < argc) {
            hold_ms = atoi(argv[i + 1]);
            i += 2;
            continue;
        }
        if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) {
            interval_ms = atoi(argv[i + 1]);
            i += 2;
            continue;
        }
        break;
    }

    if (i >= argc) {
        usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[i++];

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        fprintf(stderr, "socket failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid host: %s\n", host);
        closesocket(s);
        WSACleanup();
        return 1;
    }

    if (strcmp(cmd, "stop") == 0) {
        send_msg(s, &addr, "STOP");
    } else if (strcmp(cmd, "const") == 0) {
        if (i >= argc) {
            fprintf(stderr, "Missing value for const\n");
            usage(argv[0]);
            closesocket(s);
            WSACleanup();
            return 1;
        }
        int value = atoi(argv[i]);
        if (interval_ms <= 0) interval_ms = 50;
        if (hold_ms < interval_ms) hold_ms = interval_ms;

        int elapsed = 0;
        char msg[64];
        while (elapsed < hold_ms) {
            snprintf(msg, sizeof(msg), "CONST %d", value);
            send_msg(s, &addr, msg);
            Sleep((DWORD)interval_ms);
            elapsed += interval_ms;
        }
        send_msg(s, &addr, "STOP");
    } else if (strcmp(cmd, "sweep") == 0) {
        const int values[] = { -60, -40, -20, 0, 20, 40, 60, 40, 20, 0, -20, -40, -60, 0 };
        const int steps = (int)(sizeof(values) / sizeof(values[0]));
        char msg[64];
        for (int k = 0; k < steps; k++) {
            snprintf(msg, sizeof(msg), "CONST %d", values[k]);
            send_msg(s, &addr, msg);
            Sleep(300);
        }
        send_msg(s, &addr, "STOP");
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        usage(argv[0]);
        closesocket(s);
        WSACleanup();
        return 1;
    }

    closesocket(s);
    WSACleanup();
    return 0;
}
