#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/spi/spidev.h>
#include <linux/of.h>
#include <linux/delay.h>

#include <drm/drm_modes.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_probe_helper.h>

#include <uapi/drm/drm_fourcc.h>

#include <drm/drm_framebuffer.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fourcc.h>
#include <linux/iosys-map.h>
#include <linux/crc32.h>


#define __USE_MISC
DEFINE_DRM_GEM_FOPS(drm_fops);

struct tft35 {
    struct device *dev;
    struct drm_simple_display_pipe dsdp;
    struct drm_connector connector;
    struct drm_device dev_drm;
    struct drm_device* pdev_drm;
    struct spi_device *spi;
    struct gpio_desc *dc_gpio;
    struct gpio_desc *reset_gpio;

    void *tx_buf;          
    size_t tx_buf_size;    
};

static const struct drm_driver driver_drm = {
    .major = 1,
    .minor = 0,
    .patchlevel = 0,
    .name = "TFT35",
    .desc = "TFT35 DRM DRIVER",
    .date = "20263005",
    .driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
    .fops = &drm_fops,
    DRM_GEM_SHMEM_DRIVER_OPS,
    .prime_fd_to_handle = drm_gem_prime_fd_to_handle,
    .prime_handle_to_fd = drm_gem_prime_handle_to_fd,
    .gem_prime_import_sg_table = drm_gem_shmem_prime_import_sg_table,
    .dumb_map_offset = drm_gem_dumb_map_offset,
};

static const uint32_t formats[] = {
    DRM_FORMAT_XRGB8888,
    DRM_FORMAT_ARGB8888,
    DRM_FORMAT_RGB565 
};

int tft35_write_cmd(struct tft35 *ctx, uint8_t cmd)
{
    int ret;
    gpiod_set_value_cansleep(ctx->dc_gpio, 0);
    ret = spi_write(ctx->spi, &cmd, 1);
    if (ret < 0)
        dev_err(ctx->dev, "ERROR: Failed tft35_write_cmd %d\n", ret);
    return ret;
}

int tft35_write_data(struct tft35 *ctx, uint8_t *data, size_t data_lenght)
{
    gpiod_set_value_cansleep(ctx->dc_gpio, 1);
    return spi_write(ctx->spi, data, data_lenght);
}

int tft35_write_cmd_data(struct tft35 *ctx, uint8_t cmd, uint8_t *data, size_t data_length)
{
    int ret;
    ret = tft35_write_cmd(ctx, cmd);
    if (ret < 0)
        return ret;
    ret = tft35_write_data(ctx, data, data_length);
    if (ret < 0)
        dev_err(ctx->dev, "ERROR: Failed tft35_write_cmd_data %d\n", ret);
    return ret;
}

static inline void tft35_write_data16(struct tft35 *ctx, u16 value)
{
    uint8_t buf[2] = { value >> 8, value & 0xFF };
    tft35_write_data(ctx, buf, 2);
}

static void tft35_fill_color(struct tft35 *ctx, u16 color)
{
    int x, y;

    // Set column address
    tft35_write_cmd(ctx, 0x2A);
    tft35_write_data16(ctx, 0);
    tft35_write_data16(ctx, 479);

    // Set row address 
    tft35_write_cmd(ctx, 0x2B);
    tft35_write_data16(ctx, 0);
    tft35_write_data16(ctx, 319);

    // Memory write
    tft35_write_cmd(ctx, 0x2C);

    for (y = 0; y < 320; y++)
        for (x = 0; x < 480; x++)
            tft35_write_data16(ctx, color);
}

static int tft35_write_buffer(struct tft35 *ctx, const void *buf, size_t len)
{
    int ret = 0;
    size_t sent = 0;
    const uint8_t *p = buf;
    const size_t chunk = 16 * 1024;

    gpiod_set_value_cansleep(ctx->dc_gpio, 1);

    while (sent < len) {
        size_t this_len = min_t(size_t, chunk, len - sent);
        ret = spi_write(ctx->spi, p + sent, this_len);
        if (ret < 0) {
            dev_err(ctx->dev, "tft35_write_data: spi_write failed at %zu/%zu: %d\n",
                    sent, len, ret);
            return ret;
        }
        sent += this_len;
    }
    return 0;
}

static int tft35_spi_write_pixels(struct tft35 *ctx, void *buf, int width, int height)
{
    int ret;
    /* Set window (column + row address) */
    tft35_write_cmd(ctx, 0x2A);
    tft35_write_data16(ctx, 0);
    tft35_write_data16(ctx, width - 1);

    tft35_write_cmd(ctx, 0x2B);
    tft35_write_data16(ctx, 0);
    tft35_write_data16(ctx, height - 1);

    // Memory write
    tft35_write_cmd(ctx, 0x2C);

    /* Send pixel buffer */
    size_t len = width * height * 2;
    ret = tft35_write_buffer(ctx, buf, len);
    return ret;
}

