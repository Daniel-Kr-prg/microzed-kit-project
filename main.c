#define _POSIX_C_SOURCE 200112L

#include <sys/mman.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <malloc.h>
#include <string.h>
#include <byteswap.h>
#include <getopt.h>
#include <inttypes.h>
#include <time.h>

#include "mzapo_parlcd.h"
#include "mzapo_phys.h"
#include "mzapo_regs.h"

#include "font_types.h"

char *memdev="/dev/mem";

/*
 * Base address of the region used for mapping of the knobs and LEDs
 * peripherals in the ARM Cortex-A9 physical memory address space.
 */
#define SPILED_REG_BASE_PHYS 0x43c40000

/* Valid address range for the region */
#define SPILED_REG_SIZE      0x00004000

/*
 * Byte offset of the register which controls individual LEDs
 * in the row of 32 yellow LEDs. When the corresponding bit
 * is set (value 1) then the LED is lit.
 */
#define SPILED_REG_LED_LINE_o           0x004

/*
 * The register to control 8 bit RGB components of brightness
 * of the first RGB LED
 */
#define SPILED_REG_LED_RGB1_o           0x010

/*
 * The register to control 8 bit RGB components of brightness
 * of the second RGB LED
 */
#define SPILED_REG_LED_RGB2_o           0x014

/*
 * The register representing knobs positions as three
 * 8-bit values where each value is incremented
 * and decremented by the knob relative turning.
 */
#define SPILED_REG_KNOBS_8BIT_o         0x024


unsigned char *parlcd_mem_base;
unsigned char *mem_base;

int pos_X = 240;
int pos_Y = 160;
font_descriptor_t * fdes;

// SCALE SELECTING VARS
int scale = 5;

// COLOR VARS
short color_selected;


// MAIN VARS
int state = 0; // 0 - drawing / 1 - color select / 2 - data printing options

// LCD VARS
unsigned short layers[2][320*480*2];

unsigned short rgb2short(int r, int g, int b)
{
  if (r > 31)
    r = 31;
  if (g > 63)
    g = 63;
  if (b > 31)
    b = 31;
  // r>>=3;
  // g>>=2;
  // b>>=3;
  return (((r&0x1f)<<11)|((g&0x3f)<<5)|(b&0x1f));
}

void draw_pixel(int x, int y, unsigned short color, int layer)
{
  if (x >= 0 && x < 480 && y >= 0 && y < 320)
  {
    layers[layer][x + 480 * y] = color;
  }
}

void draw_pixel_scaled(int x, int y, unsigned short color, int layer, int scale_pixel) 
{
  int i,j;
  for (i=0; i<scale_pixel; i++) {
    for (j=0; j<scale_pixel; j++) {
      draw_pixel(x+i, y+j, color, layer);
    }
  }
}

void draw_line(int x1, int y1, int x2, int y2, unsigned short color, int layer, int scale_line)
{
  int i,j;
  int half_scale = scale_line / 2;
  if (y1 == y2)
  {
    for (int k=x1; k < x2+1; k++)
    {
      for (i=-half_scale; i<half_scale+1; i++)
        for (j=-half_scale; j<half_scale+1; j++) 
        {
          draw_pixel(k + i, y1 + j, color, layer);
        }
    }
  }
  else if (x1 == x2)
  {
    for (int k=y1; k < y2+1; k++)
    {
      for (i=-half_scale; i<half_scale+1; i++)
        for (j=-half_scale; j<half_scale+1; j++) 
        {
          draw_pixel(x1+i, k+j, color, layer);
        }
    }
  }
  else
  {

    int y_to_x = (y2 - y1) / (x2 - x1);
    for (int k=x1; k < x2+1; k++)
    {
      int tmp_y = y1 + (k - x1) * y_to_x;
      for (i=-half_scale; i<half_scale+1; i++) 
      {
        for (j=-half_scale; j<half_scale+1; j++) 
        {
          draw_pixel(k + i, tmp_y+j, color, layer);
        }
      }
    }
  }
}

int char_width(int ch)
{
  int width = 0;
  if ((ch >= fdes->firstchar) && (ch - fdes->firstchar < fdes->size))
  {
    ch -= fdes->firstchar;
    if (!fdes->width)
    {
      width = fdes->maxwidth;
    }
    else
    {
      width = fdes->width[ch];
    }
  }
  return width;
}

