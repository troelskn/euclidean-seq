#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

#include <Adafruit_NeoKey_1x4.h>
#include <seesaw_neopixel.h>

#include <MIDI.h>
#include <HardwareSerial.h>

#include "limits.h"

// https://kevinboone.me/picoflash.html?i=1
// https://github.com/raspberrypi/pico-examples/blob/master/flash/program/flash_program.c

#define ARRAY_TERMINATE 0
#define TRIGGER 'x'
#define REST '.'

int bjorklun_buffer[64][65];

#define MILLIS_PER_MINUTE 60000
unsigned long last_clock_step;
unsigned long sequencer_next_clock_step;
int sequencer_play_position = -1;
// [midichannel-1][key]
unsigned long sequencer_key_off_events[16][128];
unsigned long sequencer_next_key_off_event = 0;

struct MIDISettings : public midi::DefaultSettings
{
  static const long BaudRate = 31250;
};

MIDI_CREATE_CUSTOM_INSTANCE(HardwareSerial, Serial1, MIDI, MIDISettings);

#define SCREEN_I2C_ADDRESS 0x3c //initialize with the I2C addr 0x3C Typically eBay OLED's
#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 64 // OLED display height, in pixels
#define OLED_RESET -1   //   QT-PY / XIAO
#define SCREEN_ROTATION 1

#define NEOKEY_I2C_ADDRESS 0x30

#define KEYNUM_TRACK 3
#define KEYNUM_PROPERTY 2
#define KEYNUM_DECREMENT 1
#define KEYNUM_INCREMENT 0

Adafruit_SH1107 display = Adafruit_SH1107(SCREEN_HEIGHT, SCREEN_WIDTH, &Wire1, OLED_RESET);
Adafruit_NeoKey_1x4 neokey(NEOKEY_1X4_ADDR, &Wire1);
uint8_t neokey_buttons;

bool app_state_display_changed = true;

#define PAGE_TRACK 0
#define PAGE_SETTINGS 1
int app_state_current_page = PAGE_TRACK;

#define PROPERTY_LENGTH 0
#define PROPERTY_DENSITY 1
#define PROPERTY_SHIFT 2
#define PROPERTY_KEY 3
#define PROPERTY_VELOCITY_VARIANCE 4
#define NUMBER_OF_PROPERTIES 5
char property_names[NUMBER_OF_PROPERTIES][10] = { "Len", "Den", "Shf", "Key", "Vel" };

#define NUMBER_OF_TRACK_CURSOR_POSITIONS 6
int app_state_track_cursor_position = 0;

#define SETTINGS_NUMBER_OF_TRACKS 0
#define SETTINGS_CLOCK 1
#define SETTINGS_BPM 2
#define SETTINGS_MIDI_CHANNEL 3
#define NUMBER_OF_SETTINGS_ITEMS 4
char settings_item_names[NUMBER_OF_SETTINGS_ITEMS][10] = { "Tracks", "Clock", "BPM", "MIDI Ch" };
int app_state_settings[NUMBER_OF_SETTINGS_ITEMS] = { 4, 0, 120, 10 };

#define NUMBER_OF_SETTINGS_CURSOR_POSITIONS 5
int app_state_settings_cursor_position = 0;

#define MAX_TRACK_LENGTH 64
#define MAX_TRACKS 64
int app_state_tracks[MAX_TRACKS][NUMBER_OF_PROPERTIES];
int app_state_selected_track = 0;
int app_state_track_patterns[MAX_TRACKS][MAX_TRACK_LENGTH];

unsigned long sequencer_clock = 0;

void setup() {
  //Serial.begin(9600);
  //while (! Serial) delay(10);

  app_state_setup();
  display_setup();
  display_update();
  neokey_setup();
  midi_setup();
}

void loop() {
  MIDI.read();
  sequencer_loop();
  neokey_update();
  display_update();
}

bool midi_external_clock_active = false;
unsigned long midi_external_clock = 0;
void midi_external_on_clock() {
  midi_external_clock++;
  if (midi_external_clock % 6 == 0) {
    if (app_setting_clock() == 1) { //  running on external clock source
      sequencer_advance_clock();
    }
  }
}