int tft35_display_init(struct tft35 *ctx)
{
    int ret = 0;

    uint8_t cmdC0[] = {0x0F, 0x0F};
    uint8_t cmdC1[] = {0x41};
    uint8_t cmdC5[] = {0x00, 0x91, 0x80, 0x00};
    uint8_t cmd36[] = {0x48};
    uint8_t cmd3A[] = {0x55};
    uint8_t cmdB1[] = {0xB0};
    uint8_t cmdB4[] = {0x02};
    uint8_t cmdB6[] = {0x02, 0x02};
    uint8_t cmdE0[] = {
        0x0F, 0x1F, 0x1C, 0x0C, 0x0F, 0x08, 0x48, 0x98,
        0x37, 0x0A, 0x13, 0x04, 0x11, 0x0D, 0x00
    };
    uint8_t cmdE1[] = {
        0x0F, 0x32, 0x2E, 0x0B, 0x0D, 0x05, 0x47, 0x75,
        0x37, 0x06, 0x10, 0x03, 0x24, 0x20, 0x00
    };

    tft35_write_cmd_data(ctx, 0xC0, cmdC0, ARRAY_SIZE(cmdC0));
    tft35_write_cmd_data(ctx, 0xC1, cmdC1, ARRAY_SIZE(cmdC1));
    tft35_write_cmd_data(ctx, 0xC5, cmdC5, ARRAY_SIZE(cmdC5));

    tft35_write_cmd_data(ctx, 0x36, cmd36, ARRAY_SIZE(cmd36));
    tft35_write_cmd_data(ctx, 0x3A, cmd3A, ARRAY_SIZE(cmd3A));

    tft35_write_cmd_data(ctx, 0xB1, cmdB1, ARRAY_SIZE(cmdB1));
    tft35_write_cmd_data(ctx, 0xB4, cmdB4, ARRAY_SIZE(cmdB4));
    tft35_write_cmd_data(ctx, 0xB6, cmdB6, ARRAY_SIZE(cmdB6));

    tft35_write_cmd_data(ctx, 0xE0, cmdE0, ARRAY_SIZE(cmdE0));
    tft35_write_cmd_data(ctx, 0xE1, cmdE1, ARRAY_SIZE(cmdE1));

    tft35_write_cmd(ctx, 0x11);
    msleep(120);

    ret = tft35_write_cmd(ctx, 0x29);
    msleep(20);

    return ret;
}


void tft35_pipe_enable(struct drm_simple_display_pipe *pipe,
		       struct drm_crtc_state *crtc_state,
		       struct drm_plane_state *plane_state)
{
    ;
}

void tft35_pipe_disable(struct drm_simple_display_pipe *pipe)
{
    ;
}

void tft35_pipe_update(struct drm_simple_display_pipe *pipe,
                       struct drm_plane_state *old_plane_state)
{
    struct tft35 *ctx = container_of(pipe, struct tft35, dsdp);
    struct drm_plane_state *pstate = pipe->plane.state;
    struct drm_framebuffer *fb;
    struct drm_rect src_rect;
    int w, h;
    struct drm_gem_object *obj;
    struct drm_gem_shmem_object *shmem;
    struct iosys_map src_map = IOSYS_MAP_INIT_VADDR(NULL);
    struct iosys_map dst_map = IOSYS_MAP_INIT_VADDR(NULL);
    unsigned int dst_pitch;
    struct drm_rect rect;
    int bpp_bytes = 0;
    int ret;
    size_t required_len;

    if (!pstate) {
        dev_dbg(ctx->dev, "tft35_pipe_update: no plane state\n");
        return;
    }

    fb = pstate->fb;
    if (!fb) {
        dev_dbg(ctx->dev, "tft35_pipe_update: no framebuffer attached\n");
        return;
    }

    src_rect = pstate->src;
    w = (src_rect.x2 - src_rect.x1) >> 16;
    h = (src_rect.y2 - src_rect.y1) >> 16;

    if (w <= 0 || h <= 0) {
        dev_dbg(ctx->dev, "tft35_pipe_update: empty rect w=%d h=%d\n", w, h);
        return;
    }

