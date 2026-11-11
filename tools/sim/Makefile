# Mac-Simulator für die Soft-White-Oberfläche: make && ./sim home out.ppm
REPO := ../..
LVGL := $(REPO)/managed_components/lvgl__lvgl
LV_SRC := $(shell find $(LVGL)/src -name '*.c' -not -path '*/thorvg/*')
UI_SRC := $(wildcard $(REPO)/main/ui/*.c) $(wildcard $(REPO)/main/ui/gen/*.c) $(wildcard $(REPO)/main/ui/fonts/*.c)
CFLAGS := -O1 -g -I. -I$(LVGL) -I$(REPO)/main -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -w -MMD
BUILD := build
LV_OBJ := $(patsubst $(LVGL)/%.c,$(BUILD)/lvgl/%.o,$(LV_SRC))
UI_OBJ := $(patsubst $(REPO)/main/%.c,$(BUILD)/ui/%.o,$(UI_SRC))

sim: $(LV_OBJ) $(UI_OBJ) $(BUILD)/sim_main.o
	$(CC) -o $@ $^ -lm

$(BUILD)/lvgl/%.o: $(LVGL)/%.c lv_conf.h
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ui/%.o: $(REPO)/main/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sim_main.o: sim_main.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Simulator mit Schein-Drucker (Live-Oberfläche wie auf dem Gerät): make sim_live && ./sim_live out.ppm dr:zustand=RUNNING …
LIVE_OBJ := $(patsubst $(REPO)/main/%.c,$(BUILD)/live/%.o,$(UI_SRC))
sim_live: $(LV_OBJ) $(LIVE_OBJ) $(BUILD)/live/sim_main.o $(BUILD)/live/drucker_sim.o
	$(CC) -o $@ $^ -lm

$(BUILD)/live/%.o: $(REPO)/main/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DSW_LIVE_SIM -c $< -o $@

$(BUILD)/live/sim_main.o: sim_main.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DSW_LIVE_SIM -c $< -o $@

$(BUILD)/live/drucker_sim.o: drucker_sim.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DSW_LIVE_SIM -c $< -o $@

clean:
	rm -rf $(BUILD) sim sim_live
-include $(shell find build -name "*.d" 2>/dev/null)
