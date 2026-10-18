CXX = g++
CXX_WIN = x86_64-w64-mingw32-g++
CXXFLAGS = -std=c++17 -O2 -Wall -I. -I/usr/lib/jvm/java-25-openjdk/include -I/usr/lib/jvm/java-25-openjdk/include/linux

TARGET_SO = exmoji.so
TARGET_DLL = exmoji.dll

SRC = custom_emoji_plugin.cpp
HEADER = mandagram_plugin.h

all: $(TARGET_SO) $(TARGET_DLL)

$(TARGET_SO): $(SRC) $(HEADER)
	$(CXX) $(CXXFLAGS) -fPIC -shared $(SRC) -o $(TARGET_SO)

$(TARGET_DLL): $(SRC) $(HEADER)
	$(CXX_WIN) $(CXXFLAGS) -shared -static -static-libgcc -static-libstdc++ -s $(SRC) -o $(TARGET_DLL)

install: all
	cp $(TARGET_SO) $(TARGET_DLL) ../

clean:
	rm -f $(TARGET_SO) $(TARGET_DLL)

.PHONY: all install clean