void midi_external_on_start() {
  midi_external_clock_active = true;
  midi_external_clock = 0;
  clear_key_off_events();
}

void midi_external_on_stop() {
  midi_external_clock_active = false;
}

void midi_external_on_continue() {
  midi_external_clock_active = true;
}

void clear_key_off_events() {
  for (int channel=0; channel<16; channel++) {
    for (int key=0; key<128; key++) {
      sequencer_key_off_events[channel][key] = 0;
    }
  }
  sequencer_next_key_off_event = ULONG_MAX;  
}

void midi_setup() {
  clear_key_off_events();
  MIDI.setHandleClock(midi_external_on_clock);
  MIDI.setHandleStart(midi_external_on_start);
  MIDI.setHandleContinue(midi_external_on_continue);
  MIDI.setHandleStop(midi_external_on_stop);
  MIDI.begin(MIDI_CHANNEL_OMNI);
  MIDI.turnThruOff();
  MIDI.setThruFilterMode(midi::Thru::Off);
}

int steps_per_beat() {
  return 4;
}

int beats_per_minute() {
  return 120;
}

unsigned long millis_between_beats() {
  return MILLIS_PER_MINUTE / beats_per_minute();
}

unsigned long millis_between_steps() {
  return millis_between_beats() / steps_per_beat();
}

void sequencer_loop() {
  if (app_setting_clock() == 1) { //  running on external clock source
    return;
  }
  if (millis() < sequencer_next_clock_step) {
    return;
  }
  sequencer_next_clock_step += millis_between_steps();
  sequencer_advance_clock();
}

void sequencer_advance_clock() {
  sequencer_clock++;
  sequencer_trigger_step();
  sequencer_trigger_note_offs();
}

void sequencer_trigger_note_offs() {
  unsigned long event;
  if (sequencer_next_key_off_event > sequencer_clock) {
    return;
  }
  sequencer_next_key_off_event = ULONG_MAX;
  for (int channel=0; channel<16; channel++) {
    for (int key=0; key<128; key++) {
      event = sequencer_key_off_events[channel][key];
      if (event != 0) {
        if (event <= sequencer_clock) {
          MIDI.sendNoteOff(key+1, 0, channel+1);
          sequencer_key_off_events[channel][key] = 0;
        } else {
          sequencer_next_key_off_event = min(sequencer_next_key_off_event, event);
        }
      }
    }
  }  
}

void sequencer_trigger_step() {
  sequencer_play_position++;
  for (int track_id=0; track_id < app_setting_number_of_tracks(); track_id++) {
    if (sequencer_step_is_trigger_p(sequencer_play_position, track_id)) {
      sequencer_trigger_note(track_id);
    }
  }
}

void sequencer_trigger_note(int track_id) {
  int key = track_key(track_id);
  int midi_channel = app_setting_midi_channel();
  int velocity = 127 - random(track_velocity_variance(track_id) * 8);
  MIDI.sendNoteOn(key, velocity, midi_channel);
  sequencer_key_off_events[midi_channel-1][key-1] = sequencer_clock + 1;
  sequencer_next_key_off_event = min(sequencer_next_key_off_event, sequencer_clock + 1);  
}

void display_setup() {
  display.begin(SCREEN_I2C_ADDRESS, true);
  display.setRotation(SCREEN_ROTATION);
  display.cp437(true);
  // To use a font in your Arduino sketch, #include the corresponding .h
  // file and pass address of GFXfont struct to setFont().  Pass NULL to
  // revert to 'classic' fixed-space bitmap font.
  // display.setFont(&FreeMono9pt7b);
  display.clearDisplay();
  display.display();
}

void display_update() {
  if (!app_state_display_changed) {
    return;
  }
  if (app_state_current_page == PAGE_TRACK) {
    display_render_page_track();
  }
  if (app_state_current_page == PAGE_SETTINGS) {
    display_render_page_settings();
  }
  app_state_display_changed = false;
}  

