/**
 * \file imageio.c
 * \brief Implements read_image and write_image functions
 * \author Pascal Getreuer <getreuer@cmla.ens-cachan.fr>
 *
 * Two high-level functions are provided, #read_image() and #write_image(),
 * for reading and writing image BMP, JPEG, PNG, and TIFF files. The desired
 * format of the image data can be specified to \c read_image for how to
 * return the data (and similarly to \c write_image for how it should
 * interpret the data). Formatting options allow specifying the datatype of
 * the components, conversion to grayscale, channel ordering, interleaved vs.
 * planar, and row-major vs. column-major.
 *
 * \c read_image automatically detects the format of the image being read so
 * that the format does not need to be supplied explicitly. \c write_image
 * infers the file format from the file extension.
 *
 * Also included is a function #identify_image_type() to guess the file type
 * (BMP, JPEG, PNG, TIFF, and a few other formats) from the file header's
 * magic numbers without reading the image.
 *
 * Support for BMP reading and writing is native: BMP reading supports 1-, 2-,
 * 4-, 8-, 16-, 32-bit uncompressed, RLE, and bitfield images; BMP writing is
 * limited to 8- and 24-bit uncompressed. The implementation calls libjpeg,
 * libpng, and libtiff to handle JPEG, PNG, and TIFF images.
 *
 *
 * Copyright (c) 2010-2013, Pascal Getreuer
 * All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under, at your option, the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version, or the terms of the
 * simplified BSD license.
 *
 * You should have received a copy of these licenses along this program.
 * If not, see <http://www.gnu.org/licenses/> and
 * <http://www.opensource.org/licenses/bsd-license.html>.
 */

#include "imageio.h"
#include <string.h>
#include <ctype.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION

#include "stb_image.h"
#include "stb_image_write.h"
/** \brief buffer size to use for BMP file I/O */
#define FILE_BUFFER_CAPACITY    (1024*4)

#define ROUNDCLAMPF(x)   ((x < 0.0f) ? 0 : \
    ((x > 1.0f) ? 255 : (uint8_t)(255.0f*(x) + 0.5f)))
#define ROUNDCLAMP(x)   ((x < 0.0f) ? 0 : \
    ((x > 1.0f) ? 255 : (uint8_t)(255.0f*(x) + 0.5f)))


/** \brief Case-insensitive test to see if string ends with suffix */
static int string_ends_with(const char *string, const char *suffix) {
    unsigned string_length = strlen(string), suffix_length = strlen(suffix);
    unsigned i;

    if (string_length < suffix_length)
        return 0;

    string += string_length - suffix_length;

    for (i = 0; i < suffix_length; ++i)
        if (tolower(string[i]) != tolower(suffix[i]))
            return 0;

    return 1;
}


/** \brief Fill an image with a color */
static void fill_image(uint32_t *image, int width, int height, uint32_t color) {
    int x, y;

    if (image)
        for (y = 0; y < height; ++y, image += width)
            for (x = 0; x < width; ++x)
                image[x] = color;
}


/**
 * \brief Check use of color and alpha, and count number of distinct colors
 * \param num_colors set by the routine to the number of unique colors
 * \param use_color set to 1 if the image is not grayscale
 * \param use_alpha set to 1 if the image alpha is not constant 255
 * \param image pointer to U8 RGBA interleaved image data
 * \param width, height dimensions of the image
 * \return pointer to a color palette with num_colors entries or NULL if the
 * number of distinct colors exceeds 256.
 *
 * This routine checks whether an RGBA image makes use of color and alpha, and
 * constructs a palette if the number of distinct colors is 256 or fewer. This
 * information is useful for writing image files with smaller file size.
 */
static uint32_t *get_image_palette(int *num_colors, int *use_color,
                                   int *use_alpha, const uint32_t *image, int width, int height) {
    const int max_colors = 256;
    uint32_t *palette = NULL;
    uint32_t pixel;
    int x, y, i, red, green, blue, alpha;


    if (!use_color || !num_colors || !use_alpha)
        return NULL;
    else if (!image
             || !(palette = (uint32_t *) malloc(sizeof(uint32_t) * max_colors))) {
        *num_colors = -1;
        *use_color = *use_alpha = 1;
        return NULL;
    }

    *num_colors = *use_color = *use_alpha = 0;

    for (y = 0; y < height; ++y) {
        for (x = 0; x < width; ++x) {
            pixel = *(image++);
            red = ((uint8_t *) &pixel)[0];
            green = ((uint8_t *) &pixel)[1];
            blue = ((uint8_t *) &pixel)[2];
            alpha = ((uint8_t *) &pixel)[3];

            if (red != green || red != blue)     /* Check color */
                *use_color = 1;

            if (alpha != 255)                    /* Check alpha */
                *use_alpha = 1;

            /* Check palette colors (if *num_colors != -1) */
            for (i = 0; i < *num_colors; ++i)
                if (pixel == palette[i])
                    break;

            if (i == *num_colors) {
                if (i < max_colors) {   /* Add new color to palette */
                    palette[i] = pixel;
                    (*num_colors)++;
                } else {   /* Maximum size for palette exceeded */
                    free(palette);
                    palette = NULL;
                    *num_colors = -1;    /* Don't check palette colors */
                }
            }
        }
    }

    return palette;
}


/** \brief Read a 16-bit little Endian word from file */
static uint16_t read_u16_le(FILE *file) {
    uint16_t w;
    w = (uint16_t) getc(file);
    w |= ((uint16_t) getc(file) << 8);
    return w;
}


/** \brief Read a 32-bit little Endian float word from file */
static uint32_t read_u32_le(FILE *file) {
    uint32_t dw;
    dw = (uint32_t) getc(file);
    dw |= ((uint32_t) getc(file) << 8);
    dw |= ((uint32_t) getc(file) << 16);
    dw |= ((uint32_t) getc(file) << 24);
    return dw;
}


/** \brief Write a 16-bit word in little Endian format */
static void write_u16_le(uint16_t w, FILE *file) {
    putc(w & 0xFF, file);
    putc((w & 0xFF00) >> 8, file);
}


/** \brief Write a 32-bit float word in little Endian format */
static void write_u32_le(uint32_t dw, FILE *file) {
    putc(dw & 0xFF, file);
    putc((dw & 0xFF00) >> 8, file);
    putc((dw & 0xFF0000) >> 16, file);
    putc((dw & 0xFF000000) >> 24, file);
}