    switch (fb->format->format) {
    case DRM_FORMAT_XRGB8888:
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XBGR8888:
    case DRM_FORMAT_ABGR8888:
        bpp_bytes = 4;
        break;
    case DRM_FORMAT_RGB565:
        bpp_bytes = 2;
        break;
    default:
        dev_err(ctx->dev, "tft35_pipe_update: unsupported fb format 0x%08x\n",
                fb->format->format);
        return;
    }

    dst_pitch = w * 2;
    required_len = (size_t)w * (size_t)h * 2;

    rect.x1 = 0;
    rect.y1 = 0;
    rect.x2 = w;
    rect.y2 = h;

    dev_dbg(ctx->dev,
            "tft35_pipe_update: start fb=%u fmt=0x%08x w=%d h=%d bpp=%d dst_pitch=%u\n",
            fb->base.id, fb->format->format, w, h, bpp_bytes, dst_pitch);

    if (!ctx->tx_buf) {
        dev_err(ctx->dev, "tft35_pipe_update: no tx_buf allocated\n");
        return;
    }

    obj = drm_gem_fb_get_obj(fb, 0);
    if (!obj) {
        dev_err(ctx->dev, "tft35_pipe_update: failed to get gem object for fb\n");
        return;
    }

    shmem = to_drm_gem_shmem_obj(obj);
    if (!shmem) {
        dev_err(ctx->dev, "tft35_pipe_update: gem object is not shmem-backed\n");
        return;
    }

    ret = drm_gem_shmem_vmap(shmem, &src_map);
    if (ret) {
        dev_err(ctx->dev, "tft35_pipe_update: drm_gem_shmem_vmap failed: %d\n", ret);
        return;
    }

    if (fb->offsets[0]) {
        iosys_map_set_vaddr(&src_map, src_map.vaddr + fb->offsets[0]);
    }

    iosys_map_set_vaddr(&dst_map, ctx->tx_buf);

    if (!src_map.vaddr) {
        dev_err(ctx->dev, "tft35_pipe_update: invalid src vaddr\n");
        goto out_unmap;
    }
    if (!dst_map.vaddr) {
        dev_err(ctx->dev, "tft35_pipe_update: invalid dst vaddr\n");
        goto out_unmap;
    }
    if (dst_pitch < (unsigned int)(w * 2)) {
        dev_err(ctx->dev, "tft35_pipe_update: dst_pitch too small %u < %u\n",
                dst_pitch, (unsigned int)(w * 2));
        goto out_unmap;
    }

    dev_dbg(ctx->dev,
            "pipe_update: pitches[0]=%u offsets[0]=%u src_map=%p dst_map=%p required_len=%zu\n",
            fb->pitches[0], fb->offsets[0], src_map.vaddr, dst_map.vaddr, required_len);

    drm_fb_memcpy(&dst_map, &dst_pitch, &src_map, fb, &rect);

    {
        size_t crc_len = min_t(size_t, 64, required_len);
        u32 crc = crc32_le(0, ctx->tx_buf, crc_len);
        dev_dbg(ctx->dev, "pipe_update: tx_buf crc32=%08x (len=%zu)\n", crc, crc_len);
    }

out_unmap:
    drm_gem_shmem_vunmap(shmem, &src_map);

    ret = tft35_spi_write_pixels(ctx, ctx->tx_buf, w, h);
    if (ret)
        dev_err(ctx->dev, "tft35_pipe_update: tft35_spi_write_pixels failed: %d\n", ret);
    else
        dev_dbg(ctx->dev, "tft35_pipe_update: tft35_spi_write_pixels OK\n");
}

static const struct drm_simple_display_pipe_funcs dsdp_funcs = {
    .enable = tft35_pipe_enable,
    .disable = tft35_pipe_disable,
    .update = tft35_pipe_update,
};

static const struct drm_connector_funcs tft35_connector_funcs = {
    .reset = drm_atomic_helper_connector_reset,
    .fill_modes = drm_helper_probe_single_connector_modes,
    .destroy = drm_connector_cleanup,
    .atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
    .atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
};

