// Public headers compile and link as C++ (extern "C", no C-only constructs).
#include <shiroko/port_software.h>
#include <shiroko/shiroko.h>

#include <cstdio>

#include <greatest.h>

TEST test_headers(void) {
    shr_text_profile_info info = {};
    ASSERT_EQ(SHR_OK, shr_pl_lyr_tilemap_profile_get(&info));
    shr_framebuffer_driver drv;
    ASSERT_EQ(SHR_OK, shr_software_driver_create(nullptr, 0, 4, &drv));
    std::printf("unicode %s, cell %dx%d\n", info.unicode_version, SHR_CELL_WIDTH, SHR_CELL_HEIGHT);
    ASSERT(drv.execute);
    ASSERT_EQ(4u, drv.caps.max_buffers);
    ASSERT_EQ(0u, drv.caps.buffer_flags & SHR_BUFFER_COPIES);
    uint8_t px[4] = {}, a8[1] = {255};
    shr_surface s = {px, 1, 1, 4, 4, SHR_FORMAT_RGBX8888, 0, SHR_MEMORY_CPU, 0};
    shr_draw_cmd c[2] = {};
    c[0].kind = SHR_CMD_BUFFER_REGISTER, c[0].buffer = 1;
    c[0].src = shr_image{a8, 1, 1, 1, 1, SHR_FORMAT_A8, SHR_MEMORY_CPU};
    c[1].kind = SHR_CMD_GLYPH, c[1].dst = shr_rect{0, 0, 1, 1}, c[1].color = SHR_RGB(1, 2, 3), c[1].buffer = 1;
    c[1].src_rect = shr_rect{0, 0, 1, 1};
    ASSERT_EQ(SHR_OK, drv.execute(drv.user, &s, c, 2, 1));
    ASSERT_EQ(3, px[2]);
    ASSERT_EQ(SHR_OK, shr_software_execute(&s, c, 2, &c[0].src, 1));
    ASSERT_EQ(0xFF010203u, SHR_RGB(1, 2, 3));
    ASSERT_STR_EQ("LIMIT", shr_status_name(SHR_E_LIMIT));
    shr_text_cluster clusters[2];
    shr_text_extent extent = {};
    ASSERT_EQ(SHR_OK, shr_pl_lyr_tilemap_measure("a\xEA\xB0\x80", 4, 8, SHR_TEXT_WRAP, clusters, 2, &extent, nullptr));
    ASSERT_EQ(1, extent.rows);
    ASSERT_EQ(3, extent.cols);
    ASSERT_EQ(2, clusters[1].cells);
    ASSERT_EQ(SHR_OK, shr_software_driver_destroy(&drv));
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(test_headers);
    GREATEST_MAIN_END();
}