void display_render_page_settings() {
  char label[20];
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.println("Settings");

  if (app_state_settings_cursor_position == 0) {
    display.drawChar(128 - 16, 0, (char)0xAE, SH110X_BLACK, SH110X_WHITE, 2);
    display.drawFastVLine(128 - 17, 0, 16, SH110X_WHITE);
  } else {
    display.drawChar(128 - 16, 0, (char)0xAE, SH110X_WHITE, SH110X_BLACK, 2);
  }

  display.setCursor(0, 20);
  display.setTextSize(1);
  for (int i=0; i < NUMBER_OF_SETTINGS_ITEMS; i++) {
    if (i == app_state_settings_cursor_position - 1) {
      display.setTextColor(SH110X_BLACK, SH110X_WHITE); // 'inverted' text      
    } else {
      display.setTextColor(SH110X_WHITE);
    }
    if (i == SETTINGS_CLOCK) {
      sprintf(label, "%-9s %s", settings_item_names[i], app_state_settings[i] == 0 ? "Internal" : "External");      
    } else {
      sprintf(label, "%-7s %3d", settings_item_names[i], app_state_settings[i]);
    }
    display.println(label);
  }
  display.display();  
}

void key_to_name(int key, char output[3]) {
  int octave = (key / 12) - 1;
  switch (key % 12) {
    case 0:
      sprintf(output, "C%d", octave);
      break;
    case 1:
      sprintf(output, "C#%d", octave);
      break;
    case 2:
      sprintf(output, "D%d", octave);
      break;
    case 3:
      sprintf(output, "D#%d", octave);
      break;
    case 4:
      sprintf(output, "E%d", octave);
      break;
    case 5:
      sprintf(output, "F%d", octave);
      break;
    case 6:
      sprintf(output, "F#%d", octave);
      break;
    case 7:
      sprintf(output, "G%d", octave);
      break;
    case 8:
      sprintf(output, "G#%d", octave);
      break;
    case 9:
      sprintf(output, "A%d", octave);
      break;
    case 10:
      sprintf(output, "A#%d", octave);
      break;
    case 11:
      sprintf(output, "B%d", octave);
      break;
  }
}

void display_render_page_track() {
  char label[20];
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  sprintf(label, "Track %2d ", app_state_selected_track + 1);
  display.println(label);

  if (app_state_track_cursor_position == 0) {
    display.drawChar(128 - 16, 0, (char)0xF0, SH110X_BLACK, SH110X_WHITE, 2);
    display.drawFastVLine(128 - 17, 0, 16, SH110X_WHITE);
    display.drawFastVLine(128 - 18, 0, 16, SH110X_WHITE);
  } else {
    display.drawChar(128 - 16, 0, (char)0xF0, SH110X_WHITE, SH110X_BLACK, 2);
  }

  display.setCursor(0, 20);
  display.setTextSize(1);
  for (int prop_id=0; prop_id < NUMBER_OF_PROPERTIES; prop_id++) {
    if (prop_id == app_state_track_cursor_position - 1) {
      display.setTextColor(SH110X_BLACK, SH110X_WHITE); // 'inverted' text      
    } else {
      display.setTextColor(SH110X_WHITE);
    }
    if (prop_id == PROPERTY_KEY) {
      char key_name[3];
      key_to_name(app_state_tracks[app_state_selected_track][prop_id], key_name);
      sprintf(label, "%s.%3s", property_names[prop_id], key_name);
    } else {
      sprintf(label, "%s.%2d", property_names[prop_id], app_state_tracks[app_state_selected_track][prop_id]);      
    }
    display.print(label);
    display.setTextColor(SH110X_WHITE);
    display.print(" ");
  }

  int selected_track_length = track_length(app_state_selected_track);
  for (int step=0; step < min(8, selected_track_length); step++) {
    if (sequencer_step_is_trigger_p(step, app_state_selected_track)) {
      display.fillCircle((step * 10) + 4, 44, 4, SH110X_WHITE);
    } else {
      display.drawCircle((step * 10) + 4, 44, 4, SH110X_WHITE);
    }
  }
  for (int step=8; step < min(16, selected_track_length); step++) {
    if (sequencer_step_is_trigger_p(step, app_state_selected_track)) {
      display.fillCircle(((step - 8) * 10) + 4, 56, 4, SH110X_WHITE);
    } else {      
      display.drawCircle(((step - 8) * 10) + 4, 56, 4, SH110X_WHITE);
    }
  }

  display.display();  
}