static const struct drm_display_mode tft35_default_mode = {
    .clock = 9000,
    .hdisplay = 480,
    .hsync_start = 480,
    .hsync_end = 480,
    .htotal = 480,
    .vdisplay = 320,
    .vsync_start = 320,
    .vsync_end = 320,
    .vtotal = 320,
    .vrefresh = 60,
    .flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static int tft35_get_modes(struct drm_connector *connector)
{
    struct drm_display_mode *mode;
    struct drm_device *drm = connector->dev;
    if (!drm) {
        pr_err("tft35: get_modes: connector->dev is NULL\n");
        return -EINVAL;
    }
    mode = drm_mode_duplicate(drm, &tft35_default_mode);
    if (!mode)
        return -ENOMEM;
    drm_mode_set_name(mode);
    mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
    drm_mode_probed_add(connector, mode);
    pr_info("tft35: get_modes added mode %s\n", mode->name ? mode->name : "<noname>");

    return 1;
}

static const struct drm_connector_helper_funcs tft35_connector_helper_funcs = {
    .get_modes = tft35_get_modes,   
};

static const struct drm_mode_config_funcs drm_simple_mode_config_funcs = {
    .fb_create = drm_gem_fb_create,
    .atomic_check = drm_atomic_helper_check,
    .atomic_commit = drm_atomic_helper_commit,
};

static const struct of_device_id tft35_of_match[] = {
    {.compatible = "schneider,tft35display"},
    {/* sentinel */}
};
MODULE_DEVICE_TABLE(of, tft35_of_match);

static const struct spi_device_id tft35_id_table[] = {
    {"schneider,tft35display", 0},
    {/* sentinel */}
};
MODULE_DEVICE_TABLE(spi, tft35_id_table);

static int tft35_probe(struct spi_device *spi)
{
    struct drm_device *drm;
    struct tft35 *ctx;
    struct device *dev = &spi->dev;
    int err;

    ctx = devm_drm_dev_alloc(dev, &driver_drm, struct tft35, dev_drm);
    if (IS_ERR(ctx))
        return PTR_ERR(ctx);

    spi_set_drvdata(spi, ctx);
    drm = &ctx->dev_drm;
    ctx->dev = dev;
    ctx->spi = spi;
    ctx->pdev_drm = drm;

    drm_mode_config_init(ctx->pdev_drm);
    ctx->pdev_drm->mode_config.funcs = &drm_simple_mode_config_funcs;
    ctx->pdev_drm->mode_config.min_width  = 1;
    ctx->pdev_drm->mode_config.min_height = 1;
    ctx->pdev_drm->mode_config.max_width  = 480;
    ctx->pdev_drm->mode_config.max_height = 320;

    err = drm_connector_init(ctx->pdev_drm, &ctx->connector,
                             &tft35_connector_funcs,
                             DRM_MODE_CONNECTOR_SPI);
    if (err)
        return err;

    drm_connector_helper_add(&ctx->connector, &tft35_connector_helper_funcs);

    err = drm_simple_display_pipe_init(ctx->pdev_drm, &ctx->dsdp,
                                       &dsdp_funcs,
                                       formats, ARRAY_SIZE(formats),
                                       NULL,
                                       &ctx->connector);
    if (err < 0)
        return err;

    drm_mode_config_reset(ctx->pdev_drm);
    drm_kms_helper_poll_init(ctx->pdev_drm);

    ctx->tx_buf_size = 480 * 320 * 2;
    ctx->tx_buf = devm_kmalloc(dev, ctx->tx_buf_size, GFP_KERNEL);
    if (!ctx->tx_buf)
        return -ENOMEM;

    ctx->dc_gpio = devm_gpiod_get(dev, "dc", GPIOD_OUT_LOW);
    if (IS_ERR(ctx->dc_gpio))
        return PTR_ERR(ctx->dc_gpio);

    ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
    if (IS_ERR(ctx->reset_gpio))
        return PTR_ERR(ctx->reset_gpio);

    err = drm_dev_register(ctx->pdev_drm, 0);
    if (err < 0)
        return err;

    drm_connector_attach_encoder(&ctx->connector, &ctx->dsdp.encoder);

    pr_info("tft35: dsdp.encoder=%p connector=%p connector->encoder=%p\n",
            &ctx->dsdp.encoder, &ctx->connector, ctx->connector.encoder);

    err = tft35_display_init(ctx);
    if (err < 0)
        return err;

    tft35_fill_color(ctx, 0x07E0);

    return 0;
}

static void tft35_remove(struct spi_device *spi)
{
    struct tft35 *ctx = spi_get_drvdata(spi);
    drm_dev_unregister(ctx->pdev_drm);
}

static struct spi_driver tft35_spi_driver = {
    .probe = tft35_probe,
    .remove = tft35_remove,
    .driver =
    {
        .owner = THIS_MODULE,
        .name = "tft35display",
        .of_match_table = of_match_ptr(tft35_of_match),
    },
    .id_table = tft35_id_table,
};
module_spi_driver(tft35_spi_driver);

MODULE_DESCRIPTION("Tiny DRM driver for tft35 display");
MODULE_AUTHOR("Jacek Schneider <schneiderautomatyka@gmail.com>");
MODULE_LICENSE("GPL");
