# hypr-altswitch — alt-tab switcher as a Hyprland plugin
#
# The switcher core is C (src/switcher.c); only the Hyprland boundary is C++
# (src/plugin.cpp), because the plugin ABI hands out std::string.

PLUGIN      := hypr-altswitch
BUILD       := build
TARGET      := $(BUILD)/$(PLUGIN).so

PKGS        := hyprland pixman-1 libdrm hyprutils
PKG_CFLAGS  := $(shell pkg-config --cflags $(PKGS))

CC          ?= gcc
CXX         ?= g++

CFLAGS      := -std=c11 -O2 -g -fPIC -Wall -Wextra -Wpedantic $(shell pkg-config --cflags luajit)
# --no-gnu-unique matters: without it the .so cannot be dlclose()d, so plugin
# reloads leak the old copy.
CXXFLAGS    := -std=c++26 -O2 -g -fPIC -Wall -Wno-unused-parameter --no-gnu-unique $(PKG_CFLAGS)
LDFLAGS     := -shared $(shell pkg-config --libs cairo)

# Where hyprctl plugin load expects to find it. Any absolute path works.
INSTALL_DIR ?= $(HOME)/.local/share/hyprland/plugins
# Instance to talk to while testing; 1 is the nested one, 0 your real session.
INSTANCE    ?= 1

C_OBJ       := $(BUILD)/switcher.o $(BUILD)/altswitch_lua.o
CXX_OBJ     := $(BUILD)/plugin.o

.PHONY: all clean install nested load unload reload check

all: $(TARGET)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c src/switcher.h src/altswitch_lua.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(CXX_OBJ): src/plugin.cpp src/switcher.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -Isrc -c $< -o $@

$(TARGET): $(C_OBJ) $(CXX_OBJ)
	$(CXX) $(LDFLAGS) $^ -o $@
	@echo "built $@ against hyprland $(shell pkg-config --modversion hyprland)"

install: $(TARGET)
	@mkdir -p $(INSTALL_DIR)
	install -m644 $(TARGET) $(INSTALL_DIR)/$(PLUGIN).so
	@echo "installed to $(INSTALL_DIR)/$(PLUGIN).so"

# Starts the throwaway compositor in a window (see dotfiles: hypr/nested.lua).
nested:
	@~/.config/hypr/scripts/nested-hyprland.sh

load: $(TARGET)
	hyprctl -i $(INSTANCE) plugin load $(abspath $(TARGET))

unload:
	hyprctl -i $(INSTANCE) plugin unload $(abspath $(TARGET))

# Rebuild and swap the plugin in the nested instance without restarting it.
reload: $(TARGET)
	-hyprctl -i $(INSTANCE) plugin unload $(abspath $(TARGET))
	hyprctl -i $(INSTANCE) plugin load $(abspath $(TARGET))

# Is the built .so still valid for the Hyprland that is running?
check:
	@built="$$(pkg-config --modversion hyprland)"; \
	running="$$(hyprctl version -j | sed -n 's/.*"tag": *"\([^"]*\)".*/\1/p')"; \
	echo "headers: $$built   running: $$running"; \
	if [ ! -f $(TARGET) ]; then echo "not built yet -> make"; \
	elif [ "v$$built" = "$$running" ]; then echo "match — plugin should load"; \
	else echo "MISMATCH -> make clean && make"; fi

clean:
	rm -rf $(BUILD)
