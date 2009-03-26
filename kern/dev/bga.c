/*
 * bga.c -- This file implement a vga driver supporting Chinese GB2312
 * encoding dedicated for KludgeOS with Bochs VGA Extension
 * 
 * Might only works with BGA and never expect resonable performance as
 * I don't make good use of the emulated hardware interface BGA provide
 *
 * Referrence: wiki.osdev.org and font.c of ExpOS by Hyl
 * 
 * Written by Liu Yuan -- liuyuan8@mail.ustc.edu.cn
 * Mar 12, 2009
 */
#include <inc/x86.h>
#include <inc/string.h>
#include <kern/console.h>
#include <inc/assert.h>
#include <kern/pmap.h>
#include <inc/memlayout.h>
#include "bga.h"

/* Below excerpted from wikipedia.org with slight modification
 * 
 * Characters in GB2312 are arranged in a 94x94 grid, and the two-byte 
 * codepoint of each character is expressed in the "quwei" form.
 *
 * qu:  specifies a row
 * qwi: the position of the character within the row
 *
 * The rows (numbered from 1 to 94) contain characters as follows:
 * 	01-09, comprising punctuation and other special characters.
 * 	16-55, the first plane, arranged according to Pinyin.
 * 	56-87, the second plane, arranged according to radical and strokes.
 *
 * The rows 10-15 and 88-94 are unassigned.
 */

/* Draw a pixle locatd at (x, y) */
inline void
draw_pixle(int x, int y, int color)
{
	((uintptr_t *)screen_base)[screen_width * y + x] = color;
}

void
draw_ascii(int row, int col, uint8_t ch, int color)
{
	int i, j;
	uint8_t *bitmap;
	bitmap = ASC[ch];
	for (i = 0; i < font_asc_height; i++)
		for (j = 0; j < font_asc_width; j++)
			if ((bitmap[i] & (1 << (font_asc_width - j - 1)))
			    || ch == ' ')
				draw_pixle(col*font_asc_width + j, 
					   row*font_asc_height + i, color);
}

void
draw_gb2312(int row, int col, uint8_t c1, uint8_t c2, int color)
{
	int i, j, ch;
	uint16_t *bitmap;

	ch = (c1 - 0xA1) * font_chs_wei + (c2 - 0xA1);
	bitmap = CHS[ch];
	for (i = 0; i < font_chs_height; i++)
		for (j = 0; j < font_chs_width; j++)
				if (bitmap[i] & (1 << (font_chs_width - j - 1)))
					/* The layout of GB2312 encoded bitmap is the way ridiculous.It took me more than
					 * one hour to figiure it out. Could anybody explain why that way. Drop me a line
					 * if you could. A thank-you from liu yuan :). Mar 12, 2009, 2:24 PM
					 */
					draw_pixle(col*font_asc_width + (j + font_chs_width / 2) % font_chs_width,
						   row*font_asc_height + i, color);
}

static int ready = 0;
static uint8_t qu_store = 0;	/* 'qu' for first part of Chinese font */
static uint32_t foreground = 0;	/* Foreground color */
static uint32_t background = 0;	/* Background color */
static uint32_t screen_pos = 0;	/* Where next character to put */

/* This is the main part of the driver. Output the character 'c'
 * to the appropriate location and scroll up if necessary
 */
void
bga_putc(int c)
{
	int ch = c & 0xff;
	if (ch >= 0xA1) {	/* in case of the chinese font */
		if (!ready) {
			qu_store = ch;
			ready = 1;
			/* We have to ensure there is enough room for Chinese font */
			if (screen_pos % screen_col == screen_col - 1)
				cons_putc(' ');
		} else {
			/* Now we'er going to deal with 'wei' */
			ready = 0;
			draw_gb2312(screen_pos / screen_col,
				    screen_pos % screen_col,
				    qu_store, ch, foreground);
			screen_pos += 2;
		}
		goto tryscroll;
	}
	/* in case of the ascii */
	switch (ch) {
	case '\b':
		if (screen_pos > 0) {
			screen_pos--;
			draw_ascii(screen_pos / screen_col,
				   screen_pos % screen_col,
				   ' ', background);
		}
		break;
	case '\n':
		screen_pos += screen_col;
		/* faullthru */
	case '\r':
		screen_pos -= (screen_pos % screen_col);
		break;
	case '\t':
		cons_putc(' ');
		cons_putc(' ');
		cons_putc(' ');
		cons_putc(' ');
		cons_putc(' ');
		break;
	default:
		draw_ascii(screen_pos / screen_col,
			   screen_pos % screen_col,
			   ch, ch == ' ' ? background : foreground);
		screen_pos++;
		break;
	}

tryscroll:
	/* Scroll the screen */
	if (screen_pos >= screen_size) {
		int i;

		memmove((void *)screen_base, (void *)(screen_base + font_asc_height * screen_width * screen_bpp),
			(screen_width * (screen_height - 1) * screen_bpp));

		for (i = screen_size - screen_col; i < screen_size; i++)
			draw_ascii(i / screen_col, i % screen_col, ' ', background);
		screen_pos -= screen_col;
	}
}

enum bga {
	bga_ioport_index = 0x01ce,
	bga_ioport_data = 0x01cf,

	bga_index_xres = 1,
	bga_index_yres = 2,
	bga_index_bpp = 3,
	bga_index_enable = 4,

	bga_enabled = 1,
	bga_disabled = 0,
	bga_lfb_enabled = 0x40,
//	bga_noclearmem = 0x80,
};

static void
bga_writereg(uint16_t index, uint16_t data)
{
	outw(bga_ioport_index, index);
	outw(bga_ioport_data, data);
}

//bga_readreg(uint16_t index)
//{
//	outw(bga_ioport_index, index);
//	return inw(bga_ioport_data);
//}

/* This function set the mode, resolution, etc. of the BGA */
void
bga_setmode(uint32_t width, uint32_t height, uint32_t bytedepth)
{
	bga_writereg(bga_index_enable, bga_disabled); /* Must be off before configuring */
	bga_writereg(bga_index_xres, width);
	bga_writereg(bga_index_yres, height);
	bga_writereg(bga_index_bpp, bytedepth * 8);
	bga_writereg(bga_index_enable,bga_enabled | bga_lfb_enabled); /* Turn on */
}


/* This function is called before enabling paging */
void
bga_init(void)
{
	int i, j;
	foreground = 0xFFFFFF;	/* XXX:Might add sys_call for color setting later */
	background = 0x000000;
	bga_setmode(screen_width, screen_height, screen_bpp);
	screen_base = BGA_LFB_PA + KERNBASE; /* Temporary */
	for (i = 0; i < screen_height; i++) /* Set up background */
		for (j = 0; j < screen_width; j++)
			draw_pixle(j, i, background);
}

/* Before Paging enabled, the segmentation
 * doesn't produce identity mapping. So We
 * have to map it again after paging enabled
 */
void
bga_init2(void)
{
	int i;
	pte_t *pte;
	for (i = 0; i < BGA_LFB_SIZE; i += PGSIZE) {
		/* This snippet could get reduced by directly manipulating vpt&vpd
		 * but not as readable, so put up with it :)
		 */
		pte = pgdir_walk(boot_pgdir, (void *)(BGA_LFB_VA + i), 1);
		*pte = (BGA_LFB_PA + i ) | PTE_W | PTE_P; 
	}
	screen_base = BGA_LFB_VA;
}
