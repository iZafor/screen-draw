CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O3 -Wall -Wextra
PKG_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 cairo gio-2.0)
PKG_LIBS   := $(shell pkg-config --libs gtk+-3.0 cairo gio-2.0) -lm

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
