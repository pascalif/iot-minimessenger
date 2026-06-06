// Fonts with accents.
// - see howto_font to generate them
// - effective pixels size are in defines below
#include "fonts/FreeSans05pt8b_latin1.h"
#include "fonts/FreeSans06pt8b_latin1.h"
#include "fonts/FreeSans07pt8b_latin1.h"
#include "fonts/FreeSans08pt8b_latin1.h"
#include "fonts/FreeSans09pt8b_latin1.h"
#include "fonts/FreeSans10pt8b_latin1.h"

// Aliases. First number = high in pixels above the baseline ; 2nd number = below a baseline (for 'j', 'p', ...)
// Rem: 2nd value is not exactly correct. CF complex char that may use more bottom rows in the latin range [0x20, 0xFF] : '¿', 'Ç', '{', '¡'

#define FONT_DEFAULT_07_0PX     nullptr
#define FREESANS_ACCENTS_08_1PX &FreeSans5pt8b
#define FREESANS_ACCENTS_09_2PX &FreeSans6pt8b
#define FREESANS_ACCENTS_10_2PX &FreeSans7pt8b
#define FREESANS_ACCENTS_12_3PX &FreeSans8pt8b
#define FREESANS_ACCENTS_13_3PX &FreeSans9pt8b
#define FREESANS_ACCENTS_15_3PX &FreeSans10pt8b
// A "null" police == Glcdfont, une police bitmap 5x7 pixels fixe, définie dans glcdfont.c., et non accessible à travers une variable
