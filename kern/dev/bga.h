#ifndef JOS_KERN_BGA_H
#define JOS_KERN_BGA_H
#ifndef JOS_KERNEL
# error "This is a JOS kernel header; user programs should not #include it"
#endif

#define BGA

enum font {
	font_asc_width = 8,
	font_asc_num = 256,
	font_asc_height = 16,

	font_chs_qu = 87,
	font_chs_wei = 94,
	font_chs_width = 16,
	font_chs_height = 16,
	font_chs_num = font_chs_qu * font_chs_wei,
};

extern uint8_t ASC[font_asc_num][font_asc_height];
extern uint16_t CHS[font_chs_num][font_chs_height];

#define BGA_LFB_PA 0xE0000000
#define BGA_LFB_VA 0xFEF00000

uintptr_t screen_base;	/* This is _virtual_ address address */

enum screen {
	screen_width = 640,		/* So 80 columns */
	screen_height = 480,		/* So 30 rows */
	screen_col = screen_width / font_asc_width,
	screen_row = screen_height / font_asc_height,
	screen_size = screen_row * screen_col,
	screen_bpp = 4,		/* Byte Per Pixel */
};

#define BGA_LFB_SIZE (screen_width * screen_height * screen_bpp)
void bga_init(void);
void bga_init2(void);
void bga_putc(int c);

#endif	/* !JOS_KERN_BGA_H */
