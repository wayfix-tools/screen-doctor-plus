#define _GNU_SOURCE
#include <xcb/xcb.h>
#include <xcb/randr.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <png.h>
#include <sys/stat.h>
#include <time.h>
#include <stdarg.h>

/*
 * LD_PRELOAD: fix Time Doctor black screenshots on KDE Plasma Wayland.
 *
 * Intercepts xcb_copy_area(root -> pixmap) and replaces black content
 * with a real screenshot taken via spectacle/portal.
 *
 * Optional: set GRAB_OVERRIDE_SCREEN=N to expose only screen N to TD.
 * This intercepts XRandR monitor enumeration so TD sees a single screen.
 * N is the 0-based monitor index (order as reported by xrandr).
 */

#define LOGFILE "/tmp/grab_override.log"

static void logmsg(const char *fmt, ...) {
    FILE *f = fopen(LOGFILE, "a");
    if (!f) return;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    fprintf(f, "[%02d:%02d:%02d pid=%d] ", t->tm_hour, t->tm_min, t->tm_sec, getpid());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fflush(f);
    fclose(f);
}

/* ---- Configuration ---- */

/* -1 = show all screens, >= 0 = show only screen N */
static int target_screen = -1;
static int config_loaded = 0;

/* Geometry of the selected screen (filled from XRandR data) */
static int screen_x = 0, screen_y = 0;
static int screen_w = 0, screen_h = 0;
static int screen_geo_known = 0;

static void load_config(void) {
    if (config_loaded) return;
    config_loaded = 1;
    const char *env = getenv("GRAB_OVERRIDE_SCREEN");
    if (env) {
        target_screen = atoi(env);
        logmsg("GRAB_OVERRIDE_SCREEN=%d (single screen mode)", target_screen);
    } else {
        logmsg("GRAB_OVERRIDE_SCREEN not set (all screens mode)");
    }
}

/* ---- Function pointer types ---- */

typedef xcb_void_cookie_t (*real_xcb_copy_area_t)(
    xcb_connection_t *, xcb_drawable_t, xcb_drawable_t, xcb_gcontext_t,
    int16_t, int16_t, int16_t, int16_t, uint16_t, uint16_t);

typedef xcb_randr_get_monitors_reply_t *(*real_xcb_randr_get_monitors_reply_t)(
    xcb_connection_t *, xcb_randr_get_monitors_cookie_t, xcb_generic_error_t **);

typedef xcb_randr_get_screen_resources_current_reply_t *
    (*real_xcb_randr_get_screen_resources_current_reply_t)(
    xcb_connection_t *, xcb_randr_get_screen_resources_current_cookie_t,
    xcb_generic_error_t **);

typedef xcb_randr_get_crtc_info_reply_t *(*real_xcb_randr_get_crtc_info_reply_t)(
    xcb_connection_t *, xcb_randr_get_crtc_info_cookie_t, xcb_generic_error_t **);

static real_xcb_copy_area_t real_xcb_copy_area_fn = NULL;
static real_xcb_randr_get_monitors_reply_t real_xcb_randr_get_monitors_reply_fn = NULL;

/* ---- Helpers ---- */

static int is_root_window(xcb_connection_t *c, xcb_drawable_t drawable) {
    const xcb_setup_t *setup = xcb_get_setup(c);
    if (!setup) return 0;
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
    while (iter.rem) {
        if (iter.data->root == drawable) return 1;
        xcb_screen_next(&iter);
    }
    return 0;
}

static int take_screenshot(const char *output_path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "env -u QT_QPA_PLATFORM spectacle --background --nonotify --fullscreen --output '%s' 2>/dev/null",
             output_path);
    int ret = system(cmd);
    if (ret != 0) return -1;
    struct stat st;
    if (stat(output_path, &st) != 0 || st.st_size == 0) return -1;
    return 0;
}

