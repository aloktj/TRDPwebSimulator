# TRDPwebSimulator

A **web-based TRDP (Train Real-time Data Protocol) simulator** that uses:

- **TRDP + TAU stack (TCNopen)** for real train communication behavior  
- **XML configuration** to define ComIds, datasets, and telegrams  
- A **Drogon-based web backend** (C++)  
- A **browser UI** to view & edit telegram payloads as tables  
- **Live updates** of RX telegrams over WebSocket  

This README is written so that tools like **GitHub Copilot / Codex / ChatGPT** can understand the architecture and generate consistent code when asked.

---

## High-Level Overview

### Goals

- Load TRDP configuration from XML (TAU).
- For each PD/MD telegram:
  - Represent it as a **dataset + fields** (name, type, offset).
  - Expose its **current values** via REST/WebSocket.
- Provide a web UI where the user can:
  - Select a TRDP XML config.
  - View all telegrams (Tx/Rx) in a list.
  - Open a telegram and see all fields in a **table**.
  - Modify Tx values and send them.
  - See Rx values update in **real time** as telegrams arrive.

### Architecture

```text
                +------------------+
                |   Web Frontend   |
                | (React/Vue/etc.) |
                +---------+--------+
                          |
                REST (JSON) + WebSocket
                          |
           +--------------v---------------+
           |        Drogon Backend        |
           |  - REST Controllers          |
           |  - WebSocket Controller      |
           |  - TelegramHub plugin        |
           +--------------+---------------+
                          |
                    C++ Core Library
                          |
      +-------------------+-------------------+
      |                                       |
+-----v---------+                     +-------v--------+
| Telegram      |   uses metadata     |  TRDP Engine   |
| Model /       |<------------------->|  (TRDP + TAU)  |
| Registry      |                     +-------+--------+
+---------------+                             |
                                        TRDP Stack
                                     (TRDP + TAU libs)


---

Core Concepts

TRDP + TAU Usage

TRDP core API (tlc_*, tlp_*, tlm_*) is used for:

Initializing sessions

Creating PD/MD publishers & subscribers

Sending/receiving telegrams

Running the processing loop


TAU XML API is used for:

Loading TRDP XML config

Discovering datasets, fields, ComIds

Providing offsets, sizes, and types for marshalling



The simulator does not hard-code TRDP telegram layouts.
Instead, it derives everything from the XML config.


---

Source Layout

Proposed/expected structure (adjust to your real layout):

.
├── CMakeLists.txt
├── README.md
├── configs/
│   ├── default.xml
│   └── *.xml          # TRDP XML configurations
├── src/
│   ├── main.cpp
│   ├── telegram_model.h
│   ├── telegram_model.cpp
│   ├── trdp_engine.h
│   ├── trdp_engine.cpp
│   ├── controllers/
│   │   ├── ConfigController.h
│   │   ├── ConfigController.cpp
│   │   ├── TelegramController.h
│   │   ├── TelegramController.cpp
│   │   ├── WsTelegram.h
│   │   └── WsTelegram.cpp
│   └── plugins/
│       ├── TelegramHub.h
│       └── TelegramHub.cpp
└── third_party/
    ├── drogon/        # Drogon submodule (optional)
    └── tcnopen/       # TRDP + TAU (TCNopen) stack


---

Data Model (C++ Core)

Datasets & Fields

Each dataset is defined by XML and represented as:

enum class FieldType {
    U8, U16, U32,
    S8, S16, S32,
    F32, F64,
    BOOL,
    CHAR,    // fixed-length character / string
};

struct FieldDef {
    std::string name;
    FieldType   type;
    size_t      offsetBytes;
    size_t      bitOffset;   // 0 if unused
    size_t      arrayLen;    // 1 for scalar
};

struct DatasetDef {
    uint32_t              datasetId;
    std::string           name;
    size_t                sizeBytes;
    std::vector<FieldDef> fields;
};

Telegram Definition & Runtime State

enum class Direction { Tx, Rx };
enum class TelegramType { PD, MD };

struct TelegramDef {
    uint32_t      comId;
    uint32_t      datasetId;
    std::string   name;
    Direction     direction;
    TelegramType  type;
    uint32_t      srcIp;
    uint32_t      destIp;
    uint32_t      cycleTimeMs;  // PD
    uint32_t      timeoutMs;
};

using FieldScalar = std::variant<int64_t, double, bool, std::string>;

struct FieldValue {
    FieldType   type;
    FieldScalar value;
};

struct TelegramRuntime {
    TelegramDef                       def;
    std::vector<uint8_t>              txBuffer;
    std::vector<uint8_t>              rxBuffer;
    std::map<std::string, FieldValue> txFields;
    std::map<std::string, FieldValue> rxFields;
    void*                             pdSubHandle = nullptr;
    void*                             pdPubHandle = nullptr;
    mutable std::mutex                mtx;
};

Registry Singleton

TelegramRegistry holds all datasets + telegrams:

class TelegramRegistry {
public:
    static TelegramRegistry& instance();

    void clear();
    void addDataset(const DatasetDef& ds);
    void addTelegram(const TelegramDef& tg);

    std::shared_ptr<TelegramRuntime> getTelegramByComId(uint32_t comId);
    std::vector<std::shared_ptr<TelegramRuntime>> getAllTelegrams() const;
    const DatasetDef* getDataset(uint32_t datasetId) const;
};

There are also two key helper functions:

void encodeFieldsToBuffer(const DatasetDef& ds,
                          const std::map<std::string, FieldValue>& fields,
                          std::vector<uint8_t>& buffer);

void decodeBufferToFields(const DatasetDef& ds,
                          const std::vector<uint8_t>& buffer,
                          std::map<std::string, FieldValue>& fields);

> Important for AI tools:
When implementing marshalling/unmarshalling, always:

Use DatasetDef::fields and FieldDefs.

Respect FieldType, offsetBytes, endianness as defined by TRDP/TAU.

Do not hard-code telegram layouts.





---

TRDP Engine

The TRDP engine:

1. Loads XML using TAU.


2. Populates TelegramRegistry with DatasetDef and TelegramDef.


3. Initializes the TRDP session.


4. Creates PD/MD publishers/subscribers.


5. Runs the tlc_process() loop.


6. Routes Rx data into TelegramRuntime::rxFields.


7. Provides a helper to send Tx data based on txFields.



Important public functions (conceptual):

namespace TrdpEngine {

// Register callback to notify the web layer about Rx updates.
using TelegramUpdateFn =
    std::function<void(uint32_t comId, const std::map<std::string, FieldValue>&)>;

void setUpdateCallback(TelegramUpdateFn fn);

bool startTrdp(const std::string& xmlPath);
void stopTrdp();

// Called by TRDP callbacks internally:
void onPdReceive(uint32_t comId, const uint8_t* data, uint32_t size);

// Called from web layer to encode & send Tx telegram:
void sendTxTelegram(uint32_t comId);

} // namespace TrdpEngine


---

Web Backend (Drogon)

REST Endpoints

Base URL examples (adjust as needed):

GET /configs
List available XML configs (directory: ./configs).

POST /configs/select
Body:

{ "filename": "default.xml" }

Stops TRDP, reloads XML, restarts TRDP engine.

GET /telegrams
Returns all telegrams:

[
  {
    "comId": 1001,
    "name": "DoorStatus",
    "direction": "Rx",
    "type": "PD",
    "datasetId": 200
  }
]

GET /telegrams/{comId}
Returns a specific telegram with its fields and current values:

{
  "comId": 1001,
  "name": "DoorStatus",
  "direction": "Rx",
  "type": "PD",
  "datasetId": 200,
  "fields": [
    {
      "name": "Door1",
      "arrayLen": 1,
      "offsetBytes": 0,
      "rxValue": true,
      "txValue": false
    }
  ]
}

POST /telegrams/{comId}/values
Update Tx field values and send:

{
  "fields": {
    "Door1": true,
    "Speed": 23.5
  }
}


WebSocket Endpoint

GET /ws/telegrams (WebSocket upgrade)


Server pushes messages like:

{
  "type": "telegram_update",
  "comId": 1001,
  "fields": {
    "Door1": true,
    "Door2": false
  }
}

Front-end subscribes and updates the telegram tables in real time.


---

Frontend Expectations

The frontend (React/Vue/…) is expected to:

Fetch /configs and allow selecting an XML.

Call /configs/select when config is changed.

Fetch /telegrams to list all telegrams.

Fetch /telegrams/{comId} for details when a telegram is selected.

For Tx telegrams:

Render editable fields (checkbox, input, etc.).

Call POST /telegrams/{comId}/values on change or on “Send”.


For Rx telegrams:

Open a WebSocket to /ws/telegrams.

Update displayed values when a telegram_update arrives.




---

Build & Run

Dependencies

C++17 or later

CMake

Drogon (as submodule or preinstalled)

JSON library (Drogon uses jsoncpp)

TRDP + TAU (TCNopen) libraries and headers

POSIX sockets (Linux recommended)

### Installing TCOpen TRDP package

The repository ships a binary Debian package under `binary/tcopentrdp-2.2.23-amd64.deb`. Install it with:

```bash
sudo dpkg -i binary/tcopentrdp-2.2.23-amd64.deb
```

This installs headers under `/usr/include/trdp`, libraries under `/usr/lib` (`libtrdp`, `libtrdpap`, `libtau`), and CMake config files under `/usr/lib/cmake/TRDP` and `/usr/lib/cmake/TCOpenTRDP`, enabling `find_package(TRDP CONFIG REQUIRED)`.

## Drogon Setup (Once per Machine)

The project assumes **Drogon is installed system-wide** (typically under `/usr/local`). Install it once per machine or dev container using the helper script:

```bash
scripts/setup_drogon.sh
```

After the install, Drogon is available through `find_package(Drogon CONFIG REQUIRED)` in CMake, so the repo does **not** rebuild or vendor Drogon. When adding new targets, simply link against `Drogon::Drogon` instead of embedding the framework here.

### Dependency selection flags

CMake lets you toggle between system packages and vendored checkouts to suit your environment:

- `-DUSE_SYSTEM_DROGON=ON|OFF` (default **ON**): when OFF, CMake will try to build/use `third_party/drogon` instead of a machine-wide install.
- `-DUSE_SYSTEM_TRDP=ON|OFF` (default **ON**): when OFF, CMake searches `third_party/tcnopen` for the TRDP/TAU config package before falling back to the system.
- `-DTRDP_USE_SHARED=ON|OFF` (default **ON**): choose between shared TRDP/TAU (`TRDP::trdp_shared`, `TRDP::tau_shared`) and the static pair (`TRDP::trdp`, `TRDP::trdpap`).

System packages remain the preferred path so development containers do not need to rebuild Drogon or the TRDP stack; the vendored toggles are available for offline builds or reproducible toolchains.

### CMake detection example

A minimal consumer is provided in the repository root to verify that the installed TRDP package is discoverable via CMake:

```bash
cmake -S . -B build
cmake --build build
./build/trdp_config_check
```

The configuration stage should log that it found the TRDP package, and the sample executable will initialize and tear down a session to prove the link works. Flip `-DTRDP_USE_SHARED=OFF` if you prefer the static `TRDP::trdp`/`TRDP::trdpap` pair instead of the shared libraries.

Similarly, there is a tiny Drogon probe to validate that the framework is installed system-wide and can start an event loop:

```bash
cmake -S . -B build
cmake --build build
./build/drogon_config_check
```

It binds an ephemeral loopback listener, spins the Drogon event loop briefly, and exits. Any failure indicates that Drogon is not installed or is missing runtime dependencies.


Typical Build Steps

# Clone with submodules (if using them)
git clone --recurse-submodules <this-repo-url>
cd <this-repo>

mkdir -p build
cd build
cmake ..
make -j$(nproc)

> Adjust CMakeLists.txt to:

Link against TRDP/TAU libraries.

Add include paths for TRDP/TAU and Drogon.

Add -pthread etc. as needed.




Run

./trdp-web-simulator

Smoke Test / Demo Data
----------------------

- A minimal TAU XML lives at `configs/default.xml` with one dataset (`StatusDataset`) and two telegrams (`TxStatus`/`RxStatus`).
- A shell helper `scripts/smoke_test.sh` exercises the REST + WebSocket surfaces against that XML:

```bash
# From the repo root (expects ./build/trdp_web_simulator to be built already)
scripts/smoke_test.sh
```

What it does:

1. Starts `trdp_web_simulator` on port `8848` with `configs/default.xml`.
2. Calls `GET /api/config/telegrams` to list the parsed telegrams.
3. Opens a WebSocket to `/ws/telegrams` and waits for a TX broadcast.
4. Sends `POST /api/telegrams/1001/send` with a sample payload; the TX confirmation is captured from the WebSocket log.

`PORT`, `XML_PATH`, and `BINARY` env vars can override the defaults if you run on a different port, XML file, or binary location. The script will `pip install --user websockets` on demand for the WebSocket probe and uses `jq` to pretty-print JSON.

Then open the web UI in your browser, e.g.:

http://localhost:8080/

(Exact port and static UI path depend on your Drogon config.)

---

Configuration files

- `configs/` now ships with a minimal `default.xml` covering a TX heartbeat dataset/telegram and an RX status dataset/telegram
  so that smoke tests have predictable ComIds, datasets, and fields.
- The simulator looks for `configs/default.xml` by default. Override it with either:
  - Environment variable: `TRDP_CONFIG_PATH=/absolute/path/to/config.xml ./trdp-web-simulator`
  - CLI flag: `./trdp-web-simulator --config /absolute/path/to/config.xml`
- The CLI flag takes precedence if both are provided. Point either option at any XML that follows the same dataset/telegram
  structure as the sample.


---

Notes for Copilot / Codex / ChatGPT

When I (the developer) ask the AI assistant for help in this repository, it should assume:

1. Architecture is as described above.

TRDP/TAU are the source of telegram definitions.

TelegramRegistry is the central model for datasets and telegrams.

The web API must stay consistent with the documented endpoints.



2. Do not break these contracts:

Keep TelegramRegistry as the singleton source of truth.

Keep WebSocket messages in the format:

{ "type": "telegram_update", "comId": <int>, "fields": { ... } }

Do not remove or rename REST endpoints without a good reason.



3. When extending the project, prefer:

New controllers / functions that use TelegramRegistry and TrdpEngine.

Code that remains XML-driven (no hard-coded ComIds or layouts).

Clear separation:

Model (telegram_model)

TRDP logic (trdp_engine)

Web API (controllers + plugins)

Frontend (JS/TS, outside this repo part).




4. Typical tasks I might ask the AI:

Implement marshalling logic in encodeFieldsToBuffer / decodeBufferToFields.

Map actual TAU XML API calls to DatasetDef / TelegramDef.

Add new REST endpoints (e.g. filter by Rx/Tx, PD/MD).

Generate a React component for the telegram table UI.

Add logging, diagnostics, or a “fake mode” with no TRDP.




When in doubt, keep the code:

Type-safe

Thread-safe (lock TelegramRuntime::mtx around state changes)

XML-driven (no magic constants)
