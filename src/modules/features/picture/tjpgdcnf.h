/*----------------------------------------------*/
/* TJpgDec System Configurations R0.03          */
/*----------------------------------------------*/
/* Tuned for the Game & Watch Picture Viewer module: RGB565 output straight into
   the 320x240 framebuffer, in-decode descaling for large photos, and the 32-bit
   barrel-shifter path for the Cortex-M7. Stock TJpgDec otherwise (R0.03, ChaN). */

#define	JD_SZBUF		512
/* Size of the stream input buffer (allocated from the work pool). */

#define JD_FORMAT		1
/* Output pixel format. 0: RGB888  1: RGB565  2: Grayscale.
   1 (RGB565) matches the LTDC framebuffer, so blits are a straight halfword copy. */

#define	JD_USE_SCALE	1
/* Enable output descaling (1/1, 1/2, 1/4, 1/8) so oversized images decode small. */

#define JD_TBLCLIP		1
/* Table-based saturation (a bit faster, +~1 KB code). */

#define JD_FASTDECODE	1
/* 1: + 32-bit barrel shifter (suits the M7); no extra RAM vs level 0.
   (Level 2 would add a 6 KB huffman LUT; not worth the pool here.) */
