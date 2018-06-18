/*
 * Copyright (c) 2016, Anmol Popli <anmol.ap020@gmail.com>
 * All rights reserved.
 *
 * This program is free software: you can use, modify and/or
 * redistribute it under the terms of the GNU General Public
 * License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later
 * version. You should have received a copy of this license along
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */


/**
 * @file young.c
 * @brief Fast IIR approximation of gaussian filter using Young's approach
 *
 * @author ANMOL POPLI <anmol.ap020@gmail.com>
 *         PRAVIN NAIR  <sreehari1390@gmail.com>
 **/

#include "headersreq.h"

void convolve_young2D(int rows, int columns, int sigma, fft_complex **ip_padded);

void symmetric_padding(int rows, int columns, fft_complex **in, int w);

float bf[3], bb[3], B;
int w;

/**
 * \brief Convolve input array with 1D Causal filter
 *        (Young and van Vliet's algorithm) 
 * \param in        Pointer to input array
 * \param out       Pointer to output array
 * \param datasize  Input array size
 *
 * This routine performs constant time convolution of the
 * 1D input array of complex floats with 1D Causal filter
 * of Young and van Vliet's algorithm. The 1D filter is an
 * IIR filter. 
 */

void convolve_youngCausal(fft_complex *in, fft_complex *out, int datasize) {
	 

    /* Compute first 3 output elements */
    out[0].real =  B* in[0].real;
	out[0].imag = B* in[0].imag;
    out[1].real =   (B* in[1].real)+  (bf[2]* out[0].real);
	out[1].imag =   (B* in[1].imag)+  (bf[2]* out[0].imag);
    out[2].real =  ( (B* in[2].real)+  ( (bf[1] * out[0].real)+  (bf[2] * out[1].real)));
	out[2].imag =  ( (B* in[2].imag)+  ( (bf[1] * out[0].imag)+  (bf[2] * out[1].imag)));

    /* Recursive computation of output in forward direction using filter parameters bf and B */
    for (int i = 3; i < datasize; i++) { 

		out[i].real = B * in[i].real;
		out[i].imag = B * in[i].imag;
        for (int j = 0; j < 3; j++) {
            out[i].real +=  (   (bf[j]*out[i - (3 - j)].real));
			out[i].imag +=  (   (bf[j]*out[i - (3 - j)].imag));
        }
    }

}

/**
 * \brief Convolve input array with 1D AntiCausal filter
 *        (Young and van Vliet's algorithm) 
 * \param in        Pointer to input array
 * \param out       Pointer to output array
 * \param datasize  Input array size
 *
 * This routine performs constant time convolution of the
 * 1D input array of complex floats with 1D AntiCausal filter
 * of Young and van Vliet's algorithm. The 1D filter is an
 * IIR filter. 
 */
void convolve_youngAnticausal(fft_complex *in, fft_complex *out, int datasize) {
	 

    /* Compute last 3 output elements */
    out[datasize - 1].real =  (B* in[datasize - 1].real);
	out[datasize - 1].imag =  (B* in[datasize - 1].imag);
    out[datasize - 2].real =  ( (B* in[datasize - 2].real)+  (bb[0]* out[datasize - 1].real));
	out[datasize - 2].imag =  ( (B* in[datasize - 2].imag)+  (bb[0]* out[datasize - 1].imag));
    out[datasize - 3].real =  ( (B* in[datasize - 3].real)+
                             ( ( (bb[0]* out[datasize - 2].real)+ (bb[1]* out[datasize - 1].real))));
	out[datasize - 3].imag = ((B* in[datasize - 3].imag) +
		(((bb[0] * out[datasize - 2].imag) + (bb[1] * out[datasize - 1].imag))));

    /* Recursive computation of output in backward direction using filter parameters bb and B */
    for  (int i = datasize - 4; i >= w; i--) {
        out[i].real =  (B* in[i].real);
		out[i].imag =  (B* in[i].imag);
        for (int j = 0; j < 3; j++) {
            out[i].real +=  (  (bb[j]* out[i + (j + 1)].real));
			out[i].imag +=  (  (bb[j]* out[i + (j + 1)].imag));
        }
    }

}

/**
 * \brief Convolve input array with 1D Gaussian filter
 *        (Young and van Vliet's algorithm) 
 * \param in        Pointer to input array
 * \param out       Pointer to output array
 * \param datasize  Input array size
 *
 * This routine performs constant time convolution of the
 * 1D input array of complex floats with 1D Gaussian filter
 * using Young and van Vliet's algorithm. The input array is
 * first convolved with 1D Causal filter, the result of
 * which is convolved with 1D AntiCausal filter.
 */
void convolve_young1D(fft_complex *in, fft_complex *out, int datasize) {
    /** \brief Array to store output of Causal filter convolution */
    convolve_youngCausal(in, out, datasize);
    convolve_youngAnticausal(out, in, datasize);
}

