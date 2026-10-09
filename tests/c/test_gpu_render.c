/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Offscreen rendering: does the device come up, does it clear, does it rasterise.
 *
 * TWO THINGS THIS TEST IS DELIBERATELY BUILT AROUND.
 *
 * First, it SKIPS rather than fails when there is no Vulkan. A fresh clone on a
 * machine with no loader and no driver must still go green, so an absent GPU
 * exits 0 with a stated reason. The thing that would be useless is a test that
 * cannot tell "no Vulkan here" from "Vulkan is broken here", so the two outcomes
 * print differently and only the second is a failure.
 *
 * Second, it checks SHAPE and ORIENTATION, not merely "some pixels changed".
 * This project has a recorded incident of three numeric passes while every model
 * rendered as a white silhouette. A triangle drawn upside down, mirrored, or with
 * its vertex colours swapped passes every "is this pixel non-background" check
 * ever written. So the assertions below pin the apex to the top, pin each named
 * colour to its own corner, and pin the covered area to the area the geometry
 * actually implies. The PNGs are written as well, because the final check on a
 * picture is a human opening it.
 */

#include "gpu_device.h"
#include "gpu_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

static void check(bool condition, const char *what)
{
    checks++;
    if (condition) {
        printf("  ok   %s\n", what);
    } else {
        failures++;
        printf("  FAIL %s\n", what);
    }
}

static void check_detail(bool condition, const char *what, const char *detail)
{
    checks++;
    if (condition) {
        printf("  ok   %s (%s)\n", what, detail);
    } else {
        failures++;
        printf("  FAIL %s (%s)\n", what, detail);
    }
}

/* The clear colour for the clear test. Every component is a multiple of 0.2, and
 * 0.2 * 255 is exactly 51, so an UNORM target reproduces each one as a whole
 * number. Picking 0.1 instead would make every expected value a rounding
 * argument, and a test that argues about rounding stops catching real faults. */
static const float clear_colour[4] = { 0.2f, 0.4f, 0.8f, 1.0f };
static const uint8_t clear_expected[4] = { 51u, 102u, 204u, 255u };

/* The triangle sits on this, and 0.2 -> 51 for the same reason. Not black: a
 * black background makes "cleared correctly" and "never rendered" look identical. */
static const float background_colour[4] = { 0.2f, 0.2f, 0.2f, 1.0f };
static const uint8_t background_expected[4] = { 51u, 51u, 51u, 255u };

#define TRIANGLE_WIDTH 256u
#define TRIANGLE_HEIGHT 192u

/* Rasterisation and attribute interpolation are not bit-exact across drivers, so
 * colour comparisons inside the triangle get a tolerance. It is deliberately
 * tight: anything looser would stop distinguishing red from magenta. */
#define COLOUR_TOLERANCE 10

static const uint8_t *pixel_at(const gpu_image *image, uint32_t x, uint32_t y)
{
    return image->pixels + gpu_image_offset(image, x, y);
}

static bool pixel_equals(const gpu_image *image, uint32_t x, uint32_t y,
                         const uint8_t expected[4])
{
    return memcmp(pixel_at(image, x, y), expected, 4u) == 0;
}

static bool is_background(const gpu_image *image, uint32_t x, uint32_t y)
{
    return pixel_equals(image, x, y, background_expected);
}

static int absolute_difference(uint8_t left, uint8_t right)
{
    return left > right ? (int)left - (int)right : (int)right - (int)left;
}

/* "Dominant" rather than "equal to pure red" because only the exact vertex pixel
 * is pure, and the exact vertex pixel is on the edge where coverage rules decide
 * whether it exists at all. Sampling just inside and asking which channel wins is
 * the robust form of the same question. */
