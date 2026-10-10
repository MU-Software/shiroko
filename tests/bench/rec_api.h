/* Call recording (rec.h, kind calls) by name: bench code includes this after <shiroko/shiroko.h> and before the code
 * whose calls it records, which then calls the rec_shr_* wrappers below. They record into rec_api_rec while it is set
 * and recording, then call the library. Only the calls that change what is drawn are recorded; the asynchronous host
 * signals (shr_output_released, shr_fence_signal, shr_asset_complete, ...) are not, so a recorded host is synchronous.
 * The library never includes this header. */
#ifndef SHIROKO_BENCH_REC_API_H
#define SHIROKO_BENCH_REC_API_H

#include <shiroko/shiroko.h>

shr_status rec_shr_create(const shr_context_desc *desc, shr_context **out_ctx);
shr_status rec_shr_screen_configure(shr_context *ctx, const shr_screen_desc *desc);
shr_status rec_shr_submit(shr_context *ctx);
shr_status rec_shr_pump(shr_context *ctx);
shr_status rec_shr_poll_event(shr_context *ctx, shr_event *out);
shr_status rec_shr_request_redraw(shr_context *ctx);
shr_status rec_shr_lyr_create(shr_context *ctx, int32_t z, shr_rect rect, shr_lyr **out);
shr_status rec_shr_lyr_set_rect(shr_lyr *layer, shr_rect rect);
shr_status rec_shr_lyr_set_z(shr_lyr *layer, int32_t z);
shr_status rec_shr_lyr_set_visible(shr_lyr *layer, bool visible);
shr_status rec_shr_lyr_destroy(shr_lyr *layer);
shr_status rec_shr_lyr_cmd_begin(shr_lyr *layer);
shr_status rec_shr_lyr_cmd_fill(shr_lyr *layer, shr_rect rect, shr_color color);
shr_status rec_shr_lyr_cmd_image(shr_lyr *layer, shr_pl_res_image *image, shr_rect src, shr_point at);
shr_status rec_shr_lyr_cmd_commit(shr_lyr *layer);
shr_status rec_shr_pl_res_image_create(shr_context *ctx, int32_t width, int32_t height, const void *rgba, size_t stride,
                                       shr_pl_res_image **out);
shr_status rec_shr_pl_res_image_create_from(shr_context *ctx, const shr_image_source *source, shr_pl_res_image **out);
shr_status rec_shr_pl_res_image_update(shr_pl_res_image *image, shr_rect rect, const void *rgba, size_t stride);
shr_status rec_shr_pl_res_image_release(shr_pl_res_image *image);
shr_status rec_shr_pl_res_image_create_scaled(shr_context *ctx, const shr_image_source *source, shr_rect src,
                                              int32_t width, int32_t height, uint32_t filter, shr_pl_res_image **out);
shr_status rec_shr_pl_res_image_view(shr_pl_res_image *image, shr_rect src, int32_t width, int32_t height,
                                     uint32_t flags, shr_pl_res_image **out);
shr_status rec_shr_pl_res_bitmap_font_create(shr_context *ctx, const shr_pl_res_bitmap_font_desc *desc,
                                             shr_pl_res_bitmap_font **out);
shr_status rec_shr_pl_res_bitmap_font_destroy(shr_pl_res_bitmap_font *font);
shr_status rec_shr_pl_res_bitmap_font_preload(shr_pl_res_bitmap_font *font, const char *package, uint32_t pages);
shr_status rec_shr_pl_lyr_tilemap_resize(shr_lyr *layer, shr_pl_res_bitmap_font *font, int32_t rows, int32_t cols,
                                         const shr_color *background);
shr_status rec_shr_pl_lyr_tilemap_set_cell(shr_lyr *layer, int32_t row, int32_t col, const char *utf8, size_t length,
                                           uint32_t span, shr_text_style style);
shr_status rec_shr_pl_lyr_tilemap_set_text(shr_lyr *layer, int32_t row, int32_t col, const char *utf8, size_t length,
                                           shr_text_style style, const shr_style_run *runs, size_t run_count,
                                           uint32_t flags, shr_error_info *err);
shr_status rec_shr_pl_lyr_tilemap_clear(shr_lyr *layer, int32_t row, int32_t col, int32_t rows, int32_t cols,
                                        shr_text_style style);
shr_status rec_shr_pl_lyr_tilemap_scroll(shr_lyr *layer, int32_t top, int32_t bottom, int32_t n, shr_text_style style);
shr_status rec_shr_pl_lyr_tilemap_set_lines(shr_lyr *layer, int32_t row, const shr_text_line *lines, size_t count,
                                            shr_error_info *err);
shr_status rec_shr_pl_lyr_tilemap_set_row(shr_lyr *layer, int32_t row, int32_t col, const shr_row *in,
                                          shr_error_info *err);

#ifndef REC_API_IMPL
#define shr_create rec_shr_create
#define shr_screen_configure rec_shr_screen_configure
#define shr_submit rec_shr_submit
#define shr_pump rec_shr_pump
#define shr_poll_event rec_shr_poll_event
#define shr_request_redraw rec_shr_request_redraw
#define shr_lyr_create rec_shr_lyr_create
#define shr_lyr_set_rect rec_shr_lyr_set_rect
#define shr_lyr_set_z rec_shr_lyr_set_z
#define shr_lyr_set_visible rec_shr_lyr_set_visible
#define shr_lyr_destroy rec_shr_lyr_destroy
#define shr_lyr_cmd_begin rec_shr_lyr_cmd_begin
#define shr_lyr_cmd_fill rec_shr_lyr_cmd_fill
#define shr_lyr_cmd_image rec_shr_lyr_cmd_image
#define shr_lyr_cmd_commit rec_shr_lyr_cmd_commit
#define shr_pl_res_image_create rec_shr_pl_res_image_create
#define shr_pl_res_image_create_from rec_shr_pl_res_image_create_from
#define shr_pl_res_image_update rec_shr_pl_res_image_update
#define shr_pl_res_image_release rec_shr_pl_res_image_release
#define shr_pl_res_image_create_scaled rec_shr_pl_res_image_create_scaled
#define shr_pl_res_image_view rec_shr_pl_res_image_view
#define shr_pl_res_bitmap_font_create rec_shr_pl_res_bitmap_font_create
#define shr_pl_res_bitmap_font_destroy rec_shr_pl_res_bitmap_font_destroy
#define shr_pl_res_bitmap_font_preload rec_shr_pl_res_bitmap_font_preload
#define shr_pl_lyr_tilemap_resize rec_shr_pl_lyr_tilemap_resize
#define shr_pl_lyr_tilemap_set_cell rec_shr_pl_lyr_tilemap_set_cell
#define shr_pl_lyr_tilemap_set_text rec_shr_pl_lyr_tilemap_set_text
#define shr_pl_lyr_tilemap_clear rec_shr_pl_lyr_tilemap_clear
#define shr_pl_lyr_tilemap_scroll rec_shr_pl_lyr_tilemap_scroll
#define shr_pl_lyr_tilemap_set_lines rec_shr_pl_lyr_tilemap_set_lines
#define shr_pl_lyr_tilemap_set_row rec_shr_pl_lyr_tilemap_set_row
#endif

#endif
