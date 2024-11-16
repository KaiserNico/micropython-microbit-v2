//Nico Kaiser
#ifndef MICROPY_INCLUDED_CODAL_APP_NEOPIXELHAL_H
#define MICROPY_INCLUDED_CODAL_APP_NEOPIXELHAL_H

void neopixel_hal_NeoPixel(int id,int pin, int len, int bytes_per_pixel, const uint8_t *buf);

void neopixel_hal_write(int id, int length, const uint8_t *buf);

#endif // MICROPY_INCLUDED_CODAL_APP_NEOPIXELHAL_H