int app_property_length() {
  return track_length(app_state_selected_track);
}

int app_setting_number_of_tracks() {
  return app_state_settings[SETTINGS_NUMBER_OF_TRACKS];
}

int app_setting_clock() {
  return app_state_settings[SETTINGS_CLOCK];
}

int app_setting_bpm() {
  return app_state_settings[SETTINGS_BPM];
}

int app_setting_midi_channel() {
  return app_state_settings[SETTINGS_MIDI_CHANNEL];
}

int track_length(int track_id) {
  return app_state_tracks[track_id][PROPERTY_LENGTH];
}

int track_density(int track_id) {
  return app_state_tracks[track_id][PROPERTY_DENSITY];
}

int track_shift(int track_id) {
  return app_state_tracks[track_id][PROPERTY_SHIFT];
}

int track_velocity_variance(int track_id) {
  return app_state_tracks[track_id][PROPERTY_VELOCITY_VARIANCE];
}

int track_key(int track_id) {
  return app_state_tracks[track_id][PROPERTY_KEY];
}

void neokey_setup() {
  neokey.begin(NEOKEY_I2C_ADDRESS);
}

void neokey_update() {
  uint8_t buttons_was = neokey_buttons;
  neokey_buttons = neokey.read();
  for (int keynum=0; keynum<4; keynum++) {
    if (neokey_buttons & (1<<keynum)) {
      if (buttons_was & (1<<keynum)) {
        // noop
      } else {
        neokey_on_keydown(keynum);      
      }
    } else {
      if (buttons_was & (1<<keynum)) {
        neokey_on_keyup(keynum);      
      } else {
        // noop
      }
    }
  }
}

void neokey_on_keydown(int keynum) {
  if (keynum == KEYNUM_TRACK) {
    app_next_track();
  }
  if (keynum == KEYNUM_PROPERTY) {
    app_next_property();
  }
  if (keynum == KEYNUM_DECREMENT) {
    app_decrement_selected_property();
  }
  if (keynum == KEYNUM_INCREMENT) {
    app_increment_selected_property();
  }
}

void neokey_on_keyup(int keynum) {
}

void app_state_setup() {
  for (int i=0; i<MAX_TRACKS; i++) {
    app_state_tracks[i][PROPERTY_LENGTH] = 16;
    app_state_tracks[i][PROPERTY_DENSITY] = 0;
    app_state_tracks[i][PROPERTY_SHIFT] = 0;
    app_state_tracks[i][PROPERTY_KEY] = 36;
    app_state_tracks[i][PROPERTY_VELOCITY_VARIANCE] = 0;
    calculate_track_pattern(i);
  }
}

void app_next_track() {
  if (app_state_current_page == PAGE_SETTINGS) {
    return;
  }  
  app_state_selected_track++;
  if (app_state_selected_track >= app_setting_number_of_tracks()) {
    app_state_selected_track = 0;
  }
  app_state_display_changed = true;
}

void app_next_property() {
  if (app_state_current_page == PAGE_TRACK) {
    app_state_track_cursor_position++;
    if (app_state_track_cursor_position >= NUMBER_OF_TRACK_CURSOR_POSITIONS) {
      app_state_track_cursor_position = 0;
    }
    app_state_display_changed = true;
  }  
  if (app_state_current_page == PAGE_SETTINGS) {
    app_state_settings_cursor_position++;
    if (app_state_settings_cursor_position >= NUMBER_OF_SETTINGS_CURSOR_POSITIONS) {
      app_state_settings_cursor_position = 0;
    }
    app_state_display_changed = true;
  }  
}

