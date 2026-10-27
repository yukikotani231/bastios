/* The protocol between bwm and the programs that open windows on it, over
 * a UNIX stream socket at BWM_SOCK. Every message is a bwm_hdr (type and
 * the length of what follows) and a payload; all words are 32 bits,
 * little-endian. A window's pixels are kept by bwm, so a program draws by
 * sending rectangles of pixels (0x00RRGGBB), and gets keys and pointer
 * events back. A program with a lot to draw can instead give bwm a
 * memory file (memfd) with BWM_CREATE_SHM, sent with SCM_RIGHTS beside the
 * message: the window's pixels are then the file's first w * h words,
 * row by row, and a BWM_DRAW with no pixels after it says which rectangle
 * of them changed.
 *
 * A window can be resized with the grip at the corner of its frame once
 * its program has said BWM_RESIZABLE. bwm asks for a size (BWM_RESIZE); the
 * program picks the size it will have (the one asked for, or a smaller one
 * that fits its cells), answers with BWM_RESIZED (a shared window sends a
 * new memory file with it, as with BWM_CREATE_SHM) and draws all of the
 * window again. The window keeps its old size until the answer comes.
 * A resizable program may also send BWM_RESIZED unasked (bterm does when
 * the text size changes). */
#ifndef BWMPROTO_H
#define BWMPROTO_H
#include <stdint.h>

#define BWM_SOCK "/tmp/bwm.sock"
#define BWM_MAX_PAYLOAD 65536

/* Client to server. */
enum { BWM_CREATE = 1, BWM_DRAW = 2, BWM_DESTROY = 3, BWM_SETTITLE = 4, BWM_CREATE_SHM = 5, BWM_RESIZABLE = 6, BWM_RESIZED = 7 };
/* Server to client. */
enum { BWM_CREATED = 1, BWM_KEY = 2, BWM_MOUSE = 3, BWM_CLOSE = 4, BWM_RESIZE = 5 };

struct bwm_hdr { uint32_t type, len; };
struct bwm_create { uint32_t w, h; char title[24]; };        /* the size of the contents */
struct bwm_created { uint32_t id, w, h; };
struct bwm_draw { uint32_t id, x, y, w, h; };                  /* then w * h pixels, or none for a shared window */
struct bwm_destroy { uint32_t id; };
struct bwm_settitle { uint32_t id; char title[24]; };
struct bwm_key { uint32_t id, ch; };                            /* ch is a byte, BWM_KEY_PASTE for Shift-Insert and Ctrl-Shift-V, or BWM_KEY_SCROLL_UP and _DOWN for Shift-Page Up and Shift-Page Down */
#define BWM_KEY_PASTE 0x100
#define BWM_KEY_SCROLL_UP 0x101
#define BWM_KEY_SCROLL_DOWN 0x102
#define BWM_KEY_SCROLL_TOP 0x103       /* Ctrl-Shift-Page Up: back to the oldest line kept */
#define BWM_KEY_SCROLL_END 0x104       /* Ctrl-Shift-Page Down: back to the live screen */
#define BWM_KEY_ZOOM_IN 0x105          /* Ctrl-plus: bigger text */
#define BWM_KEY_ZOOM_OUT 0x106         /* Ctrl-minus: smaller text */
#define BWM_KEY_ZOOM_RESET 0x107       /* Ctrl-0: the normal size */
struct bwm_mouse { uint32_t id, x, y, buttons; };              /* x, y inside the contents; signed, and outside it while a drag that began in it goes on */
struct bwm_close { uint32_t id; };
struct bwm_resizable { uint32_t id; };
struct bwm_resize { uint32_t id, w, h; };                       /* the size of the contents asked for */
struct bwm_resized { uint32_t id, w, h; };                      /* the size of the contents it now has */

#endif
