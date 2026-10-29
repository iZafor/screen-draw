CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O3 -Wall -Wextra
PKG_CFLAGS := $(shell pkg-config --cflags cairo x11 xext xrandr)
PKG_LIBS   := $(shell pkg-config --libs cairo x11 xext xrandr) -lm

TARGET   := screen-draw
SRC      := src/main.cpp

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(PKG_CFLAGS) -o $(TARGET) $(SRC) $(PKG_LIBS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) /usr/local/bin/$(TARGET)