/** \brief Internal function for reading 1-bpp BMP */
static int read_bmp_1bpp(uint32_t *image, int width, int height,
                         FILE *file, const uint32_t *palette) {
    int row_padding = (-(width + 7) / 8) & 3;
    int x, y, bit;
    unsigned code;

    image += ((long int) width) * ((long int) height - 1);

    for (y = height; y; --y, image -= width) {
        if (feof(file))
            return 0;

        for (x = 0; x < width;) {
            code = getc(file);

            for (bit = 7; bit >= 0 && x < width; --bit, code <<= 1)
                image[x++] = palette[(code & 0x80) ? 1 : 0];
        }

        for (x = row_padding; x; --x)
            getc(file); /* Skip padding bytes at the end of the row */
    }

    return 1;
}


/** \brief Internal function for reading 4-bpp BMP */
static int read_bmp_4bpp(uint32_t *image, int width, int height,
                         FILE *file, const uint32_t *palette) {
    int row_padding = (-(width + 1) / 2) & 3;
    int x, y;
    unsigned code;

    image += ((long int) width) * ((long int) height - 1);

    for (y = height; y; --y, image -= width) {
        if (feof(file))
            return 0;

        for (x = 0; x < width;) {
            code = getc(file);
            image[x++] = palette[(code & 0xF0) >> 4];

            if (x < width)
                image[x++] = palette[code & 0x0F];
        }

        for (x = row_padding; x; --x)
            getc(file); /* Skip padding bytes at the end of the row */
    }

    return 1;
}


/** \brief Internal function for reading 4-bpp RLE-compressed BMP */
static int read_bmp_4bpp_rle(uint32_t *image, int width, int height,
                             FILE *file, const uint32_t *palette) {
    int x, y, dy, k;
    unsigned count, value;
    uint32_t color_high, color_low;

    fill_image(image, width, height, palette[0]);
    image += ((long int) width) * ((long int) height - 1);

    for (x = 0, y = height; y;) {
        if (feof(file))
            return 0;

        count = getc(file);
        value = getc(file);

        if (!count) {   /* count = 0 is the escape code */
            switch (value) {
                case 0:     /* End of line */
                    image -= width;
                    x = 0;
                    y--;
                    break;
                case 1:     /* End of bitmap */
                    return 1;
                case 2:     /* Delta */
                    x += getc(file);
                    dy = getc(file);
                    y -= dy;
                    image -= dy * width;

                    if (x >= width || y < 0)
                        return 0;
                    break;
                default:
                    /* Read a run of uncompressed data (value = length of run) */
                    count = k = value;

                    if (x >= width)
                        return 0;

                    do {
                        value = getc(file);
                        image[x++] = palette[(value & 0xF0) >> 4];

                        if (x >= width)
                            break;

                        if (--k) {
                            image[x++] = palette[value & 0x0F];
                            k--;

                            if (x >= width)
                                break;
                        }
                    } while (k);

                    if (((count + 1) / 2) & 1)
                        getc(file); /* Padding for word align */
            }
        } else {   /* Run of pixels (count = length of run) */
            color_high = palette[(value & 0xF0) >> 4];
            color_low = palette[value & 0xF];

            if (x >= width)
                return 0;

            do {
                image[x++] = color_high;
                count--;

                if (x >= width)
                    break;

                if (count) {
                    image[x++] = color_low;
                    count--;

                    if (x >= width)
                        break;
                }
            } while (count);
        }
    }

    return 1;
}


/** \brief Internal function for reading 8-bpp BMP */
static int read_bmp_8bpp(uint32_t *image, int width, int height,
                         FILE *file, const uint32_t *palette) {
    int row_padding = (-width) & 3;
    int x, y;

    image += ((long int) width) * ((long int) height - 1);

    for (y = height; y; --y, image -= width) {
        if (feof(file))
            return 0;

        for (x = 0; x < width; ++x)
            image[x] = palette[getc(file) & 0xFF];

        for (x = row_padding; x; --x)
            getc(file); /* Skip padding bytes at the end of the row */
    }

    return 1;
}


/** \brief Internal function for reading 8-bpp RLE-compressed BMP */
static int read_bmp_8bpp_rle(uint32_t *image, int width, int height,
                             FILE *file, const uint32_t *palette) {
    int x, y, dy, k;
    unsigned count, value;
    uint32_t color;

    fill_image(image, width, height, palette[0]);
    image += ((long int) width) * ((long int) height - 1);

    for (x = 0, y = height; y;) {
        if (feof(file))
            return 0;

        count = getc(file);
        value = getc(file);

        if (!count) {   /* count = 0 is the escape code */
            switch (value) {
                case 0:     /* End of line */
                    image -= width;
                    x = 0;
                    y--;
                    break;
                case 1:     /* End of bitmap */
                    return 1;
                case 2:     /* Delta */
                    x += getc(file);
                    dy = getc(file);
                    y -= dy;
                    image -= dy * width;

                    if (x >= width || y < 0)
                        return 0;
                    break;
                default:
                    /* Read a run of uncompressed data (value = length of run) */
                    count = k = value;

                    do {
                        if (x >= width)
                            break;

                        image[x++] = palette[getc(file) & 0xFF];
                    } while (--k);

                    if (count & 1)
                        getc(file); /* Padding for word align */
            }
        } else {   /* Run of pixels equal to value (count = length of run) */
            color = palette[value & 0xFF];

            do {
                if (x >= width)
                    break;

                image[x++] = color;
            } while (--count);
        }
    }

    return 1;
}


/** \brief Internal function for reading 24-bpp BMP */
static int read_bmp_24bpp(uint32_t *image, int width, int height, FILE *file) {
    uint8_t *image_ptr = (uint8_t *) image;
    int row_padding = (-3 * width) & 3;
    int x, y;

    width <<= 2;
    image_ptr += ((long int) width) * ((long int) height - 1);

    for (y = height; y; --y, image_ptr -= width) {
        if (feof(file))
            return 0;

        for (x = 0; x < width; x += 4) {
            image_ptr[x + 3] = 255;        /* Set alpha            */
            image_ptr[x + 2] = getc(file); /* Read blue component  */
            image_ptr[x + 1] = getc(file); /* Read green component */
            image_ptr[x + 0] = getc(file); /* Read red component   */
        }

        for (x = row_padding; x; --x)
            getc(file); /* Skip padding bytes at the end of the row */
    }

    return 1;
}

