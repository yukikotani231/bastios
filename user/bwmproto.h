/* The protocol between bwm and the programs that open windows on it, over
 * a UNIX stream socket at BWM_SOCK. Every message is a bwm_hdr (type and
 * the length of what follows) and a payload; all words are 32 bits,
 * little-endian. A window's pixels are kept by bwm, so a program draws by
 * sending rectangles of pixels (0x00RRGGBB), and gets keys and pointer
 * events back. A program with a lot to draw can instead give bwm a
 * memory file (memfd) with BWM_CREATE_SHM, sent with SCM_RIGHTS beside the
 * message: the window's pixels are then the file's first w * h words,
 * row by row, and a BWM_DRAW with no pixels after it says which rectangle
 * of them changed. */
#ifndef BWMPROTO_H
#define BWMPROTO_H
#include <stdint.h>

#define BWM_SOCK "/tmp/bwm.sock"
#define BWM_MAX_PAYLOAD 65536

/* Client to server. */
enum { BWM_CREATE = 1, BWM_DRAW = 2, BWM_DESTROY = 3, BWM_SETTITLE = 4, BWM_CREATE_SHM = 5 };
/* Server to client. */
enum { BWM_CREATED = 1, BWM_KEY = 2, BWM_MOUSE = 3, BWM_CLOSE = 4 };

struct bwm_hdr { uint32_t type, len; };
struct bwm_create { uint32_t w, h; char title[24]; };        /* the size of the contents */
struct bwm_created { uint32_t id, w, h; };
struct bwm_draw { uint32_t id, x, y, w, h; };                  /* then w * h pixels, or none for a shared window */
struct bwm_destroy { uint32_t id; };
struct bwm_settitle { uint32_t id; char title[24]; };
struct bwm_key { uint32_t id, ch; };
struct bwm_mouse { uint32_t id, x, y, buttons; };              /* x, y inside the contents */
struct bwm_close { uint32_t id; };

#endif
