Windows UDP test client for the g29ffb host daemon.

Build (MinGW):
  x86_64-w64-mingw32-gcc -O2 -o ffb_client.exe ffb_client.c -lws2_32

Usage:
  ffb_client.exe [--host HOST] [--port PORT] const <value> [--hold ms] [--interval ms]
  ffb_client.exe [--host HOST] [--port PORT] stop
  ffb_client.exe [--host HOST] [--port PORT] sweep

Examples:
  ffb_client.exe const 40
  ffb_client.exe const -30 --hold 1500 --interval 50
  ffb_client.exe sweep
