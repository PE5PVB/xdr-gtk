#ifndef XDR_FONTS_H_
#define XDR_FONTS_H_

/* Loads the embedded TTF fonts (Inter, JetBrains Mono) from the GResource
   bundle and registers them privately for the current process so the GTK CSS
   font-family lookups succeed without the user installing them system-wide. */
void fonts_load_embedded(void);

#endif