void draw_char(int x, int y, font_descriptor_t * fdes_char, char ch, int scale_pixel, unsigned short color, int layer) 
{
  int w = char_width(ch);
  const font_bits_t *ptr;
  if ((ch >= fdes_char->firstchar) && (ch-fdes_char->firstchar < fdes_char->size)) 
  {
    if (fdes_char->offset) 
    {
      ptr = &fdes_char->bits[fdes_char->offset[ch-fdes_char->firstchar]];
    } 
    else 
    {
      int bw = (fdes_char->maxwidth+15)/16;
      ptr = &fdes_char->bits[(ch-fdes_char->firstchar)*bw*fdes_char->height];
    }
    int i, j;
    for (i=0; i<fdes_char->height; i++) {
      font_bits_t val = *ptr;
      for (j=0; j<w; j++) {
        if ((val&0x8000)!=0) {
          draw_pixel_scaled(x+scale_pixel*j, y+scale_pixel*i, color, layer, scale_pixel);
        }
        val<<=1;
      }
      ptr++;
    }
  }
}

void clear_screen(unsigned char * parlcd_mem_base)
{
  parlcd_write_cmd(parlcd_mem_base, 0x2c);
  for (int i = 0; i < 320 ; i++) {
    for (int j = 0; j < 480 ; j++) {
      int c = 0;
      parlcd_write_data(parlcd_mem_base, c);
    }
  }
}

void clear_layer(int layer)
{
  for (int i = 0; i < 320 ; i++) {
    for (int j = 0; j < 480 ; j++) {
      layers[layer][i * 480 + j] = 0;
    }
  }
}

void lcd_update(unsigned char * parlcd_mem_base)
{
    parlcd_write_cmd(parlcd_mem_base, 0x2c);
    for (int i = 0; i < 320; i++)
    {
      for (int j = 0; j < 480; j++)
      {
        if ((state == 0) && (i >= pos_Y) && (i < pos_Y + scale) && (j >= pos_X) && (j < pos_X + scale))
          parlcd_write_data(parlcd_mem_base, color_selected);
        else if ((layers[1][i * 480 + j] != 0) || (state == 1) || (state == 2) || (state == 3))
          parlcd_write_data(parlcd_mem_base, layers[1][i * 480 + j]);
        else if (state == 0)
          parlcd_write_data(parlcd_mem_base, layers[0][i * 480 + j]);
          
      }
    }
}

void draw_text(int x_start, int y_start, font_descriptor_t * fdes_text, int scale_text, int width_mp, int layer, short color, char str[], int size)
{
  char *ch=str;
  int x = 0;
  for (int i=0; i<size; i++) {
    draw_char(x_start + x, y_start, fdes_text, *ch, scale_text, color, layer);
    x+=char_width(*ch) * width_mp;
    ch++;
  }

}
void draw_int(int x_start, int y_start, font_descriptor_t * fdes_text, int scale_text, int width_mp, int layer, short color, int number)
{
  int x = 0;

  while (number > 0)
  {
    char digit = number % 10 + '0';
    number /= 10;
    draw_char(x_start + x, y_start, fdes_text, digit, scale_text, color, layer);
    x-=char_width(digit) * width_mp;

  }
}

void showUIlines(unsigned char* parlcd_mem_base)
{

  clear_layer(1);
  clear_screen(parlcd_mem_base);

  draw_line(10,10, 470, 10, rgb2short(31, 63, 31), 1, 3);
  draw_line(10,10, 10, 310, rgb2short(31, 31, 0), 1, 3);
  draw_line(470,10, 470, 310, rgb2short(31, 63, 0), 1, 3);
  draw_line(10,310, 470, 310, rgb2short(0, 63, 0), 1, 3);


}
void draw_background()
{
    draw_line(0,35, 35, 0, rgb2short(31, 0, 0), 1, 35);
    draw_line(0,105, 105, 0, rgb2short(31, 31, 0), 1, 35);
    draw_line(0,175, 175, 0, rgb2short(31, 63, 0), 1, 35);
    draw_line(0,245, 245, 0, rgb2short(0, 63, 0), 1, 35);
    draw_line(0,315, 315, 0, rgb2short(0 , 26, 31), 1, 35);
    draw_line(70,315, 385, 0, rgb2short(20, 0, 31), 1, 35);
    draw_line(140,315, 455, 0, rgb2short(29, 0, 31), 1, 35);
    draw_line(240,200, 480, 200, rgb2short(0, 0, 0), 1, 150);
}
void draw_hi()
{
  clear_screen(parlcd_mem_base);
  draw_background();
  draw_text(175, 130, &font_winFreeSystem14x16,  5, 5, 1, rgb2short(31,63,31), "MZ APO", 6);
  draw_text(175, 200, &font_winFreeSystem14x16, 5, 5, 1, rgb2short(31,63,31), "PAINTER", 7);
  draw_text(205, 300, &font_rom8x16, 1, 1, 1, rgb2short(31,63,31), "PRESS ANYTHING TO START", 23);
  lcd_update(parlcd_mem_base);
}

