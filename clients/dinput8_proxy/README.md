DirectInput8 proxy (64-bit) for logging FFB calls.

Build (MinGW):
  x86_64-w64-mingw32-g++ -shared -O2 -o dinput8.dll dinput8_proxy.cpp -lws2_32

Install (CrossOver/Wine):
  1) Copy dinput8.dll next to acs.exe (game folder).
  2) Add Wine override: dinput8 = native, builtin.
  3) Run the game; check C:\\ac_ffb_proxy.log for logs.

Notes:
  - Logs CreateEffect / SetParameters / Start / Stop / SendForceFeedbackCommand.
  - Sends UDP to the macOS host when ConstantForce is set.
    - Defaults: 127.0.0.1:21999
    - Override with env vars: FFB_HOST and FFB_PORT
  - Logging controls (env vars):
    - FFB_LOG=0 disables logging
    - FFB_LOG_EVERY_MS=200 throttles logs (one line per 200ms)