void check_bounds() {
  if (app_state_settings[SETTINGS_NUMBER_OF_TRACKS] < 1) {
    app_state_settings[SETTINGS_NUMBER_OF_TRACKS] = 1;
  }
  if (app_state_settings[SETTINGS_NUMBER_OF_TRACKS] >= MAX_TRACKS) {
    app_state_settings[SETTINGS_NUMBER_OF_TRACKS] = MAX_TRACKS - 1;
  }
  if (app_state_selected_track >= app_state_settings[SETTINGS_NUMBER_OF_TRACKS]) {
    app_state_selected_track = app_state_settings[SETTINGS_NUMBER_OF_TRACKS] - 1;
  }
  if (app_state_settings[SETTINGS_CLOCK] < 0) {
    app_state_settings[SETTINGS_CLOCK] = 0;
  }
  if (app_state_settings[SETTINGS_CLOCK] > 1) {
    app_state_settings[SETTINGS_CLOCK] = 1;
  }
  if (app_state_settings[SETTINGS_MIDI_CHANNEL] < 1) {
    app_state_settings[SETTINGS_MIDI_CHANNEL] = 1;
  }
  if (app_state_settings[SETTINGS_MIDI_CHANNEL] > 16) {
    app_state_settings[SETTINGS_MIDI_CHANNEL] = 16;
  }  
  if (app_state_tracks[app_state_selected_track][PROPERTY_LENGTH] < 1) {
    app_state_tracks[app_state_selected_track][PROPERTY_LENGTH] = 1;
  }
  if (app_state_tracks[app_state_selected_track][PROPERTY_LENGTH] > MAX_TRACK_LENGTH) {
    app_state_tracks[app_state_selected_track][PROPERTY_LENGTH] = MAX_TRACK_LENGTH;
  }
  if (app_state_tracks[app_state_selected_track][PROPERTY_DENSITY] < 0) {
    app_state_tracks[app_state_selected_track][PROPERTY_DENSITY] = 0;
  }
  if (app_state_tracks[app_state_selected_track][PROPERTY_DENSITY] > app_state_tracks[app_state_selected_track][PROPERTY_LENGTH]) {
    app_state_tracks[app_state_selected_track][PROPERTY_DENSITY] = app_state_tracks[app_state_selected_track][PROPERTY_LENGTH];
  }
  // TODO: Do we want to allow negative shift?
  if (app_state_tracks[app_state_selected_track][PROPERTY_SHIFT] < 0) {
    app_state_tracks[app_state_selected_track][PROPERTY_SHIFT] = 0;
  }
  if (app_state_tracks[app_state_selected_track][PROPERTY_SHIFT] > app_state_tracks[app_state_selected_track][PROPERTY_LENGTH]) {
    app_state_tracks[app_state_selected_track][PROPERTY_SHIFT] = app_state_tracks[app_state_selected_track][PROPERTY_LENGTH];
  }  
  if (app_state_tracks[app_state_selected_track][PROPERTY_KEY] < 24) {
    app_state_tracks[app_state_selected_track][PROPERTY_KEY] = 24;
  }
  if (app_state_tracks[app_state_selected_track][PROPERTY_KEY] > 127) {
    app_state_tracks[app_state_selected_track][PROPERTY_KEY] = 127;
  }
  if (app_state_tracks[app_state_selected_track][PROPERTY_VELOCITY_VARIANCE] < 0) {
    app_state_tracks[app_state_selected_track][PROPERTY_VELOCITY_VARIANCE] = 0;
  }
  if (app_state_tracks[app_state_selected_track][PROPERTY_VELOCITY_VARIANCE] > 16) {
    app_state_tracks[app_state_selected_track][PROPERTY_VELOCITY_VARIANCE] = 16;
  }
}

void app_decrement_selected_property() {
  if (app_state_current_page == PAGE_TRACK) {
    if (app_state_track_cursor_position == 0) {
      return;
    }
    app_state_tracks[app_state_selected_track][app_state_track_cursor_position-1]--;
    calculate_track_pattern(app_state_selected_track); // TODO: Only really necessary if one of len, den, shf changed
    app_state_display_changed = true;
  }
  if (app_state_current_page == PAGE_SETTINGS) {
    if (app_state_settings_cursor_position == 0) {
      return;
    }
    app_state_settings[app_state_settings_cursor_position-1]--;
    app_state_display_changed = true;
  }
  check_bounds();
}