static uint8_t *load_png_pixels(const char *path,
                                 int req_x, int req_y,
                                 int req_w, int req_h,
                                 int *out_size) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(fp); return NULL; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); fclose(fp); return NULL; }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return NULL;
    }

    png_init_io(png, fp);
    png_read_info(png, info);

    int png_w = png_get_image_width(png, info);
    int png_h = png_get_image_height(png, info);
    png_byte color_type = png_get_color_type(png, info);
    png_byte bit_depth = png_get_bit_depth(png, info);

    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_RGB ||
        color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    png_set_bgr(png);
    png_read_update_info(png, info);

    png_bytep *rows = malloc(sizeof(png_bytep) * png_h);
    if (!rows) { png_destroy_read_struct(&png, &info, NULL); fclose(fp); return NULL; }
    for (int i = 0; i < png_h; i++)
        rows[i] = malloc(png_get_rowbytes(png, info));
    png_read_image(png, rows);

    int data_size = req_w * req_h * 4;
    uint8_t *data = calloc(1, data_size);
    if (!data) {
        for (int i = 0; i < png_h; i++) free(rows[i]);
        free(rows);
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return NULL;
    }

    for (int y = 0; y < req_h; y++) {
        int src_y = req_y + y;
        if (src_y < 0 || src_y >= png_h) continue;
        for (int x = 0; x < req_w; x++) {
            int src_x = req_x + x;
            if (src_x < 0 || src_x >= png_w) continue;
            memcpy(&data[(y * req_w + x) * 4], &rows[src_y][src_x * 4], 4);
        }
    }

    for (int i = 0; i < png_h; i++) free(rows[i]);
    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);

    *out_size = data_size;
    return data;
}

/* ---- XRandR monitor filter ---- */

xcb_randr_get_monitors_reply_t *
xcb_randr_get_monitors_reply(xcb_connection_t *c,
                              xcb_randr_get_monitors_cookie_t cookie,
                              xcb_generic_error_t **e) {
    if (!real_xcb_randr_get_monitors_reply_fn)
        real_xcb_randr_get_monitors_reply_fn =
            (real_xcb_randr_get_monitors_reply_t)dlsym(RTLD_NEXT,
                "xcb_randr_get_monitors_reply");

    xcb_randr_get_monitors_reply_t *reply =
        real_xcb_randr_get_monitors_reply_fn(c, cookie, e);

    load_config();
    if (!reply || target_screen < 0)
        return reply;

    /* Find the target monitor in the reply */
    int n = reply->nMonitors;
    if (target_screen >= n) {
        logmsg("WARN: GRAB_OVERRIDE_SCREEN=%d but only %d monitors, ignoring filter",
                target_screen, n);
        return reply;
    }

    xcb_randr_monitor_info_iterator_t iter =
        xcb_randr_get_monitors_monitors_iterator(reply);

    /* Advance to target monitor */
    xcb_randr_monitor_info_t *target = NULL;
    for (int i = 0; iter.rem; i++) {
        if (i == target_screen) {
            target = iter.data;
            break;
        }
        xcb_randr_monitor_info_next(&iter);
    }

    if (!target) return reply;

    /* Save geometry for use in xcb_copy_area */
    screen_x = target->x;
    screen_y = target->y;
    screen_w = target->width;
    screen_h = target->height;
    screen_geo_known = 1;

    logmsg("Filtering monitors: keeping only #%d (%dx%d+%d+%d), hiding %d others",
            target_screen, screen_w, screen_h, screen_x, screen_y, n - 1);

    /* Build a new reply with only this one monitor.
     * Monitor info is: fixed struct + nOutput * sizeof(xcb_randr_output_t) */
    int outputs_size = target->nOutput * sizeof(xcb_randr_output_t);
    int monitor_data_size = sizeof(xcb_randr_monitor_info_t) + outputs_size;
    int new_reply_size = sizeof(xcb_randr_get_monitors_reply_t) + monitor_data_size;

    xcb_randr_get_monitors_reply_t *filtered = calloc(1, new_reply_size);
    if (!filtered) return reply;

    /* Copy header */
    memcpy(filtered, reply, sizeof(xcb_randr_get_monitors_reply_t));
    filtered->nMonitors = 1;
    filtered->nOutputs = target->nOutput;
    filtered->length = (monitor_data_size + 3) / 4;

    /* Copy the target monitor info + its outputs into the new reply.
     * Move the monitor to position (0,0) so TD thinks it's the only screen. */
    uint8_t *dst = (uint8_t *)filtered + sizeof(xcb_randr_get_monitors_reply_t);
    memcpy(dst, target, sizeof(xcb_randr_monitor_info_t));

    xcb_randr_monitor_info_t *new_mon = (xcb_randr_monitor_info_t *)dst;
    new_mon->x = 0;
    new_mon->y = 0;
    new_mon->primary = 1;

    /* Copy output IDs */
    xcb_randr_output_t *src_outputs = xcb_randr_monitor_info_outputs(target);
    memcpy(dst + sizeof(xcb_randr_monitor_info_t), src_outputs, outputs_size);

    free(reply);
    return filtered;
}

