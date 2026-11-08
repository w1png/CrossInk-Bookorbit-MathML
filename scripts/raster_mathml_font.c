#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void write_i16(int value) {
  const uint16_t bits = (uint16_t)value;
  fputc(bits & 255, stdout);
  fputc(bits >> 8, stdout);
}

int main(int argc, char** argv) {
  FT_Library library;
  FT_Face face;
  if (argc != 3 || FT_Init_FreeType(&library) || FT_New_Face(library, argv[1], 0, &face) ||
      FT_Set_Pixel_Sizes(face, 0, (unsigned)atoi(argv[2]))) {
    return 1;
  }

  for (FT_UInt glyphId = 0; glyphId < face->num_glyphs; ++glyphId) {
    if (FT_Load_Glyph(face, glyphId, FT_LOAD_TARGET_MONO | FT_LOAD_NO_BITMAP) ||
        FT_Render_Glyph(face->glyph, FT_RENDER_MODE_MONO)) {
      return 2;
    }

    const FT_GlyphSlot glyph = face->glyph;
    const FT_Bitmap bitmap = glyph->bitmap;
    const unsigned stride = (bitmap.width + 7) / 8;
    if (stride && bitmap.pixel_mode != FT_PIXEL_MODE_MONO) {
      return 3;
    }

    write_i16(glyph->bitmap_left);
    write_i16(glyph->bitmap_top);
    write_i16((int)bitmap.width);
    write_i16((int)bitmap.rows);
    for (unsigned row = 0; row < bitmap.rows; ++row) {
      if (!stride) continue;
      const unsigned char* pixels = bitmap.pitch >= 0 ? bitmap.buffer + row * bitmap.pitch
                                                      : bitmap.buffer + (bitmap.rows - 1 - row) * -bitmap.pitch;
      fwrite(pixels, 1, stride - 1, stdout);
      fputc(pixels[stride - 1] & (255 << ((8 - bitmap.width % 8) % 8)), stdout);
    }
  }

  FT_Done_Face(face);
  FT_Done_FreeType(library);
  return ferror(stdout) ? 4 : 0;
}
