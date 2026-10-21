CXX = g++
CXX_WIN = x86_64-w64-mingw32-g++
JAVAC = javac
JAVA_INCLUDE = -I/usr/lib/jvm/java-21-openjdk/include -I/usr/lib/jvm/java-21-openjdk/include/linux
CXXFLAGS = -std=c++17 -O3 -Wall -I. $(JAVA_INCLUDE)
CLIENT_PLUGINS_DIR = /home/neokirilx/Downloads/AyuGram Desktop/plugins

TARGET_SO = WallpaperTheme.so
TARGET_DLL = WallpaperTheme.dll

JAVA_SRC = java/com/mandagram/plugin/monet/MonetWallpaperEngine.java
CLASS_FILE = bin/com/mandagram/plugin/monet/MonetWallpaperEngine.class
BYTES_HEADER = monet_wallpaper_engine_bytes.h
CPP_SRC = wallpaper_theme.cpp

all: $(TARGET_SO) $(TARGET_DLL)

install: all
	mkdir -p ..
	cp $(TARGET_SO) ../
	cp $(TARGET_DLL) ../
	mkdir -p "$(CLIENT_PLUGINS_DIR)"
	cp $(TARGET_SO) "$(CLIENT_PLUGINS_DIR)/"
	cp $(TARGET_DLL) "$(CLIENT_PLUGINS_DIR)/"

$(CLASS_FILE): $(JAVA_SRC)
	mkdir -p bin
	$(JAVAC) -source 17 -target 17 $(JAVA_SRC) -d bin

$(BYTES_HEADER): $(CLASS_FILE)
	python3 -c 'f=open("$(CLASS_FILE)","rb");d=f.read();f.close();o=open("$(BYTES_HEADER)","w");o.write("static const unsigned char s_monet_engine_class_bytes[] = {\n");[o.write(f"0x{b:02x}, "+("\n" if (i+1)%12==0 else "")) for i,b in enumerate(d)];o.write(f"\n}};\nstatic const unsigned int s_monet_engine_class_bytes_len = {len(d)};\n");o.close()'

$(TARGET_SO): $(BYTES_HEADER) $(CPP_SRC) mandagram_plugin.h
	$(CXX) $(CXXFLAGS) -fPIC -shared $(CPP_SRC) -o $(TARGET_SO) -lpthread -ldl

$(TARGET_DLL): $(BYTES_HEADER) $(CPP_SRC) mandagram_plugin.h
	$(CXX_WIN) $(CXXFLAGS) -shared -static -static-libgcc -static-libstdc++ -s $(CPP_SRC) -o $(TARGET_DLL)

clean:
	rm -rf bin $(TARGET_SO) $(TARGET_DLL) $(BYTES_HEADER)

.PHONY: all install clean
