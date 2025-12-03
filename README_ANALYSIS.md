# README Analysis

## Project Purpose and Goals
- Web-based TRDP simulator leveraging TRDP + TAU stack to mirror real train communication behaviors with XML-driven configuration.
- Backend built with Drogon (C++) serving REST and WebSocket interfaces; frontend displays and edits telegram payloads with live updates.
- Aim: load TRDP XML, expose telegrams/datasets/fields, allow modifying Tx values, and stream Rx updates in real time.

## Architecture Highlights
- Frontend communicates via JSON REST and WebSocket to Drogon backend.
- Backend relies on a C++ core library with Telegram registry/model interfacing with TRDP engine (TRDP + TAU libs).
- XML is sole source of telegram layouts; TelegramRegistry acts as singleton truth, keeping API formats stable (including websocket message schema).

## Repository Layout (expected)
- `configs/` for XML configurations (e.g., default.xml).
- `src/` containing main app, telegram model/engine, controllers (REST & WS), and plugin `TelegramHub`.
- `third_party/` placeholders for Drogon and TCNopen stacks.

## Build/Run Notes
- Requires C++17, CMake, Drogon, jsoncpp, TRDP + TAU libs, pthreads.
- Standard build: clone with submodules, configure with CMake, build with Make, then run `./trdp-web-simulator` and open web UI (default example `http://localhost:8080/`).
- CMake must link TRDP/TAU, include their headers and Drogon, and add pthread options.

## Development Guidance from README
- Preserve XML-driven design; avoid hard-coded ComIds/layouts.
- Maintain REST/WebSocket contracts, especially websocket update payload format.
- Keep code type- and thread-safe (lock `TelegramRuntime::mtx`).
- Prefer extensions through TelegramRegistry/TrdpEngine and additional controllers while following the separation of model, TRDP logic, web API, and frontend.

## Example Tasks Suggested
- Implement encoding/decoding of telegram fields, map TAU XML to definitions, add REST filters, build React UI component, add logging or "fake mode" without TRDP.
