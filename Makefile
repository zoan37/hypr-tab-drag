PLUGIN_NAME = tab-drag

SOURCES = $(wildcard src/*.cpp)

CXXFLAGS += -shared -fPIC --no-gnu-unique -std=c++2b -Wall -O2 -g
PKGS = pixman-1 libdrm hyprland libinput

all: $(PLUGIN_NAME).so

$(PLUGIN_NAME).so: $(SOURCES) $(wildcard src/*.hpp)
	$(CXX) -o $@ $(SOURCES) $(CXXFLAGS) $(shell pkg-config --cflags $(PKGS))

clean:
	rm -f $(PLUGIN_NAME).so

.PHONY: all clean