void app_increment_selected_property() {
  if (app_state_current_page == PAGE_TRACK) {
    if (app_state_track_cursor_position == 0) {
      app_state_current_page = PAGE_SETTINGS;
      app_state_settings_cursor_position = 0;
      app_state_display_changed = true;
      return;
    }
    app_state_tracks[app_state_selected_track][app_state_track_cursor_position-1]++;
    calculate_track_pattern(app_state_selected_track); // TODO: Only really necessary if one of len, den, shf changed
    app_state_display_changed = true;
  }
  if (app_state_current_page == PAGE_SETTINGS) {
    if (app_state_settings_cursor_position == 0) {
      app_state_current_page = PAGE_TRACK;
      app_state_display_changed = true;
      return;
    }
    app_state_settings[app_state_settings_cursor_position-1]++;
    app_state_display_changed = true;
  }
  check_bounds();
}

int zarray_count(int arr[]) {
  int counter = 0;
  while (arr[counter] != ARRAY_TERMINATE) {
    counter++;
  }
  return counter;
}

void zarray_concat(int target[], int source[]) {
  int target_size = zarray_count(target);
  int counter = 0;
  while (source[counter] != ARRAY_TERMINATE) {
    target[target_size + counter] = source[counter];
    counter++;
  }
  target[target_size + counter] = ARRAY_TERMINATE;
}

void print_array(int arr[]) {
  int length = zarray_count(arr);
  for (int i=0; i < length; i++) {
    Serial.print((char)arr[i]);
  }
  Serial.println();
}

void bjorklund_calculate(int length, int density, int output[]) {
  if (density > length || density < 1) {
    for (int i=0; i < length; i++) {
      output[i] = REST;
    }
    output[length] = ARRAY_TERMINATE;
    return;
  }  
  // Init arrays
  // Ex. l = 8 and d = 5
  // [1] [1] [1] [1] [1] [0] [0] [0]
  int width = length - 1;
  for (int i=0; i < length; i++) {
    bjorklun_buffer[i][0] = i < density ? TRIGGER : REST;
    bjorklun_buffer[i][1] = ARRAY_TERMINATE;    
  }
  int target, remainder_size;
  while (true) {
    if (width == 0) {
      break;
    }
    if (bjorklun_buffer[width][0] == REST) {
      // zero remainder
      target = 0;
      while (bjorklun_buffer[width][0] == REST && bjorklun_buffer[target][0] != REST) {
        zarray_concat(bjorklun_buffer[target], bjorklun_buffer[width]);
        target++;
        width--;
      }
    } else {
      // pattern-remainder?
      remainder_size = zarray_count(bjorklun_buffer[width]);
      if (remainder_size == zarray_count(bjorklun_buffer[0])) {
        // no remainder present
        break;
      }
      if (remainder_size != zarray_count(bjorklun_buffer[width-1])) {
        // we have reached a core
        break;
      }
      target = 0;
      while (zarray_count(bjorklun_buffer[width]) == remainder_size && zarray_count(bjorklun_buffer[target]) != remainder_size) {
        zarray_concat(bjorklun_buffer[target], bjorklun_buffer[width]);
        target++;
        width--;
      }
    }
  }

  // collapse rows into output
  for (int i=0; i <= width; i++) {
    zarray_concat(output, bjorklun_buffer[i]);
  }
}

void calculate_track_pattern(int track_id) {
  int length = track_length(track_id);
  int density = track_density(track_id);
  app_state_track_patterns[track_id][0] = ARRAY_TERMINATE;
  bjorklund_calculate(length, density, app_state_track_patterns[track_id]);    
}

bool sequencer_step_is_trigger_p(int step, int track_id) {
  int length = track_length(track_id);
  int shift = track_shift(track_id);
  int position = (step - shift) % length;
  while (position < 0) {
    position = position + length;
  }
  return app_state_track_patterns[track_id][position] == TRIGGER;
}
