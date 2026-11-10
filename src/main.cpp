#include "led_wall.h"
#include "rgbled.h"
#include "sender.h"
#include <Arduino.h>
#include <filesystem.h>

#include <Wire.h>
kts1622 expander1(0x20, 28, &Wire1);
kts1622 expander2(0x21, 26, &Wire1);
uwu::telemetry telem(&expander2);

#if TYPEUWU_VERSION == 2
#include <keyboard.h>

uwu::rgbled rgb(9, KEYBOARD_NUM_KEYS, KEY_LED_MAX_BRIGHTNESS);
uwu::led_wall lwall(10);
uwu::keyboard typeuwu2("typeuwu", &expander1, &rgb, &lwall);
#endif

#if MACRUWU_VERSION == 3
#include <macruwu.h>

uwu::rgbled rgb(1, MACRUWU_NUM_KEYS, KEY_LED_MAX_BRIGHTNESS);
uwu::led_wall lwall(4);
uwu::macruwu macruwu3("macruwu", &expander1, &rgb, &lwall);
#endif

bool update_keymaps(FatVolume &volume)
{
#if TYPEUWU_VERSION == 2
  return typeuwu2.update_keymap_from_file(volume);
#endif
#if MACRUWU_VERSION == 3
  return macruwu3.update_keymap_from_file(volume);
#endif
}

void update_colors()
{
  static unsigned long time_prev = 0;
  if(millis()-10 > time_prev)
  {
    time_prev = millis();

#if TYPEUWU_VERSION == 2
    lwall.update();
    typeuwu2.update_layer_color_effect();
    rgb.send();
#endif
#if MACRUWU_VERSION == 3
    lwall.update();
    macruwu3.update_layer_color_effect();
    rgb.send();
#endif

  }
}

void setup_moduluwu_i2c()
{
  Wire1.setSCL(31);
  Wire1.setSDA(30);
  Wire1.begin();
}

void setup() {
  lwall.init();
#if TYPEUWU_VERSION == 2
  int typeuwu_init = typeuwu2.init();
  Serial.printf("typeuwu init: %d",typeuwu_init);
#endif
#if MACRUWU_VERSION == 3
  int macruwu_init = macruwu3.init();
  Serial.printf("typeuwu init: %d",macruwu_init);
  expander1.use_manual();
#endif

  uwu::init_sender(&telem);

  lwall.set_flash_read_session(filesystem_begin_read, filesystem_end_read);
  filesystem_set_config_file_read_callback(update_keymaps);
  filesystem_setup();

  TinyUSBDevice.detach();
  delay(1000);
  TinyUSBDevice.attach();

  Serial.begin(115200);

  setup_moduluwu_i2c();

  Serial.println("begin");
}

void loop() {
  filesystem_loop();
  uwu::sender_loop();

#if TYPEUWU_VERSION == 2
  typeuwu2.update();
#endif
#if MACRUWU_VERSION == 3
  macruwu3.update();
#endif

  update_colors();

  delay(1);
}