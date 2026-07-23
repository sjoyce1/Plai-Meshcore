# Plai-Meshcore

Plai-Meshcore is a port of the beautiful **Plai** M5Stack Cardputer communicator application from Meshtastic to the lightweight, decentralized **MeshCore** networking stack. 

This repository decouples the Plai UI from the heavy Meshtastic backend, tying the user interface directly to MeshCore's peer-to-peer and group messaging framework.

---

## 🚀 Key Features

*   **Premium UI**: Retains the complete, feature-rich LovyanGFX and Mooncake UI components of Plai.
*   **MeshCore Core Integration**: Powered by the lightweight, decentralized MeshCore stack, optimized for ESP32 hardware.
*   **Default MeshCore Interop**: Pre-configured out of the box to communicate with standard MeshCore nodes.
*   **Direct Messaging (P2P)**: Ed25519-signed, ECDH-encrypted, and authenticated peer-to-peer private messages.
*   **Group Channel Broadcasts**: Broadly accessible mesh-wide message broadcasting with AES-128 / HMAC-SHA256 authentication.
*   **Local SD Card Logging**: Message persistence and historical conversation logging using FATFS on SD card.

---

## 📡 Default Radio & Channel Parameters

| Parameter | Value |
| :--- | :--- |
| **Frequency** | `910.525 MHz` |
| **Bandwidth** | `62.5 kHz` |
| **Spreading Factor (SF)** | `7` |
| **Coding Rate (CR)** | `4/5` |
| **Transmit Power** | `22 dBm` |
| **Sync Word** | `0x12` *(MeshCore Default)* |
| **Default Public Channel PSK** | `izOH6cXN6mrJ5e26oRXNcg==` (Hash `0x11`) |

---

## 🛠️ Build & Installation

### Prerequisites
*   **ESP-IDF v5.5** or higher.
*   **Python v3.10+** (with `protobuf` and `grpcio-tools` packages).

### 1. Clone Recursively
Ensure submodules are populated:
```bash
git clone --recursive https://github.com/sjoyce1/Plai-Meshcore.git
cd Plai-Meshcore
```

### 2. Generate Nanopb Headers
Change to the `protobufs` directory and run `protoc` via Python to compile the Meshtastic schemas with Nanopb options:
```bash
cd protobufs
python -m grpc_tools.protoc --experimental_allow_proto3_optional --plugin=protoc-gen-nanopb=../components/Nanopb/generator/protoc-gen-nanopb.bat --nanopb_out=-S.cpp:../main/meshtastic -I. -I../components/Nanopb/generator/proto/ meshtastic/*.proto
cd ..
```

### 3. Build & Flash
Set up the ESP-IDF environment and build:
```bash
# Export ESP-IDF variables (Windows Example)
. C:\Users\sjoyce1\esp\v5.5.1\esp-idf\export.ps1

# Build the project
idf.py build

# Flash and Monitor
idf.py -p PORT flash monitor
```

---

## 📂 Architecture Overview

*   **`main/mesh/meshcore_bridge.h`**: The bridge class routing data streams between the UI's entry box/logger and the underlying MeshCore stack.
*   **`main/mesh/meshcore/meshcore_radio_wrapper.h`**: Event-driven hardware SX1262 driver interface for MeshCore packet dispatching.
*   **`main/mesh/mesh_service.cpp`**: Implements startup initialization, node databases (`NodeDB`), and coordinates wrappers for GPS and Radio modules.
*   **`main/mesh/meshcore/`**: Lightweight decentralized networking layer managing routing tables, packet dispatch, flooding, and cryptography.
*   **`components/LovyanGFX/` & `components/mooncake/`**: UI components and framework drivers.
