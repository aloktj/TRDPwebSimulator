#!/usr/bin/env bash
set -e

echo "[setup_drogon] Updating package index..."
sudo apt update

echo "[setup_drogon] Installing build dependencies..."
sudo apt install -y git cmake g++ uuid-dev openssl libssl-dev \
    zlib1g-dev libjsoncpp-dev libsqlite3-dev libboost-all-dev

if [ ! -d /tmp/drogon ]; then
  echo "[setup_drogon] Cloning Drogon..."
  git clone https://github.com/drogonframework/drogon.git /tmp/drogon
else
  echo "[setup_drogon] Drogon source already exists in /tmp/drogon, pulling latest..."
  (cd /tmp/drogon && git pull)
fi

cd /tmp/drogon
mkdir -p build
cd build

echo "[setup_drogon] Configuring..."
cmake .. -DCMAKE_BUILD_TYPE=Release

echo "[setup_drogon] Building..."
make -j"$(nproc)"

echo "[setup_drogon] Installing (sudo)..."
sudo make install

echo "[setup_drogon] Done. Drogon installed system-wide."