/** \brief Internal function for determining bit shifts in bitfield BMP */
static void get_mask_shifts(uint32_t mask, int *left_shift, int *right_shift) {
    int shift = 0, bitcount = 0;

    if (!mask) {
        *left_shift = 0;
        *right_shift = 0;
        return;
    }

    while (!(mask & 1))  /* Find the first true bit */
    {
        mask >>= 1;
        ++shift;
    }

    /* Adjust the result for scaling to 8-bit quantities */
    while (mask & 1)     /* count the number of true bits */
    {
        mask >>= 1;
        ++bitcount;
    }

    /* Compute a signed shift (right is positive) */
    shift += bitcount - 8;

    if (shift >= 0) {
        *left_shift = 0;
        *right_shift = shift;
    } else {
        *left_shift = -shift;
        *right_shift = 0;
    }
}

/** \brief Internal function for reading 16-bpp BMP */
static int read_bmp_16bpp(uint32_t *image, int width, int height, FILE *file,
                          uint32_t redmask, uint32_t greenmask,
                          uint32_t bluemask, uint32_t alphamask) {
    uint8_t *image_ptr = (uint8_t *) image;
    uint32_t code;
    int row_padding = (-2 * width) & 3;
    int redleft_shift, greenleft_shift, blueleft_shift, alphaleft_shift;
    int redright_shift, greenright_shift, blueright_shift, alpharight_shift;
    int x, y;

    get_mask_shifts(redmask, &redleft_shift, &redright_shift);
    get_mask_shifts(greenmask, &greenleft_shift, &greenright_shift);
    get_mask_shifts(bluemask, &blueleft_shift, &blueright_shift);
    get_mask_shifts(alphamask, &alphaleft_shift, &alpharight_shift);
    width <<= 2;
    image_ptr += ((long int) width) * ((long int) height - 1);

    for (y = height; y; --y, image_ptr -= width) {
        if (feof(file))
            return 0;

        for (x = 0; x < width; x += 4) {
            code = read_u16_le(file);
            /* By the Windows 4.x BMP specification, masks must be contiguous
               <http://www.fileformat.info/format/bmp/egff.htm>.  So we can
               decode bitfields by bitshifting and bitwise AND. */
            image_ptr[x + 3] = ((code & alphamask) >> alpharight_shift)
                    << alphaleft_shift;
            image_ptr[x + 2] = ((code & bluemask) >> blueright_shift)
                    << blueleft_shift;
            image_ptr[x + 1] = ((code & greenmask) >> greenright_shift)
                    << greenleft_shift;
            image_ptr[x + 0] = ((code & redmask) >> redright_shift)
                    << redleft_shift;
        }

        for (x = row_padding; x; --x)
            getc(file); /* Skip padding bytes at the end of the row */
    }

    return 1;
}


/** \brief Internal function for reading 32-bpp BMP */
static int read_bmp_32bpp(uint32_t *image, int width, int height, FILE *file,
                          uint32_t redmask, uint32_t greenmask,
                          uint32_t bluemask, uint32_t alphamask) {
    uint8_t *image_ptr;
    uint32_t code;
    int redleft_shift, greenleft_shift, blueleft_shift, alphaleft_shift;
    int redright_shift, greenright_shift, blueright_shift, alpharight_shift;
    int x, y;

    get_mask_shifts(redmask, &redleft_shift, &redright_shift);
    get_mask_shifts(greenmask, &greenleft_shift, &greenright_shift);
    get_mask_shifts(bluemask, &blueleft_shift, &blueright_shift);
    get_mask_shifts(alphamask, &alphaleft_shift, &alpharight_shift);
    width <<= 2;
    image_ptr = (uint8_t *) image + ((long int) width) * ((long int) height - 1);

    for (y = height; y; --y, image_ptr -= width) {
        if (feof(file))
            return 0;

        for (x = 0; x < width; x += 4) {
            code = read_u32_le(file);
            image_ptr[x + 3] = ((code & alphamask) >> alpharight_shift)
                    << alphaleft_shift;
            image_ptr[x + 2] = ((code & bluemask) >> blueright_shift)
                    << blueleft_shift;
            image_ptr[x + 1] = ((code & greenmask) >> greenright_shift)
                    << greenleft_shift;
            image_ptr[x + 0] = ((code & redmask) >> redright_shift)
                    << redleft_shift;
        }
    }

    return 1;
}

