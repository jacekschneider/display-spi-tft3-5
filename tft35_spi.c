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

#include <uapi/drm/drm_fourcc.h>


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



static const struct drm_simple_display_pipe_funcs dsdp_funcs = {
    .enable = tft35_pipe_enable,
    .disable = tft35_pipe_disable,
    .update = tft35_pipe_update,
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
    .prime_fd_to_handle = drm_gem_prime_fd_to_handle,
    .prime_handle_to_fd = drm_gem_prime_handle_to_fd,
    .gem_prime_import = drm_gem_prime_import,
    .gem_prime_import_sg_table = drm_gem_shmem_prime_import_sg_table,
    .dumb_create = drm_gem_shmem_dumb_create,
    .dumb_map_offset = drm_gem_dumb_map_offset,
};

static const uint32_t formats[] = {
    DRM_FORMAT_RGB565,
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

int tft35_spi_write_pixels(struct tft35 *ctx, void *buf, int width, int height)
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

static int tft35_write_buffer(struct tft35 *ctx, void *buf, size_t data_length)
{
    int ret;
    gpiod_set_value_cansleep(ctx->dc_gpio, 0);
    ret = spi_write(ctx->spi, buf, data_length);
    if (ret < 0)
        dev_err(ctx->dev, "ERROR: Failed tft35_write_buffer %d\n", ret);
    return ret;
}

// init sequence
// CMD F1: 36 04 00 3C 0F 8F
// CMD F2: 18 A3 12 02 B2 12 FF 10 00
// CMD F8: 21 04
// CMD F9: 00 08
// CMD 36: 08
// CMD B4: 00
// CMD C1: 41
// CMD C5: 00 91 80 00
// CMD E0: 0F 1F 1C 0C 0F 08 48 98 37 0A 13 04 11 0D 00
// CMD E1: 0F 32 2E 0B 0D 05 47 75 37 06 10 03 24 20 00
// CMD 3A: 55
// CMD 11
// CMD 36: 28
// DELAY 255 ms
// CMD 29
int tft35_display_init(struct tft35 *ctx)
{
    int ret=0;
    uint8_t cmdf1[] = {0x36, 0x04, 0x00, 0x3C, 0x0F, 0x8F};
    uint8_t cmdf2[] = {0x18, 0xA3, 0x12, 0x02, 0xB2, 0x12, 0xFF, 0x10, 0x00};
    uint8_t cmdf8[] = {0x21, 0x04};
    uint8_t cmdf9[] = {0x00, 0x08};
    uint8_t cmd36[] = {0x08};    
    uint8_t cmdb4[] = {0x00};
    uint8_t cmdc1[] = {0x41};
    uint8_t cmdc5[] = {0x00, 0x91, 0x80, 0x00};
    uint8_t cmde0[] = {0x0F, 0x1F, 0x1C, 0x0C, 0x0F, 0x08, 0x48, 0x98, 0x37, 0x0A, 0x13, 0x04, 0x11, 0x0D, 0x00};
    uint8_t cmde1[] = {0x0F, 0x32, 0x2E, 0x0B, 0x0D, 0x05, 0x47, 0x75, 0x37, 0x06, 0x10, 0x03, 0x24, 0x20, 0x00};
    uint8_t cmd3a[] = {0x55};
    uint8_t cmd36_2[] = {0x28};

    tft35_write_cmd_data(ctx, 0xF1, cmdf1, ARRAY_SIZE(cmdf1));
    tft35_write_cmd_data(ctx, 0xF2, cmdf2, ARRAY_SIZE(cmdf2));
    tft35_write_cmd_data(ctx, 0xF8, cmdf8, ARRAY_SIZE(cmdf8));
    tft35_write_cmd_data(ctx, 0xF9, cmdf9, ARRAY_SIZE(cmdf9));
    tft35_write_cmd_data(ctx, 0x36, cmd36, ARRAY_SIZE(cmd36));
    tft35_write_cmd_data(ctx, 0xb4, cmdb4, ARRAY_SIZE(cmdb4));
    tft35_write_cmd_data(ctx, 0xc1, cmdc1, ARRAY_SIZE(cmdc1));
    tft35_write_cmd_data(ctx, 0xc5, cmdc5, ARRAY_SIZE(cmdc5));
    tft35_write_cmd_data(ctx, 0xe0, cmde0, ARRAY_SIZE(cmde0));
    tft35_write_cmd_data(ctx, 0xe1, cmde1, ARRAY_SIZE(cmde1));
    tft35_write_cmd_data(ctx, 0x3a, cmd3a, ARRAY_SIZE(cmd3a));
    tft35_write_cmd(ctx, 0x11);
    tft35_write_cmd_data(ctx, 0x36, cmd36_2, ARRAY_SIZE(cmd36_2));
    msleep(255);
    ret=tft35_write_cmd(ctx, 0x29);
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
    dev_info(ctx->dev, "tft35_pipe_update - 0");
    struct drm_plane_state *pstate = pipe->plane.state;
    struct drm_framebuffer *fb = pstate->fb;
    struct drm_rect src_rect = pstate->src;

    int w = src_rect.x2 - src_rect.x1;   // still in 16.16
    int h = src_rect.y2 - src_rect.y1;

    w >>= 16;   // convert from 16.16
    h >>= 16;
    dev_info(ctx->dev, "tft35_pipe_update - 1");
    struct drm_gem_object *obj;
    struct drm_gem_shmem_object *shmem;
    struct iosys_map src, dst;
    struct drm_rect rect;
    unsigned int dst_pitch;
    size_t len;
    int ret;

    if (!fb)
    {
        dev_info(ctx->dev, "ignored a display update\n");
        return;
    }
    dev_info(ctx->dev, "tft35_pipe_update - 2");
    obj = drm_gem_fb_get_obj(fb, 0);
    shmem = to_drm_gem_shmem_obj(obj);
    iosys_map_set_vaddr(&src, shmem->vaddr);
    iosys_map_set_vaddr(&dst, ctx->tx_buf);

    dst_pitch = w * 2;

    rect.x1 = 0;
    rect.y1 = 0;
    rect.x2 = w;
    rect.y2 = h;
    dev_info(ctx->dev, "tft35_pipe_update - 3");
    drm_fb_memcpy(&dst, &dst_pitch, &src, fb, &rect);

    len = w * h * 2;
    ret = tft35_write_buffer(ctx, ctx->tx_buf, len);
    if (ret)
        dev_err(ctx->dev, "ERROR: Failed tft35_pipe_update %d\n", ret);
}


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
    /* Create the main structure */
    struct tft35 *ctx =  spi_get_drvdata(spi);
    struct device *dev = &spi->dev; // Use ctx->dev directly.
    int err_code;
    
    ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
    if(!ctx)
        return -ENOMEM;

    ctx->dev = &spi->dev;
    ctx->spi = spi;
    struct tft35 *ctx2; 
    ctx2 = devm_drm_dev_alloc(dev, &driver_drm, struct tft35, dev_drm);
    if (IS_ERR(ctx2))
    {
        dev_dbg(dev, "ERROR: Failed devm_drm_dev_alloc\n");
        return -ENXIO;
    }

    ctx->pdev_drm = &ctx2->dev_drm;

    drm_mode_config_init(ctx->pdev_drm);
    ctx->pdev_drm->mode_config.min_width  = 0;
    ctx->pdev_drm->mode_config.min_height = 0;
    ctx->pdev_drm->mode_config.max_width  = 480; 
    ctx->pdev_drm->mode_config.max_height = 320;  

    err_code = drm_simple_display_pipe_init(ctx->pdev_drm, &ctx->dsdp, &dsdp_funcs, formats, ARRAY_SIZE(formats), NULL, NULL);
    if (err_code < 0)
    {
        dev_dbg(dev, "ERROR: Failed drm_simple_display_pipe_init %d\n", err_code);
        return err_code;
    }

    ctx->tx_buf_size = 480 * 320 * 2;
    ctx->tx_buf = devm_kmalloc(dev, ctx->tx_buf_size, GFP_KERNEL);
    if (!ctx->tx_buf)
        return -ENOMEM;

    ctx->dc_gpio = devm_gpiod_get(dev, "dc", GPIOD_OUT_HIGH);
    if(IS_ERR(ctx->dc_gpio))
    {
        dev_dbg(dev, "ERROR: dc_gpio devm_gpio_get\n");
        return -EIO;
    }

    ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
    if(IS_ERR(ctx->reset_gpio))
    {
        dev_dbg(dev, "ERROR: reset_gpio devm_gpio_get\n");
        return -EIO;
    }

    /* flags are passed to driver's load function,
    but the callback is deprecated and the value should be 0*/
    err_code = drm_dev_register(ctx->pdev_drm, 0);
    if (err_code < 0)
    {
        dev_dbg(dev, "ERROR: Failed drm_dev_register %d\n", err_code);
        return err_code;
    }


    spi_set_drvdata(spi, ctx);

    err_code=tft35_display_init(ctx);
    if (err_code < 0)
    {
        dev_dbg(dev, "ERROR: tft35 display init %d\n", err_code);
        return err_code;
    }
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
