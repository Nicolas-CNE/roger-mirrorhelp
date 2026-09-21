CXX = g++
CXXFLAGS = -Wall -Wextra -O2 -std=c++17 -Isrc
LDFLAGS_ROGER = -lcurl -lminisat
LDFLAGS_MIRROR = -lcurl

SRC_DIR = src
BUILD_DIR = build

all: roger roger-mirrorhelp

# 1. Compilar el Roger principal
roger: $(SRC_DIR)/main.cpp
	@mkdir -p $(BUILD_DIR)
	@echo "==> Compilando Roger principal..."
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS_ROGER)
	@echo "[✓] Generado: ./roger"

# 2. Compilar MirrorHelp en modo standalone (aprovechando su propio main)
roger-mirrorhelp: $(SRC_DIR)/mirrorhelp.cpp
	@mkdir -p $(BUILD_DIR)
	@echo "==> Compilando Roger-MirrorHelp (Standalone)..."
	$(CXX) $(CXXFLAGS) -DSTANDALONE_MIRRORHELP $^ -o $@ $(LDFLAGS_MIRROR)
	@echo "[✓] Generado: ./roger-mirrorhelp"

clean:
	@echo "==> Limpiando..."
	rm -rf $(BUILD_DIR) roger roger-mirrorhelp

install: all
	@echo "==> Instalando binarios en /usr/local/bin..."
	install -m 755 roger /usr/local/bin/
	install -m 755 roger-mirrorhelp /usr/local/bin/

.PHONY: all clean install