void draw_bye()
{
  clear_screen(parlcd_mem_base);
  clear_layer(1);
  draw_background();
  draw_text(175, 160, &font_winFreeSystem14x16,  6, 6, 1, rgb2short(31,63,31), "BYE :)", 6);
  lcd_update(parlcd_mem_base);
}

int main(int argc, char *argv[])
{
  fdes = &font_winFreeSystem14x16;
  // COLOR SELECTOR
  int r = 0;
  int g = 0;
  int b = 0;

  char not_started = 1;

  mem_base = map_phys_address(SPILED_REG_BASE_PHYS, SPILED_REG_SIZE, 0);
  if (mem_base == NULL)
    exit(1);

  parlcd_mem_base = map_phys_address(PARLCD_REG_BASE_PHYS, PARLCD_REG_SIZE, 0);
  if (parlcd_mem_base == NULL)
    exit(1);

  parlcd_hx8357_init(parlcd_mem_base);

  // BEGIN
  printf("Not started\n");
  draw_hi();
  uint32_t rgb_knobs_value_old = -1;
  int led_moving = 31;
  int back = 0;
  while (not_started) {
     /* Initialize structure to 0 seconds and 200 milliseconds */
     struct timespec loop_delay = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
      uint32_t rgb_knobs_value = *(volatile uint32_t*)(mem_base + SPILED_REG_KNOBS_8BIT_o);

    if (rgb_knobs_value_old == -1)
    {
      rgb_knobs_value_old = rgb_knobs_value;
    }
    else if (rgb_knobs_value != rgb_knobs_value_old)
    {
      not_started = 0;
    }

     /* Store the read value to the register controlling individual LEDs */
    *(volatile uint32_t*)(mem_base + SPILED_REG_LED_LINE_o) = 1 << led_moving;
    if (led_moving == 31)
      back = 1;
    else if (led_moving == 0)
      back = 0;

    if (back)
      led_moving--;
    else
      led_moving++; 
     clock_nanosleep(CLOCK_MONOTONIC, 0, &loop_delay, NULL);
    
  }

  *(volatile uint32_t*)(mem_base + SPILED_REG_LED_LINE_o) = 0;
  printf("Started\n");
  clear_layer(1);
  clear_screen(parlcd_mem_base);

  // KNOBS CHANGING VARS

  int knobs_old[4];
  knobs_old[0] = -1;
  knobs_old[1] = -1;
  knobs_old[2] = -1;
  knobs_old[3] = -1;

  int knobs_change[4];

  int scale_changing = 0;
  color_selected = rgb2short(31,62,31);
  r = 31;
  g = 62;
  b = 31;

  state = 0;

  int selected_in_menu = 0;

  int showing_coordinates = 0;
  int showing_color = 0;
  int faster_brush = 1;

  while (1) 
  {
    // BASICS
    
     uint32_t rgb_knobs_value;
    struct timespec loop_delay = {.tv_sec = 0, .tv_nsec = 200 * 1000 * 1000};
    int scale_LED_RGB;

     rgb_knobs_value = *(volatile uint32_t*)(mem_base + SPILED_REG_KNOBS_8BIT_o);
     /* Store the read value to the register controlling individual LEDs */

     knobs_change[0] = 0;
     knobs_change[1] = 0;
     knobs_change[2] = 0;
     knobs_change[3] = 0;

     unsigned char bytes[4];
     bytes[0] = (rgb_knobs_value >> 24) & 0xFF;
     bytes[1] = (rgb_knobs_value >> 16) & 0xFF;
     bytes[2] = (rgb_knobs_value >> 8) & 0xFF;
     bytes[3] = rgb_knobs_value & 0xFF;

     for (int i = 0; i < 4; i++)
     {
       if (knobs_old[i] == -1)
       {
         knobs_old[i] = bytes[i];
       }
       else if (knobs_old[i] != bytes[i])
       {
         if (knobs_old[i] < bytes[i] || (knobs_old[i] > 251 && bytes[i] < 5))
           knobs_change[i] = 1;
         else if (knobs_old[i] > bytes[i] || (knobs_old[i] < 5 && bytes[i] > 251))
           knobs_change[i] = -1;

         knobs_old[i] = bytes[i];
       }
    }

    if (state == 3)
    {
      clear_screen(parlcd_mem_base);
      led_moving = 0;
      draw_bye();
      while (led_moving < 32) 
      {
        /* Initialize structure to 0 seconds and 200 milliseconds */
        struct timespec loop_delay_2 = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};

        /* Store the read value to the register controlling individual LEDs */
        *(volatile uint32_t*)(mem_base + SPILED_REG_LED_LINE_o) = 1 << led_moving;
        led_moving++; 
        clock_nanosleep(CLOCK_MONOTONIC, 0, &loop_delay_2, NULL);
        
      }
      *(volatile uint32_t*)(mem_base + SPILED_REG_LED_LINE_o) = 0;
      *(volatile uint32_t *)(mem_base + SPILED_REG_LED_RGB1_o) = 0;
      *(volatile uint32_t *)(mem_base + SPILED_REG_LED_RGB2_o) = 0;
      clear_screen(parlcd_mem_base);
      exit(0);
    }

    if (knobs_change[0] == 1)
    {
      state = (state + 1) % 3;

      if (state == 0)
      {
        clear_layer(1);
      }
      if (state == 1)
      {
        showUIlines(parlcd_mem_base);
      }
      if (state == 2)
      {
        showUIlines(parlcd_mem_base);
      }
    }

    if (state == 0) 
    {
      // SET SCALE OF DRAWING
      if (knobs_change[2] != 0)
      {
        scale_changing = (scale_changing + knobs_change[2] + 8) % 8;
        scale = 5 + scale_changing;
      }
      
      *(volatile uint32_t*)(mem_base + SPILED_REG_LED_LINE_o) = 1 << (15 - scale_changing);

      scale_LED_RGB = (scale_changing << 24) * 16 + (scale_changing << 16) * 16 + (scale_changing << 8) * 16 + scale * 16;
      *(volatile uint32_t *)(mem_base + SPILED_REG_LED_RGB2_o) = scale_LED_RGB;
      // SCALE END

      // MOVE THE POINT
      
      if (knobs_change[1] != 0)
      {
        pos_X = (pos_X + knobs_change[1] + scale_changing * knobs_change[1] * faster_brush + 480) % 480;
      }
      if (knobs_change[3] != 0)
      {
        pos_Y = (pos_Y - knobs_change[3] - scale_changing * knobs_change[3] * faster_brush + 320) % 320;
      }

      if ((knobs_change[1] != 0) || (knobs_change[3] != 0)) 
      {
        if (scale != 5)
          draw_pixel_scaled(pos_X, pos_Y, color_selected, 0, scale);
      }

      clear_layer(1);
      if (showing_coordinates)
      {
        draw_text(4,10, &font_rom8x16, 1, 1, 1,rgb2short(31,63,31), "X: ", 3);
        draw_int(55,10, &font_rom8x16, 1, 1, 1,rgb2short(31,63,31), pos_X);

        draw_text(4,30, &font_rom8x16, 1, 1, 1,rgb2short(31,63,31), "Y: ", 3);
        draw_int(55,30, &font_rom8x16, 1, 1, 1,rgb2short(31,63,31), 320 - pos_Y);
      }
      if (showing_color)
        draw_pixel_scaled(4,50, color_selected, 1, 12);

      printf("X: %d | Y: %d | Width: %d\n", pos_X, pos_Y, scale);
    }
    else if (state == 1)
    {
      if (knobs_change[1] != 0)
      {
        r = (r + knobs_change[1] + 32) % 32;
      }
      if (knobs_change[2] != 0)
      {
         g = (g + knobs_change[2] * 2 + 64) % 64;
      }
      if (knobs_change[3] != 0)
      {
         b = (b + knobs_change[3] + 32) % 32;
      }

      color_selected = rgb2short(r,g,b);

      draw_line(30,30, 120, 30, rgb2short(31, 63, 31), 1, 5);
      draw_line(120,30, 120, 290, rgb2short(31, 63, 31), 1, 5);
      draw_line(30,290, 120, 290, rgb2short(31, 63, 31), 1, 5);
      draw_line(30,30, 30, 290, rgb2short(31, 63, 31), 1, 5);

      for (int i = 0; i < 32; i++)
      {
        draw_pixel_scaled(50, 270 - i*7, rgb2short(i,0,0), 1, 7);
        draw_pixel_scaled(70, 270 - i*7, rgb2short(0,i*2,0), 1, 7);
        draw_pixel_scaled(90, 270 - i*7, rgb2short(0,0,i), 1, 7);
      }
      
      draw_pixel_scaled(50, 270 - r*7, rgb2short(31,63,31), 1, 7);
      draw_pixel_scaled(70, 270 - (g/2)*7, rgb2short(31,63,31), 1, 7);
      draw_pixel_scaled(90, 270 - b*7, rgb2short(31,63,31), 1, 7);

      draw_pixel_scaled(250, 190, rgb2short(r,g,b), 1, 80);

      draw_text(130, 25, &font_winFreeSystem14x16, 3, 3, 1, rgb2short(31,63,31), "COLOR PICKER", 12);
      draw_text(135, 80, &font_rom8x16, 1, 1, 1, rgb2short(31,63,31), "USE KNOBS TO CHANGE THE COLOR", 29);
      
    }
    else if (state == 2)
    {
      if (knobs_change[1] != 0)
      {
        draw_char(20, 30 + 60*selected_in_menu, &font_winFreeSystem14x16, '>', 2, rgb2short(0,0,0), 1);
        selected_in_menu = (selected_in_menu + knobs_change[1] + 5) % 5;
      }
      if (knobs_change[3] != 0)
      {

        if (selected_in_menu == 0)
          showing_coordinates = (showing_coordinates + knobs_change[3] + 2) % 2;
        else if (selected_in_menu == 1)
          showing_color = (showing_color + knobs_change[3] + 2) % 2;
        else if (selected_in_menu == 2)
          faster_brush = (faster_brush + knobs_change[3] + 2) % 2;
        else if (selected_in_menu == 3)
        {
          clear_layer(0);
          draw_text(330, 210, &font_winFreeSystem14x16, 2, 2, 1, rgb2short(31,63,31), "- DONE", 6);
        }
        else if (selected_in_menu == 4)
          state = 3;
          
      }
      draw_char(20, 30 + 60*selected_in_menu, &font_winFreeSystem14x16, '>', 2, rgb2short(31,63,31), 1);

      draw_text(50, 30, &font_winFreeSystem14x16, 2, 2, 1, showing_coordinates ? rgb2short(10,63,10) : rgb2short(31,20,10), "SHOW COORDINATES", 16);
      draw_text(50, 90, &font_winFreeSystem14x16, 2, 2, 1, showing_color ? rgb2short(10,63,10) : rgb2short(31,20,10), "SHOW COLOR", 10);
      draw_text(50, 150, &font_winFreeSystem14x16, 2, 2, 1, faster_brush ? rgb2short(10,63,10) : rgb2short(31,20,10), "FASTER BRUSH", 12);
      draw_text(50, 210, &font_winFreeSystem14x16, 2, 2, 1, rgb2short(31,63,31), "CLEAR THE CANVAS", 16);
      draw_text(50, 270, &font_winFreeSystem14x16, 2, 2, 1, rgb2short(31,63,31), "EXIT", 4);

      draw_text(180, 270, &font_rom8x16, 1, 1, 1, rgb2short(31,63,31), "USE RED KNOB FOR MOVE ON MENU", 29);
      draw_text(140, 290, &font_rom8x16, 1, 1, 1, rgb2short(31,63,31), "USE BLUE KNOB FOR SELECT THE MENU ITEM", 38);
    }

    *(volatile uint32_t *)(mem_base + SPILED_REG_LED_RGB1_o) = (r*8 << 16) + (g*4 << 8) + (b*8 << 0);
    

    lcd_update(parlcd_mem_base);
    clock_nanosleep(CLOCK_MONOTONIC, 0, &loop_delay, NULL);
  }

  return 0;
}