static bool channel_dominates(const gpu_image *image, uint32_t x, uint32_t y,
                              unsigned channel)
{
    const uint8_t *pixel = pixel_at(image, x, y);
    for (unsigned other = 0; other < 3u; other++) {
        if (other == channel) {
            continue;
        }
        if (pixel[channel] <= pixel[other] + 40u) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */

static void test_device_reports_a_name_and_api_version(gpu_device *device)
{
    printf("test_device_reports_a_name_and_api_version\n");
    const char *name = gpu_device_name(device);
    check(name != NULL && name[0] != '\0', "physical device has a non-empty name");
    /* VK_API_VERSION_1_0 packed, spelled out rather than included, because
     * gpu_device.h deliberately keeps vulkan.h out of its consumers. */
    check(gpu_device_api_version(device) >= (1u << 22),
          "device reports at least Vulkan 1.0");
    printf("       device: %s, api %u.%u.%u\n", name,
           gpu_device_api_version(device) >> 22,
           (gpu_device_api_version(device) >> 12) & 0x3ffu,
           gpu_device_api_version(device) & 0xfffu);
}

static void test_clear_fills_every_pixel_with_the_requested_colour(gpu_device *device,
                                                                   const char *png_path)
{
    printf("test_clear_fills_every_pixel_with_the_requested_colour\n");
    gpu_image image = { 0 };
    gpu_result outcome = gpu_render_clear(device, 64u, 48u, clear_colour, &image);
    check_detail(outcome == GPU_OK, "gpu_render_clear succeeded",
                 gpu_result_string(outcome));
    if (outcome != GPU_OK) {
        return;
    }

    check(image.width == 64u && image.height == 48u, "returned image has the requested size");
    check(image.stride_bytes >= image.width * 4u, "stride covers at least one row of pixels");

    /* Every pixel, not a sample. The target is 3 KB, so there is no reason to
     * check a corner and hope, and a stray row from a bad render area or a bad
     * stride is exactly the kind of fault sampling misses. */
    size_t wrong = 0;
    uint32_t first_wrong_x = 0;
    uint32_t first_wrong_y = 0;
    for (uint32_t y = 0; y < image.height; y++) {
        for (uint32_t x = 0; x < image.width; x++) {
            if (!pixel_equals(&image, x, y, clear_expected)) {
                if (wrong == 0) {
                    first_wrong_x = x;
                    first_wrong_y = y;
                }
                wrong++;
            }
        }
    }
    char detail[128];
    if (wrong == 0) {
        snprintf(detail, sizeof detail, "all %u pixels are exactly %u,%u,%u,%u",
                 image.width * image.height, clear_expected[0], clear_expected[1],
                 clear_expected[2], clear_expected[3]);
    } else {
        const uint8_t *bad = pixel_at(&image, first_wrong_x, first_wrong_y);
        snprintf(detail, sizeof detail, "%zu wrong, first at %u,%u = %u,%u,%u,%u",
                 wrong, first_wrong_x, first_wrong_y, bad[0], bad[1], bad[2], bad[3]);
    }
    check_detail(wrong == 0, "every pixel is the exact requested colour", detail);

    check(gpu_png_write_rgba(png_path, image.pixels, image.width, image.height,
                             image.stride_bytes),
          "clear target written as a PNG");
    printf("       wrote %s -- open it, it should be flat medium blue\n", png_path);

    gpu_image_free(&image);
    check(image.pixels == NULL, "gpu_image_free releases and zeroes the image");
}

static void test_triangle_vertex_colours_reach_their_own_corners(const gpu_image *image)
{
    printf("test_triangle_vertex_colours_reach_their_own_corners\n");
    /* Derived from the clip-space positions in src/gpu/shaders/triangle.vert, not
     * from what the renderer happened to produce. A pixel coordinate copied out of
     * a previous run would make this test agree with any bug it was born with.
     *
     *   pixel_x = (ndc_x + 1) / 2 * width,  pixel_y = (ndc_y + 1) / 2 * height
     *
     * apex      ndc ( 0.0, -0.7) -> (128,  29)
     * bottom-r  ndc ( 0.7,  0.7) -> (218, 163)
     * bottom-l  ndc (-0.7,  0.7) -> ( 38, 163)
     *
     * Each sample is nudged a few pixels toward the centroid so it is safely
     * inside the edge rather than on it. */
    check(channel_dominates(image, 128u, 36u, 0u), "apex region is red-dominant");
    check(channel_dominates(image, 210u, 157u, 1u), "bottom-right region is green-dominant");
    check(channel_dominates(image, 46u, 157u, 2u), "bottom-left region is blue-dominant");

    /* The centroid of a triangle with one pure channel per vertex is an equal
     * third of each, so roughly 85,85,85. This is what proves INTERPOLATION rather
     * than three flat-shaded regions. */
    const uint8_t *centre = pixel_at(image, TRIANGLE_WIDTH / 2u, 119u);
    bool even = absolute_difference(centre[0], centre[1]) < COLOUR_TOLERANCE &&
                absolute_difference(centre[1], centre[2]) < COLOUR_TOLERANCE &&
                absolute_difference(centre[0], 85u) < COLOUR_TOLERANCE * 2;
    char detail[96];
    snprintf(detail, sizeof detail, "centroid = %u,%u,%u, expected about 85,85,85",
             centre[0], centre[1], centre[2]);
    check_detail(even, "centroid is an even three-way blend", detail);
}

static void test_triangle_apex_points_up(const gpu_image *image)
{
    printf("test_triangle_apex_points_up\n");
    /* THE ORIENTATION CHECK. Vulkan clip space has +Y down, and getting that
     * backwards produces a perfectly good upside-down triangle that satisfies
     * every coverage and colour-dominance assertion written so far. The shape
     * itself has to be measured.
     *
     * Count covered pixels per row. For an apex-up triangle the first covered row
     * is nearly empty and the last is nearly the full base. Flipping the image
     * swaps those two numbers, so comparing them is a direct test of orientation. */
    uint32_t first_row = image->height;
    uint32_t last_row = 0;
    uint32_t first_row_width = 0;
    uint32_t last_row_width = 0;
    size_t covered = 0;

    for (uint32_t y = 0; y < image->height; y++) {
        uint32_t row_width = 0;
        for (uint32_t x = 0; x < image->width; x++) {
            if (!is_background(image, x, y)) {
                row_width++;
            }
        }
        covered += row_width;
        if (row_width > 0u) {
            if (first_row == image->height) {
                first_row = y;
                first_row_width = row_width;
            }
            last_row = y;
            last_row_width = row_width;
        }
    }

    char detail[128];
    snprintf(detail, sizeof detail, "top row %u is %u px wide, bottom row %u is %u px wide",
             first_row, first_row_width, last_row, last_row_width);
    check_detail(last_row_width > first_row_width * 10u,
                 "the triangle is far wider at the bottom than at the top", detail);

    /* The apex is at ndc y = -0.7 and the base at +0.7, so the covered band should
     * start near row 29 and end near row 163. Pinning both ends catches a vertical
     * flip that somehow preserved the taper, and catches a wrong viewport. */
    snprintf(detail, sizeof detail, "covered rows %u..%u, expected about 29..163",
             first_row, last_row);
    check_detail(first_row >= 24u && first_row <= 34u && last_row >= 158u && last_row <= 168u,
                 "the triangle occupies the rows the geometry implies", detail);

    /* Area of the triangle in pixels: base 1.4 * width / 2, height 1.4 * height / 2,
     * halved, which is 0.245 * width * height. A coverage check alone proves little,
     * but combined with the taper and the row span it pins the shape. */
    double expected_area = 0.245 * (double)image->width * (double)image->height;
    double ratio = (double)covered / expected_area;
    snprintf(detail, sizeof detail, "%zu px covered, geometry implies %.0f, ratio %.3f",
             covered, expected_area, ratio);
    check_detail(ratio > 0.95 && ratio < 1.05,
                 "covered area matches the triangle's computed area", detail);

    /* Red must sit above green and blue. This is the colour half of the same
     * question, and it is what catches a vertex array rotated by one. */
    double red_row_sum = 0.0;
    double other_row_sum = 0.0;
    size_t red_count = 0;
    size_t other_count = 0;
    for (uint32_t y = 0; y < image->height; y++) {
        for (uint32_t x = 0; x < image->width; x++) {
            if (is_background(image, x, y)) {
                continue;
            }
            const uint8_t *pixel = pixel_at(image, x, y);
            if (pixel[0] > pixel[1] && pixel[0] > pixel[2]) {
                red_row_sum += (double)y;
                red_count++;
            } else {
                other_row_sum += (double)y;
                other_count++;
            }
        }
    }
    bool red_is_above = red_count > 0 && other_count > 0 &&
                        (red_row_sum / (double)red_count) <
                            (other_row_sum / (double)other_count);
    snprintf(detail, sizeof detail, "red centroid row %.1f, green/blue centroid row %.1f",
             red_count ? red_row_sum / (double)red_count : -1.0,
             other_count ? other_row_sum / (double)other_count : -1.0);
    check_detail(red_is_above, "the red vertex is above the green and blue ones", detail);
}

static void test_triangle_leaves_the_background_untouched(const gpu_image *image)
{
    printf("test_triangle_leaves_the_background_untouched\n");
    /* All four corners are outside a triangle inset to 0.7 of the frame. If any of
     * them changed, either the clear colour is wrong or something drew outside the
     * primitive. */
    check(is_background(image, 0u, 0u), "top-left corner is still the clear colour");
    check(is_background(image, image->width - 1u, 0u), "top-right corner is still the clear colour");
    check(is_background(image, 0u, image->height - 1u), "bottom-left corner is still the clear colour");
    check(is_background(image, image->width - 1u, image->height - 1u),
          "bottom-right corner is still the clear colour");
    check(is_background(image, image->width / 2u, 2u), "above the apex is still the clear colour");
    check(is_background(image, image->width / 2u, image->height - 3u),
          "below the base is still the clear colour");
}

static void test_triangle_is_written_as_a_png(const gpu_image *image, const char *png_path)
{
    printf("test_triangle_is_written_as_a_png\n");
    check(gpu_png_write_rgba(png_path, image->pixels, image->width, image->height,
                             image->stride_bytes),
          "triangle target written as a PNG");

    /* Re-open it and check the signature. The PNG writer has its own thorough
     * test, so this only confirms that a real file with real bytes landed where
     * this test said it would, which is what a human needs in order to look. */
    FILE *file = fopen(png_path, "rb");
    check(file != NULL, "the PNG file exists and can be reopened");
    if (file) {
        unsigned char signature[8] = { 0 };
        size_t read = fread(signature, 1u, sizeof signature, file);
        static const unsigned char expected[8] = { 0x89u, 'P', 'N', 'G', '\r', '\n', 0x1au, '\n' };
        check(read == sizeof signature && memcmp(signature, expected, sizeof expected) == 0,
              "the file starts with the PNG signature");
        fclose(file);
    }
    printf("       wrote %s -- open it: red apex at TOP, green bottom-right,\n", png_path);
    printf("       blue bottom-left, smoothly blended, on dark grey\n");
}

static void test_render_rejects_degenerate_sizes(gpu_device *device)
{
    printf("test_render_rejects_degenerate_sizes\n");
    gpu_image image = { 0 };
    check(gpu_render_clear(device, 0u, 16u, clear_colour, &image) == GPU_ERR_ARGUMENT,
          "zero width is rejected");
    check(gpu_render_clear(device, 16u, 0u, clear_colour, &image) == GPU_ERR_ARGUMENT,
          "zero height is rejected");
    check(gpu_render_clear(device, 16u, 16u, NULL, &image) == GPU_ERR_ARGUMENT,
          "a NULL clear colour is rejected");
    check(gpu_render_clear(device, 16u, 16u, clear_colour, NULL) == GPU_ERR_ARGUMENT,
          "a NULL output image is rejected");
    check(gpu_render_triangle(device, 100000u, 16u, clear_colour, &image) == GPU_ERR_ARGUMENT,
          "an absurd width is rejected rather than attempted");
    check(image.pixels == NULL, "no pixels were allocated by a rejected request");
}

/* ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    /* CTest runs this from the build directory, so a bare filename lands
     * somewhere findable. An explicit directory can be passed for a manual run. */
    const char *directory = (argc > 1) ? argv[1] : ".";
    char clear_png[512];
    char triangle_png[512];
    snprintf(clear_png, sizeof clear_png, "%s/gpu_clear.png", directory);
    snprintf(triangle_png, sizeof triangle_png, "%s/gpu_triangle.png", directory);

    if (!gpu_vulkan_available()) {
        printf("SKIP: no Vulkan loader on this machine, nothing to test\n");
        printf("      (this is a clean skip, not a failure -- the host half of\n");
        printf("       the graphics boundary is an optional component)\n");
        return 0;
    }

    gpu_device *device = NULL;
    gpu_result outcome = gpu_device_create(&device);
    if (outcome == GPU_ERR_NO_LOADER || outcome == GPU_ERR_NO_PHYSICAL_DEVICE ||
        outcome == GPU_ERR_NO_GRAPHICS_QUEUE) {
        /* A loader with no usable driver behind it. Still a skip: the machine
         * cannot render, and that is a fact about the machine. */
        printf("SKIP: %s\n", gpu_result_string(outcome));
        return 0;
    }
    if (outcome != GPU_OK) {
        /* Anything else means Vulkan is present and went wrong, which is a real
         * failure and must not be swallowed by the skip path above. */
        printf("FAIL: gpu_device_create: %s\n", gpu_result_string(outcome));
        return 1;
    }

    test_device_reports_a_name_and_api_version(device);
    test_clear_fills_every_pixel_with_the_requested_colour(device, clear_png);

    gpu_image triangle = { 0 };
    outcome = gpu_render_triangle(device, TRIANGLE_WIDTH, TRIANGLE_HEIGHT,
                                  background_colour, &triangle);
    printf("test_triangle_renders\n");
    check_detail(outcome == GPU_OK, "gpu_render_triangle succeeded",
                 gpu_result_string(outcome));
    if (outcome == GPU_OK) {
        test_triangle_vertex_colours_reach_their_own_corners(&triangle);
        test_triangle_apex_points_up(&triangle);
        test_triangle_leaves_the_background_untouched(&triangle);
        test_triangle_is_written_as_a_png(&triangle, triangle_png);
        gpu_image_free(&triangle);
    }

    test_render_rejects_degenerate_sizes(device);

    gpu_device_destroy(device);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
