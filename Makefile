CXX        ?= g++
CXXFLAGS   ?= -std=c++17 -O3 -Wall -Wextra
PKG_CFLAGS := $(shell pkg-config --cflags cairo x11 xext xrandr)
PKG_LIBS   := $(shell pkg-config --libs cairo x11 xext xrandr) -lm

TARGET     := screen-draw
TEST_BIN   := build/test-screen-draw

CORE_SRC   := src/geometry.cpp src/draw_item.cpp src/ipc.cpp src/toolbar.cpp src/window.cpp
APP_SRC    := src/main.cpp $(CORE_SRC)
TEST_SRC   := tests/test_main.cpp $(CORE_SRC)
HEADERS    := src/geometry.hpp src/draw_item.hpp src/ipc.hpp src/window.hpp

.PHONY: all test clean install

all: $(TARGET)

$(TARGET): $(APP_SRC) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(PKG_CFLAGS) -o $(TARGET) $(APP_SRC) $(PKG_LIBS)

$(TEST_BIN): $(TEST_SRC) $(HEADERS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(PKG_CFLAGS) -o $(TEST_BIN) $(TEST_SRC) $(PKG_LIBS)

test: $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -f $(TARGET) $(TEST_BIN)

install: $(TARGET)
	install -Dm755 $(TARGET) /usr/local/bin/$(TARGET)