/**
 * \brief Apply 2D Gaussian filter to input image
 *        (Young and van Vliet's algorithm) 
 * \param rows      Image height
 * \param columns   Image width
 * \param sigma     Gaussian kernel standard deviation
 * \param ip_padded Pointer to input image
 * \param op_padded Pointer to output image
 *
 * This routine applies 2D Gaussian filter of s.d.
 * sigma to input image ip_padded of dimensions
 * rows x columns and computes output image op_padded.
 * 1D filter is first convolved along rows and then
 * along columns. The 1D convolution is performed using
 * Young and van Vliet's fast recursive algorithm.
 */
void convolve_young2D(int rows, int columns, int sigma, fft_complex **ip_padded) {

    /** \brief Filter radius */
    w = 3 * sigma;
    /** \brief Filter parameter q */
    float q;
    if (sigma < 2.5)
        q = 3.97156f - 4.14554f * sqrtf(1 - 0.26891f * sigma);
    else
        q = 0.98711f * sigma - 0.9633f;

    /** \brief Filter parameters b0, b1, b2, b3 */
    float b0 = 1.57825f + 2.44413f * q + 1.4281f * q * q + 0.422205f * q * q * q;
    float b1 = 2.44413f * q + 2.85619f* q * q + 1.26661f * q * q * q;
    float b2 = -(1.4281f * q * q + 1.26661f * q * q * q);
    float b3 = 0.422205f * q * q * q;
	float invb0 = 1.0f / b0;
    /** \brief Filter parameters bf, bb, B */
    bf[0] = b3 * invb0  ;
    bf[1] = b2 * invb0;
    bf[2] = b1 * invb0;
    bb[0] = b1 * invb0;
    bb[1] = b2 * invb0;
    bb[2] = b3 * invb0;
    B = 1 - (b1 + b2 + b3) * invb0; 
    symmetric_padding(rows, columns, ip_padded, w);
    /* Convolve each row with 1D Gaussian filter */
    fft_complex *out_t = calloc(columns + (2 * w), sizeof(fft_complex));
    for (int i = 0; i < rows + 2 * w; i++) {
        convolve_young1D(ip_padded[i], out_t, columns + 2 * w);
    }
    free(out_t);
    fft_complex *intemp = calloc(rows + (2 * w), sizeof(fft_complex)), *outtemp = calloc(rows + (2 * w),
                                                                                         sizeof(fft_complex));
    for (int j = w; j < columns + w; j++) {
        /* Convolve each column with 1D Gaussian filter */
        for (int i = 0; i < rows + (2 * w); i++) {
            intemp[i] = ip_padded[i][j];
        }
        convolve_young1D(intemp, outtemp, rows + 2 * w);
        /* Store the convolved column in row of output matrix*/
        for (int i = 0; i < rows + (2 * w); i++) {
            ip_padded[i][j] = intemp[i];
        }
    }
    free(intemp);
    free(outtemp);
}

/**
 * \brief Apply symmetric padding to input image
 *        (Young and van Vliet's algorithm) 
 * \param rows      Image height
 * \param columns   Image width
 * \param in Pointer to input image padded with zeros
 *
 * This routine applies mirror boundary conditions
 * to input image which is zero padded i.e size of
 * input image will be [rows+2*w, columns+2*w]
 */
void symmetric_padding(int rows, int columns, fft_complex **in, int w) {
    int i, j;
    fft_complex pixval;
    /* Rows 0 - w are duplicated to satisfy mirror bondary condition */
    for (i = 0; i < w; i++) {
        for (j = 0; j < columns; j++) {
            pixval = in[i + w][j + w];
            if (j < w) {
                in[i + w][w - 1 - j] = pixval;
                in[w - 1 - i][w - 1 - j] = pixval;
            }
            if (j >= columns - w) {
                in[i + w][columns + w + (columns - j) - 1] = pixval;
                in[w - 1 - i][columns + w + (columns - j) - 1] = pixval;
            }
            in[w - 1 - i][j + w] = pixval;
        }
    }
    /* Rows rows-w - rows  are duplicated to satisfy mirror bondary condition */
    for (i = rows - w; i < rows; i++) {
        for (j = 0; j < columns; j++) {
            pixval = in[i + w][j + w];
            if (j < w) {
                in[i + w][w - 1 - j] = pixval;
                in[rows + w + (rows - i) - 1][w - 1 - j] = pixval;
            }
            if (j >= columns - w) {
                in[i + w][columns + w + (columns - j) - 1] = pixval;
                in[rows + w + (rows - i) - 1][columns + w + (columns - j) - 1] = pixval;
            }
            in[rows + w + (rows - i) - 1][j + w] = pixval;
        }
    }
    /* Remaining rows are duplicated to satisfy mirror bondary condition*/
    for (i = w; i < rows - w; i++) {
        for (j = 0; j < w; j++) {
            in[i + w][w - 1 - j] = in[i + w][j + w];
        }
    }
    for (i = w; i < rows - w; i++) {
        for (j = columns - w; j < columns; j++) {
            in[i + w][columns + w + (columns - j) - 1] = in[i + w][j + w];
        }
    }

}

