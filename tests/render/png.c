/* stb_image_write on its own: its arithmetic wraps on purpose, so this file is built without -fsanitize=integer. */
#include <shiroko/shiroko.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

shr_status png_write(const char *path, int32_t w, int32_t h, const uint8_t *rgb);

shr_status png_write(const char *path, int32_t w, int32_t h, const uint8_t *rgb) {
    return stbi_write_png(path, w, h, 3, rgb, w * 3) ? SHR_OK : SHR_E_IO;
}
