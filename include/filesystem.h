#ifndef FILESYSTEM_H
#define FILESYSTEM_H
#include <Arduino.h>
#include <SdFat.h>
#include <functional>

#include "config.h"

typedef std::function<void(FatVolume &volume)> config_file_read_callback;

void filesystem_set_config_file_read_calback(config_file_read_callback f);
void filesystem_setup();
void filesystem_loop();

FatVolume* filesystem_get_flash_volume();

#endif //FILESYSTEM_H