/* ---- xcb_copy_area intercept ---- */

xcb_void_cookie_t
xcb_copy_area(xcb_connection_t *c,
              xcb_drawable_t src_drawable,
              xcb_drawable_t dst_drawable,
              xcb_gcontext_t gc,
              int16_t src_x, int16_t src_y,
              int16_t dst_x, int16_t dst_y,
              uint16_t width, uint16_t height) {

    if (!real_xcb_copy_area_fn)
        real_xcb_copy_area_fn =
            (real_xcb_copy_area_t)dlsym(RTLD_NEXT, "xcb_copy_area");

    if (!is_root_window(c, src_drawable)) {
        return real_xcb_copy_area_fn(c, src_drawable, dst_drawable, gc,
                                      src_x, src_y, dst_x, dst_y,
                                      width, height);
    }

    load_config();

    /* Determine what region of the full screenshot to grab.
     * In single-screen mode, TD sees a virtual screen at (0,0),
     * but the real content is at (screen_x, screen_y) in the full screenshot. */
    int grab_x = src_x;
    int grab_y = src_y;

    if (target_screen >= 0 && screen_geo_known) {
        /* TD thinks the screen starts at (0,0), translate to real coords */
        grab_x = screen_x + src_x;
        grab_y = screen_y + src_y;
        logmsg("xcb_copy_area from ROOT (single-screen mode): "
                "virtual(%d,%d) -> real(%d,%d) %dx%d",
                src_x, src_y, grab_x, grab_y, width, height);
    } else {
        logmsg("xcb_copy_area from ROOT: src(%d,%d) dst(%d,%d) %dx%d",
                src_x, src_y, dst_x, dst_y, width, height);
    }

    /* Do the real copy first (will be black but needed for protocol) */
    xcb_void_cookie_t cookie =
        real_xcb_copy_area_fn(c, src_drawable, dst_drawable, gc,
                               src_x, src_y, dst_x, dst_y,
                               width, height);

    /* Take a real screenshot and overwrite the pixmap */
    char tmppath[256];
    snprintf(tmppath, sizeof(tmppath), "/tmp/td_grab_%d.png", getpid());

    if (take_screenshot(tmppath) != 0) {
        logmsg("Screenshot failed");
        return cookie;
    }

    int data_size = 0;
    uint8_t *pixels = load_png_pixels(tmppath, grab_x, grab_y,
                                       width, height, &data_size);
    unlink(tmppath);

    if (!pixels) {
        logmsg("Failed to load PNG pixels");
        return cookie;
    }

    /* Get screen depth */
    const xcb_setup_t *setup = xcb_get_setup(c);
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
    uint8_t depth = iter.data ? iter.data->root_depth : 24;

    /* Write to pixmap in chunks (xcb has max request size) */
    uint32_t max_req = xcb_get_maximum_request_length(c);
    uint32_t max_data = (max_req * 4) - 64;
    int row_bytes = width * 4;
    int rows_per_chunk = max_data / row_bytes;
    if (rows_per_chunk < 1) rows_per_chunk = 1;
    if (rows_per_chunk > height) rows_per_chunk = height;

    for (int y_off = 0; y_off < height; y_off += rows_per_chunk) {
        int chunk_rows = rows_per_chunk;
        if (y_off + chunk_rows > height)
            chunk_rows = height - y_off;

        xcb_put_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP,
                       dst_drawable, gc,
                       width, chunk_rows,
                       dst_x, dst_y + y_off,
                       0, depth,
                       chunk_rows * row_bytes,
                       &pixels[y_off * row_bytes]);
    }

    xcb_flush(c);
    free(pixels);

    logmsg("Wrote %dx%d screenshot to pixmap", width, height);
    return cookie;
}

__attribute__((constructor))
static void init(void) {
    load_config();
    logmsg("=== grab_override.so loaded ===");
}