/**
* \brief Read a BMP (Windows Bitmap) image file as RGBA data
*
* \param image, width, height pointers to be filled with the pointer
*        to the image data and the image dimensions.
* \param file stdio FILE pointer pointing to the beginning of the BMP file
*
* \return 1 on success, 0 on failure
*
* This function is called by \c read_image to read BMP images. Before calling
* \c read_bmp, the caller should open \c file as a FILE pointer in binary read
* mode. When \c read_bmp is complete, the caller should close \c file.
*/
static int read_bmp(uint32_t **image, int *width, int *height, FILE *file) {
    uint32_t *palette = NULL;
    uint8_t *palette_ptr;
    long int image_data_offset, info_size;
    unsigned i, num_planes, bits_per_pixel, compression, num_colors;
    uint32_t redmask, greenmask, bluemask, alphamask;
    int success = 0, os2bmp;
    uint8_t magic[2];

    *image = NULL;
    *width = *height = 0;
    fseek(file, 0, SEEK_SET);

    magic[0] = getc(file);
    magic[1] = getc(file);

    if (!(magic[0] == 0x42 && magic[1] == 0x4D) /* Verify the magic numbers */
        || fseek(file, 8, SEEK_CUR))         /* Skip the reserved fields */
    {
        fprintf(stderr, "Invalid BMP header.\n");
        goto fail;
    }

    image_data_offset = read_u32_le(file);
    info_size = read_u32_le(file);

    /* Read the info header */
    if (info_size < 12) {
        fprintf(stderr, "Invalid BMP info header.\n");
        goto fail;
    }

    if ((os2bmp = (info_size == 12)))  /* This is an OS/2 V1 infoheader */
    {
        *width = (int) read_u16_le(file);
        *height = (int) read_u16_le(file);
        num_planes = (unsigned) read_u16_le(file);
        bits_per_pixel = (unsigned) read_u16_le(file);
        compression = 0;
        num_colors = 0;
        redmask = 0x00FF0000;
        greenmask = 0x0000FF00;
        bluemask = 0x000000FF;
        alphamask = 0xFF000000;
    } else {
        *width = abs((int) read_u32_le(file));
        *height = abs((int) read_u32_le(file));
        num_planes = (unsigned) read_u16_le(file);
        bits_per_pixel = (unsigned) read_u16_le(file);
        compression = (unsigned) read_u32_le(file);
        fseek(file, 12, SEEK_CUR);
        num_colors = (unsigned) read_u32_le(file);
        fseek(file, 4, SEEK_CUR);
        redmask = read_u32_le(file);
        greenmask = read_u32_le(file);
        bluemask = read_u32_le(file);
        alphamask = read_u32_le(file);
    }

    /* Check for problems or unsupported compression modes */
    if (*width > MAX_IMAGE_SIZE || *height > MAX_IMAGE_SIZE) {
        fprintf(stderr, "image dimensions exceed MAX_IMAGE_SIZE.\n");
        goto fail;
    }

    if (feof(file) || num_planes != 1 || compression > 3)
        goto fail;

    /* Allocate the image data */
    if (!(*image = (uint32_t *) malloc(
            sizeof(uint32_t) * ((long int) *width) * ((long int) *height))))
        goto fail;

    /* Read palette */
    if (bits_per_pixel <= 8) {
        fseek(file, 14 + info_size, SEEK_SET);

        if (!num_colors)
            num_colors = 1 << bits_per_pixel;

        if (!(palette = (uint32_t *) malloc(sizeof(uint32_t) * 256)))
            goto fail;

        for (i = 0, palette_ptr = (uint8_t *) palette; i < num_colors; ++i) {
            palette_ptr[3] = 255;          /* Set alpha            */
            palette_ptr[2] = getc(file);   /* Read blue component  */
            palette_ptr[1] = getc(file);   /* Read green component */
            palette_ptr[0] = getc(file);   /* Read red component   */
            palette_ptr += 4;

            if (!os2bmp)
                getc(file); /* Skip extra byte (for non-OS/2 bitmaps) */
        }

        /* Fill the rest of the palette with the first color */
        for (; i < 256; ++i)
            palette[i] = palette[0];
    }

    if (fseek(file, image_data_offset, SEEK_SET) || feof(file)) {
        fprintf(stderr, "file error.\n");
        goto fail;
    }

    /*** Read the bitmap image data ***/
    switch (compression) {
        case 0: /* Uncompressed data */
            switch (bits_per_pixel) {
                case 1: /* Read 1-bit uncompressed indexed data */
                    success = read_bmp_1bpp(
                            *image, *width, *height, file, palette);
                    break;
                case 4: /* Read 4-bit uncompressed indexed data */
                    success = read_bmp_4bpp(
                            *image, *width, *height, file, palette);
                    break;
                case 8: /* Read 8-bit uncompressed indexed data */
                    success = read_bmp_8bpp(
                            *image, *width, *height, file, palette);
                    break;
                case 24: /* Read 24-bit BGR image data */
                    success = read_bmp_24bpp(*image, *width, *height, file);
                    break;
                case 16: /* Read 16-bit data */
                    success = read_bmp_16bpp(*image, *width, *height, file,
                                             0x001F << 10, 0x001F << 5, 0x0001F, 0);
                    break;
                case 32: /* Read 32-bit BGRA image data */
                    success = read_bmp_32bpp(*image, *width, *height, file,
                                             0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
                    break;
            }
            break;
        case 1: /* 8-bit RLE */
            if (bits_per_pixel == 8)
                success = read_bmp_8bpp_rle(
                        *image, *width, *height, file, palette);
            break;
        case 2: /* 4-bit RLE */
            if (bits_per_pixel == 4)
                success = read_bmp_4bpp_rle(
                        *image, *width, *height, file, palette);
            break;
        case 3: /* bitfields data */
            switch (bits_per_pixel) {
                case 16: /* Read 16-bit bitfields data */
                    success = read_bmp_16bpp(*image, *width, *height, file,
                                             redmask, greenmask, bluemask, alphamask);
                    break;
                case 32: /* Read 32-bit bitfields data */
                    success = read_bmp_32bpp(*image, *width, *height, file,
                                             redmask, greenmask, bluemask, alphamask);
                    break;
            }
            break;
    }

    if (!success)
        fprintf(stderr, "Error reading BMP data.\n");

    fail:   /* There was a problem, clean up and exit */
    if (palette)
        free(palette);

    if (!success && *image)
        free(*image);

    return success;
}


/**
* \brief Write a BMP image
*
* \param image pointer to RGBA image data
* \param width, height the image dimensions
* \param file stdio FILE pointer
*
* \return 1 on success, 0 on failure
*
* This function is called by \c write_image to write BMP images.  The caller
* should open \c file in binary write mode.  When \c write_bmp is complete,
* the caller should close \c file.
*
* The image is generally saved in uncompressed 24-bit RGB format. But where
* possible, the image is saved using an 8-bit palette for a substantial
* decrease in file size.  The image data is always saved losslessly.
*
* \note The alpha channel is lost when saving to BMP.  It is possible to write
*       the alpha channel in a 32-bit BMP image, however, such images are not
*       widely supported.  RGB 24-bit BMP on the other hand is well supported.
*/
static int write_bmp(const uint32_t *image, int width, int height, FILE *file) {
    const uint8_t *image_ptr = (uint8_t *) image;
    uint32_t *palette = NULL;
    uint32_t pixel;
    long int imageSize;
    int use_palette, num_colors, use_color, use_alpha;
    int x, y, i, row_padding, success = 0;


    if (!image)
        return 0;

    palette = get_image_palette(&num_colors, &use_color, &use_alpha,
                                image, width, height);

    /* Decide whether to use 8-bit palette or 24-bit RGB format */
    if (palette && 2 * num_colors < width * height)
        use_palette = 1;
    else
        use_palette = num_colors = 0;

    /* Tell file to use buffering */
    setvbuf(file, 0, _IOFBF, FILE_BUFFER_CAPACITY);

    if (use_palette) {
        row_padding = (-width) & 3;
        imageSize = (width + row_padding) * ((long int) height);
    } else {
        row_padding = (-3 * width) & 3;
        imageSize = (3 * width + row_padding) * ((long int) height);
    }

    /*** Write the header ***/

    /* Write the BMP header */
    putc(0x42, file);                         /* Magic numbers             */
    putc(0x4D, file);

    /* filesize */
    write_u32_le(54 + 4 * num_colors + imageSize, file);

    write_u32_le(0, file);                    /* Reserved fields           */
    write_u32_le(54 + 4 * num_colors, file);    /* Image data offset         */

    /* Write the infoheader */
    write_u32_le(40, file);                   /* infoheader size           */
    write_u32_le(width, file);                /* Image width               */
    write_u32_le(height, file);               /* Image height              */
    write_u16_le(1, file);                    /* Number of colorplanes     */
    write_u16_le((use_palette) ? 8 : 24, file); /* Bits per pixel            */
    write_u32_le(0, file);                    /* Compression method (none) */
    write_u32_le(imageSize, file);            /* Image size                */
    write_u32_le(2835, file);                 /* HResolution (2835=72dpi)  */
    write_u32_le(2835, file);                 /* VResolution               */

    /* Number of colors */
    write_u32_le((!use_palette || num_colors == 256) ? 0 : num_colors, file);

    write_u32_le(0, file);                    /* Important colors          */

    if (ferror(file)) {
        fprintf(stderr, "Error during write to file.\n");
        goto fail;
    }

    if (use_palette) {   /* Write the palette */
        for (i = 0; i < num_colors; ++i) {
            pixel = palette[i];
            putc(((uint8_t *) &pixel)[2], file);     /* Blue   */
            putc(((uint8_t *) &pixel)[1], file);     /* Green  */
            putc(((uint8_t *) &pixel)[0], file);     /* Red    */
            putc(0, file);                          /* Unused */
        }
    }

    /* Write the image data */
    width <<= 2;
    image_ptr += ((long int) width) * ((long int) height - 1);

    for (y = height; y; --y, image_ptr -= width) {
        if (use_palette) {   /* 8-bit palette image data */
            for (x = 0; x < width; x += 4) {
                pixel = *((uint32_t *) (image_ptr + x));

                for (i = 0; i < num_colors; ++i)
                    if (pixel == palette[i])
                        break;

                putc(i, file);
            }
        } else {   /* 24-bit RGB image data */
            for (x = 0; x < width; x += 4) {
                putc(image_ptr[x + 2], file);  /* Write blue component  */
                putc(image_ptr[x + 1], file);  /* Write green component */
                putc(image_ptr[x + 0], file);  /* Write red component   */
            }
        }

        for (x = row_padding; x; --x)         /* Write row padding */
            putc(0, file);
    }

    if (ferror(file)) {
        fprintf(stderr, "Error during write to file.\n");
        goto fail;
    }

    success = 1;
    fail:
    if (palette)
        free(palette);
    return success;
}


/** \brief Convert from RGBA U8 to a specified format */
static void *convert_to_format(uint32_t *src, int width, int height,
                               unsigned format) {
    const int num_pixels = width * height;
    const int num_channels = (format & IMAGEIO_GRAYSCALE) ?
                             1 : ((format & IMAGEIO_STRIP_ALPHA) ? 3 : 4);
    const int channel_stride = (format & IMAGEIO_PLANAR) ? num_pixels : 1;
    const int channel_stride2 = 2 * channel_stride;
    const int channel_stride3 = 3 * channel_stride;
    float *dest_f64;
    float *dest_f32;
    uint8_t *dest_u8;
    uint32_t pixel;
    int order[4] = {0, 1, 2, 3};
    int i, x, y, pixel_stride, row_stride;


    pixel_stride = (format & IMAGEIO_PLANAR) ? 1 : num_channels;

    if (format & IMAGEIO_COLUMNMAJOR) {
        row_stride = pixel_stride;
        pixel_stride *= height;
    } else
        row_stride = width * pixel_stride;

    if (format & IMAGEIO_BGRFLIP) {
        order[0] = 2;
        order[2] = 0;
    }

    if ((format & IMAGEIO_AFLIP) && !(format & IMAGEIO_STRIP_ALPHA)) {
        order[3] = order[2];
        order[2] = order[1];
        order[1] = order[0];
        order[0] = 3;
    }

    switch (format & (IMAGEIO_U8 | IMAGEIO_SINGLE | IMAGEIO_float)) {
        case IMAGEIO_U8:  /* Destination type is uint8_t. */
            if (!(dest_u8 = (uint8_t *) malloc(
                    sizeof(uint8_t) * num_channels * num_pixels)))
                return NULL;

            switch (num_channels) {
                case 1: /* Convert RGBA U8 to grayscale U8. */
                    for (y = 0; y < height; ++y, src += width)
                        for (x = 0, i = row_stride * y;
                             x < width;
                             ++x, i += pixel_stride) {
                            pixel = src[x];
                            dest_u8[i] = (uint8_t) (0.299f * ((uint8_t *) &pixel)[0]
                                                    + 0.587f * ((uint8_t *) &pixel)[1]
                                                    + 0.114f * ((uint8_t *) &pixel)[2] + 0.5f);
                        }
                    break;
                case 3: /* Convert RGBA U8 to RGB (or BGR) U8. */
                    for (y = 0; y < height; ++y, src += width)
                        for (x = 0, i = row_stride * y;
                             x < width;
                             ++x, i += pixel_stride) {
                            pixel = src[x];
                            dest_u8[i] =
                                    ((uint8_t *) &pixel)[order[0]];
                            dest_u8[i + channel_stride] =
                                    ((uint8_t *) &pixel)[order[1]];
                            dest_u8[i + channel_stride2] =
                                    ((uint8_t *) &pixel)[order[2]];
                        }
                    break;
                case 4: /* Convert RGBA U8 to RGBA (or BGRA, ARGB, or ABGR) U8. */
                    for (y = 0; y < height; ++y, src += width)
                        for (x = 0, i = row_stride * y;
                             x < width;
                             ++x, i += pixel_stride) {
                            pixel = src[x];
                            dest_u8[i] =
                                    ((uint8_t *) &pixel)[order[0]];
                            dest_u8[i + channel_stride] =
                                    ((uint8_t *) &pixel)[order[1]];
                            dest_u8[i + channel_stride2] =
                                    ((uint8_t *) &pixel)[order[2]];
                            dest_u8[i + channel_stride3] =
                                    ((uint8_t *) &pixel)[order[3]];
                        }
                    break;
            }
            return dest_u8;
        case IMAGEIO_SINGLE:  /* Destination type is float. */
            if (!(dest_f32 = (float *) malloc(
                    sizeof(float) * num_channels * num_pixels)))
                return NULL;

            switch (num_channels) {
                case 1: /* Convert RGBA U8 to grayscale float. */
                    for (y = 0; y < height; ++y, src += width)
                        for (x = 0, i = row_stride * y;
                             x < width; ++x,
                                     i += pixel_stride) {
                            pixel = src[x];
                            dest_f32[i] = 1.172549019607843070675535e-3f
                                          * ((uint8_t *) &pixel)[0]
                                          + 2.301960784313725357840079e-3f
                                            * ((uint8_t *) &pixel)[1]
                                          + 4.470588235294117808150007e-4f
                                            * ((uint8_t *) &pixel)[2];
                        }
                    break;
                case 3: /* Convert RGBA U8 to RGB (or BGR) float. */
                    for (y = 0; y < height; ++y, src += width)
                        for (x = 0, i = row_stride * y;
                             x < width;
                             ++x, i += pixel_stride) {
                            pixel = src[x];
                            dest_f32[i] =
                                    ((uint8_t *) &pixel)[order[0]] / 255.0f;
                            dest_f32[i + channel_stride] =
                                    ((uint8_t *) &pixel)[order[1]] / 255.0f;
                            dest_f32[i + channel_stride2] =
                                    ((uint8_t *) &pixel)[order[2]] / 255.0f;
                        }
                    break;
                case 4: /* Convert RGBA U8 to RGBA (or BGRA, ARGB, or ABGR) float. */
                    for (y = 0; y < height; ++y, src += width)
                        for (x = 0, i = row_stride * y;
                             x < width;
                             ++x, i += pixel_stride) {
                            pixel = src[x];
                            dest_f32[i] =
                                    ((uint8_t *) &pixel)[order[0]] / 255.0f;
                            dest_f32[i + channel_stride] =
                                    ((uint8_t *) &pixel)[order[1]] / 255.0f;
                            dest_f32[i + channel_stride2] =
                                    ((uint8_t *) &pixel)[order[2]] / 255.0f;
                            dest_f32[i + channel_stride3] =
                                    ((uint8_t *) &pixel)[order[3]] / 255.0f;
                        }
                    break;
            }
            return dest_f32;
        case IMAGEIO_float:  /* Destination type is float. */
            if (!(dest_f64 = (float *)
                    malloc(sizeof(float) * num_channels * num_pixels)))
                return NULL;

            switch (num_channels) {
                case 1: /* Convert RGBA U8 to grayscale float. */
                    for (y = 0; y < height; ++y, src += width)
                        for (x = 0, i = row_stride * y;
                             x < width;
                             ++x, i += pixel_stride) {
                            pixel = src[x];
                            dest_f64[i] = 1.172549019607843070675535e-3
                                          * ((uint8_t *) &pixel)[0]
                                          + 2.301960784313725357840079e-3
                                            * ((uint8_t *) &pixel)[1]
                                          + 4.470588235294117808150007e-4
                                            * ((uint8_t *) &pixel)[2];
                        }
                    break;
                case 3: /* Convert RGBA U8 to RGB (or BGR) float. */
                    for (y = 0; y < height; ++y, src += width)
                        for (x = 0, i = row_stride * y;
                             x < width;
                             ++x, i += pixel_stride) {
                            pixel = src[x];
                            dest_f64[i] =
                                    ((uint8_t *) &pixel)[order[0]] / 255.0f;
                            dest_f64[i + channel_stride] =
                                    ((uint8_t *) &pixel)[order[1]] / 255.0f;
                            dest_f64[i + channel_stride2] =
                                    ((uint8_t *) &pixel)[order[2]] / 255.0f;
                        }
                    break;
                case 4: /* Convert RGBA U8 to RGBA (or BGRA, ARGB, or ABGR) float. */
                    for (y = 0; y < height; ++y, src += width)
                        for (x = 0, i = row_stride * y;
                             x < width;
                             ++x, i += pixel_stride) {
                            pixel = src[x];
                            dest_f64[i] =
                                    ((uint8_t *) &pixel)[order[0]] / 255.0f;
                            dest_f64[i + channel_stride] =
                                    ((uint8_t *) &pixel)[order[1]] / 255.0f;
                            dest_f64[i + channel_stride2] =
                                    ((uint8_t *) &pixel)[order[2]] / 255.0f;
                            dest_f64[i + channel_stride3] =
                                    ((uint8_t *) &pixel)[order[3]] / 255.0f;
                        }
                    break;
            }
            return dest_f64;
        default:
            return NULL;
    }
}


/** \brief Convert from a specified format to RGBA U8 */
static uint32_t *convert_from_format(void *src, int width, int height,
                                     unsigned format) {
    const int num_pixels = width * height;
    const int num_channels = (format & IMAGEIO_GRAYSCALE) ?
                             1 : ((format & IMAGEIO_STRIP_ALPHA) ? 3 : 4);
    const int channel_stride = (format & IMAGEIO_PLANAR) ? num_pixels : 1;
    const int channel_stride2 = 2 * channel_stride;
    const int channel_stride3 = 3 * channel_stride;
    float *src_f64 = (float *) src;
    float *src_f32 = (float *) src;
    uint8_t *src_u8 = (uint8_t *) src;
    uint8_t *dest, *dest_ptr;
    int order[4] = {0, 1, 2, 3};
    int i, x, y, pixel_stride, row_stride;

    if (!(dest = (uint8_t *) malloc(sizeof(uint32_t) * num_pixels)))
        return NULL;

    dest_ptr = dest;
    pixel_stride = (format & IMAGEIO_PLANAR) ? 1 : num_channels;

    if (format & IMAGEIO_COLUMNMAJOR) {
        row_stride = pixel_stride;
        pixel_stride *= height;
    } else
        row_stride = width * pixel_stride;

    if (format & IMAGEIO_BGRFLIP) {
        order[0] = 2;
        order[2] = 0;
    }

    if ((format & IMAGEIO_AFLIP) && !(format & IMAGEIO_STRIP_ALPHA)) {
        order[3] = order[2];
        order[2] = order[1];
        order[1] = order[0];
        order[0] = 3;
    }

    switch (format & (IMAGEIO_U8 | IMAGEIO_SINGLE | IMAGEIO_float)) {
        case IMAGEIO_U8:  /* Source type is uint8_t. */
            switch (num_channels) {
                case 1: /* Convert grayscale U8 to RGBA U8. */
                    for (y = 0; y < height; ++y, dest_ptr += 4 * width)
                        for (x = 0, i = row_stride * y;
                             x < width;
                             ++x, i += pixel_stride) {
                            dest_ptr[4 * x] =
                            dest_ptr[4 * x + 1] =
                            dest_ptr[4 * x + 2] = src_u8[i];
                            dest_ptr[4 * x + 3] = 255;
                        }
                    break;
                case 3: /* Convert RGB (or BGR) U8 to RGBA U8. */
                    for (y = 0; y < height; ++y, dest_ptr += 4 * width)
                        for (x = 0, i = row_stride * y;
                             x < width;
                             ++x, i += pixel_stride) {
                            dest_ptr[4 * x + order[0]] = src_u8[i];
                            dest_ptr[4 * x + order[1]] = src_u8[i + channel_stride];
                            dest_ptr[4 * x + order[2]] = src_u8[i + channel_stride2];
                            dest_ptr[4 * x + 3] = 255;
                        }
                    break;
                case 4: /* Convert RGBA U8 to RGBA (or BGRA, ARGB, or ABGR) U8. */
                    for (y = 0; y < height; ++y, dest_ptr += 4 * width)
                        for (x = 0, i = row_stride * y;
                             x < width;
                             ++x, i += pixel_stride) {
                            dest_ptr[4 * x + order[0]] = src_u8[i];
                            dest_ptr[4 * x + order[1]] = src_u8[i + channel_stride];
                            dest_ptr[4 * x + order[2]] = src_u8[i + channel_stride2];
                            dest_ptr[4 * x + order[3]] = src_u8[i + channel_stride3];
                        }
                    break;
            }
            break;
        case IMAGEIO_SINGLE:  /* Source type is float. */
            switch (num_channels) {
                case 1: /* Convert grayscale float to RGBA U8. */
                    for (y = 0; y < height; ++y, dest_ptr += 4 * width)
                        for (x = 0, i = row_stride * y;
                             x < width;
                             ++x, i += pixel_stride) {
                            dest_ptr[4 * x] =
                            dest_ptr[4 * x + 1] =
                            dest_ptr[4 * x + 2] = ROUNDCLAMPF(src_f32[i]);
                            dest_ptr[4 * x + 3] = 255;
                        }
                    break;
                case 3: /* Convert RGBA U8 to RGB (or BGR) float. */
                    for (y = 0; y < height; ++y, dest_ptr += 4 * width)
                        for (x = 0, i = row_stride * y;
                             x < width;
                             ++x, i += pixel_stride) {
                            dest_ptr[4 * x + order[0]] =
                                    ROUNDCLAMPF(src_f32[i]);
                            dest_ptr[4 * x + order[1]] =
                                    ROUNDCLAMPF(src_f32[i + channel_stride]);
                            dest_ptr[4 * x + order[2]] =
                                    ROUNDCLAMPF(src_f32[i + channel_stride2]);
                            dest_ptr[4 * x + 3] = 255;
                        }
                    break;
                case 4: /* Convert RGBA U8 to RGBA (or BGRA, ARGB, or ABGR) float. */
                    for (y = 0; y < height; ++y, dest_ptr += 4 * width)
                        for (x = 0, i = row_stride * y;
                             x < width;
                             ++x, i += pixel_stride) {
                            dest_ptr[4 * x + order[0]] =
                                    ROUNDCLAMPF(src_f32[i]);
                            dest_ptr[4 * x + order[1]] =
                                    ROUNDCLAMPF(src_f32[i + channel_stride]);
                            dest_ptr[4 * x + order[2]] =
                                    ROUNDCLAMPF(src_f32[i + channel_stride2]);
                            dest_ptr[4 * x + order[3]] =
                                    ROUNDCLAMPF(src_f32[i + channel_stride3]);
                        }
                    break;
            }
            break;
        case IMAGEIO_float:  /* Source type is float. */
            switch (num_channels) {
                case 1: /* Convert grayscale float to RGBA U8. */
                    for (y = 0; y < height; ++y, dest_ptr += 4 * width)
                        for (x = 0, i = row_stride * y;
                             x < width;
                             ++x, i += pixel_stride) {
                            dest_ptr[4 * x] =
                            dest_ptr[4 * x + 1] =
                            dest_ptr[4 * x + 2] = ROUNDCLAMP(src_f64[i]);
                            dest_ptr[4 * x + 3] = 255;
                        }
                    break;
                case 3: /* Convert RGB (or BGR) float to RGBA U8. */
                    for (y = 0; y < height; ++y, dest_ptr += 4 * width)
                        for (x = 0, i = row_stride * y;
                             x < width;
                             ++x, i += pixel_stride) {
                            dest_ptr[4 * x + order[0]] =
                                    ROUNDCLAMP(src_f64[i]);
                            dest_ptr[4 * x + order[1]] =
                                    ROUNDCLAMP(src_f64[i + channel_stride]);
                            dest_ptr[4 * x + order[2]] =
                                    ROUNDCLAMP(src_f64[i + channel_stride2]);
                            dest_ptr[4 * x + 3] = 255;;
                        }
                    break;
                case 4: /* Convert RGBA (or BGRA, ARGB, or ABGR) float to RGBA U8. */
                    for (y = 0; y < height; ++y, dest_ptr += 4 * width)
                        for (x = 0, i = row_stride * y;
                             x < width;
                             ++x, i += pixel_stride) {
                            dest_ptr[4 * x + order[0]] =
                                    ROUNDCLAMP(src_f64[i]);
                            dest_ptr[4 * x + order[1]] =
                                    ROUNDCLAMP(src_f64[i + channel_stride]);
                            dest_ptr[4 * x + order[2]] =
                                    ROUNDCLAMP(src_f64[i + channel_stride2]);
                            dest_ptr[4 * x + order[3]] =
                                    ROUNDCLAMP(src_f64[i + channel_stride3]);
                        }
                    break;
            }
            break;
        default:
            return NULL;
    }

    return (uint32_t *) dest;
}


/**
 * \brief Identify the file type of an image file by its magic numbers
 * \param type destination buffer with space for at least 5 chars
 * \param filename image file name
 * \return 1 on successful identification, 0 on failure.
 *
 * The routine fills type with an identifying string. If there is an error
 * or the file type is unknown, type is set to a null string.
 */
int identify_image_type(char *type, const char *filename) {
    FILE *file;
    uint32_t magic;

    type[0] = '\0';

    if (!(file = fopen(filename, "rb")))
        return 0;

    /* Determine the file format by reading the first 4 bytes */
    magic = ((uint32_t) getc(file));
    magic |= ((uint32_t) getc(file)) << 8;
    magic |= ((uint32_t) getc(file)) << 16;
    magic |= ((uint32_t) getc(file)) << 24;

    /* Test for errors */
    if (ferror(file)) {
        fclose(file);
        return 0;
    }

    fclose(file);

    if ((magic & 0x0000FFFFL) == 0x00004D42L)                /* BMP */
        strcpy(type, "BMP");
    else if ((magic & 0x00FFFFFFL) == 0x00FFD8FFL)           /* JPEG/JFIF */
        strcpy(type, "JPEG");
    else if (magic == 0x474E5089L)                           /* PNG */
        strcpy(type, "PNG");
    else if (magic == 0x002A4949L || magic == 0x2A004D4DL)   /* TIFF */
        strcpy(type, "TIFF");
    else if (magic == 0x38464947L)                           /* GIF */
        strcpy(type, "GIF");
    else if (magic == 0x474E4D8AL)                           /* MNG */
        strcpy(type, "MNG");
    else if ((magic & 0xF0FF00FFL) == 0x0001000AL            /* PCX */
             && ((magic >> 8) & 0xFF) < 6)
        strcpy(type, "PCX");
    else
        return 0;

    return 1;
}


/**
* \brief Read an image file as 32-bit RGBA data
*
* \param width, height pointers to be filled with the image dimensions
* \param filename image file name
* \param format specifies the desired format for the image
*
* \return Pointer to the image data, or null on failure
*
* The calling syntax is that the filename is the input and \c width,
* and \c height and the returned pointer are outputs. \c read_image allocates
* memory for the image as one contiguous block of memory and returns a
* pointer. It is the responsibility of the caller to call \c free on this
* pointer when done to release this memory.
*
* A non-null pointer indicates success. On failure, the returned pointer
* is null, and \c width and \c height are set to 0.
*
* The format argument is used by specifying one of the data type options
*
*  - IMAGEIO_U8:            unsigned 8-bit components
*  - IMAGEIO_SINGLE:        float components
*  - IMAGEIO_float:        float components
*
* and one of the channel options
*
*  - IMAGEIO_GRAYSCALE:     grayscale data
*  - IMAGEIO_RGB:           RGB color data (red is the first channel)
*  - IMAGEIO_BGR:           BGR color data (blue is the first channel)
*  - IMAGEIO_RGBA:          RGBA color+alpha data
*  - IMAGEIO_BGRA:          BGRA color+alpha data
*  - IMAGEIO_ARGB:          ARGB color+alpha data
*  - IMAGEIO_ABGR:          ABGR color+alpha data
*
* and optionally either or both of the ordering options
*
*  - IMAGEIO_PLANAR:        planar order instead of interleaved components
*  - IMAGEIO_COLUMNMAJOR:   column major order instead of row major order
*
\code
    uint32_t *image;
    int width, height;
    
    if (!(image = (uint32_t *)read_image(&width, &height, "myimage.bmp",
        IMAGEIO_U8 | IMAGEIO_RGBA)))
        return 0;
    
    printf("Read image of size %dx%d\n", width, height);
    
    ...
    
    free(image);
\endcode
*
* With the default formatting IMAGEIO_U8 | IMAGEIO_RGBA, the image is
* organized in standard row major top-down 32-bit RGBA order. The image
* is organized as
\verbatim
    (Top left)                                             (Top right)
    image[0]                image[1]        ...  image[width-1]
    image[width]            image[width+1]  ...  image[2*width]
    ...                     ...             ...  ...
    image[width*(height-1)] ...             ...  image[width*height-1]
    (Bottom left)                                       (Bottom right)
\endverbatim
* Each element \c image[k] represents one RGBA pixel, which is a 32-bit
* bitfield. The components of pixel \c image[k] can be unpacked as
\code
    uint8_t *Component = (uint8_t *)&image[k];
    uint8_t red = Component[0];
    uint8_t green = Component[1];
    uint8_t blue = Component[2];
    uint8_t alpha = Component[3];
\endcode
* Each component is an unsigned 8-bit integer value with range 0-255. Most
* images do not have alpha information, in which case the alpha component
* is set to value 255 (full opacity).
*
* With IMAGEIO_SINGLE or IMAGEIO_float, the components are values in the
* range 0 to 1.
*/
void *read_image(int *width, int *height,
                 const char *filename, unsigned format) {
    void *image = NULL;
    int Channels = 0;
    uint32_t *image_u8 = (uint32_t *) (stbi_load(filename, width, height, &Channels, 4));

    if (image_u8 && format) {
        image = convert_to_format(image_u8, *width, *height, format);
        free(image_u8);
    } else
        image = image_u8;

    return image;
}


/**
* \brief Write an image file from 8-bit RGBA image data
*
* \param image pointer to the image data
* \param width, height image dimensions
* \param filename image file name
* \param format specifies how the data is formatted (see read_image)
* \param quality the JPEG image quality (between 0 and 100)
*
* \return 1 on success, 0 on failure
*
* The input \c image should be a 32-bit RGBA image stored as in the
* description of \c read_image. \c write_image writes to \c filename in the
* file format specified by its extension. If saving a JPEG image, the
* \c quality argument specifies the quality factor (between 0 and 100).
* \c quality has no effect on other formats.
*
* The return value indicates success with 1 or failure with 0.
*/
int write_image(void *image, int width, int height,
                const char *filename, unsigned format, int quality) {

    uint32_t *image_u8;
    enum {
        BMP_FORMAT, JPEG_FORMAT, PNG_FORMAT, TIFF_FORMAT
    } fileformat;
    int success = 0;

    if (!image || width <= 0 || height <= 0) {
        fprintf(stderr, "Null image.\n");
        fprintf(stderr, "Failed to write \"%s\".\n", filename);
        return 0;
    }

    if (string_ends_with(filename, ".bmp"))
        fileformat = BMP_FORMAT;
    else if (string_ends_with(filename, ".jpg")
             || string_ends_with(filename, ".jpeg")) {
        fileformat = JPEG_FORMAT;
    } else if (string_ends_with(filename, ".png")) {
        fileformat = PNG_FORMAT;
    } else {
        fprintf(stderr, "Failed to write \"%s\".\n", filename);

        return 0;
    }

    if (!(image_u8 = convert_from_format(image, width, height, format)))
        return 0;
    const int num_channels = 4;
    switch (fileformat) {
        case BMP_FORMAT:
            success = stbi_write_bmp(filename, width, height, num_channels, image_u8);
            break;
        case JPEG_FORMAT:
            success = stbi_write_jpg(filename, width, height, num_channels, image_u8, quality);
            break;
        case PNG_FORMAT:
            success = stbi_write_png(filename, width, height, num_channels, image_u8, 0);
            break;
    }
    if (!success)
        fprintf(stderr, "Failed to write \"%s\".\n", filename);

    free(image_u8);
    return success;
}
