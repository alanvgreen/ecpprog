/*
 *  iceprog -- simple programming tool for FTDI-based Lattice iCE programmers
 *
 *  Copyright (C) 2015  Clifford Wolf <clifford@clifford.at>
 *  Copyright (C) 2018  Piotr Esden-Tempski <piotr@esden.net>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 *  Relevant Documents:
 *  -------------------
 *  http://www.ftdichip.com/Support/Documents/AppNotes/AN_108_Command_Processor_for_MPSSE_and_MCU_Host_Bus_Emulation_Modes.pdf
 */

#define _GNU_SOURCE

#include <ftdi.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#include "mpsse.h"

// ---------------------------------------------------------
// MPSSE / FTDI definitions
// ---------------------------------------------------------

/* FTDI bank pinout typically used for iCE dev boards
 * BUS IO | Signal | Control
 * -------+--------+--------------
 * xDBUS0 |    SCK | MPSSE
 * xDBUS1 |   MOSI | MPSSE
 * xDBUS2 |   MISO | MPSSE
 * xDBUS3 |     nc |
 * xDBUS4 |     CS | GPIO
 * xDBUS5 |     nc |
 * xDBUS6 |  CDONE | GPIO
 * xDBUS7 | CRESET | GPIO
 */

struct ftdi_context mpsse_ftdic;
bool mpsse_ftdic_open = false;
bool mpsse_ftdic_latency_set = false;
unsigned char mpsse_ftdi_latency;


// ---------------------------------------------------------
// MPSSE / FTDI function implementations
// ---------------------------------------------------------

void mpsse_check_rx()
{
	while (1) {
		uint8_t data;
		int rc = ftdi_read_data(&mpsse_ftdic, &data, 1);
		if (rc <= 0)
			break;
		fprintf(stderr, "unexpected rx byte: %02X\n", data);
	}
}

void mpsse_error(int status)
{
	mpsse_check_rx();
	fprintf(stderr, "ABORT.\n");
	if (mpsse_ftdic_open) {
		if (mpsse_ftdic_latency_set)
			ftdi_set_latency_timer(&mpsse_ftdic, mpsse_ftdi_latency);
		ftdi_usb_close(&mpsse_ftdic);
	}
	ftdi_deinit(&mpsse_ftdic);
	exit(status);
}

uint8_t mpsse_recv_byte()
{
	uint8_t data;
	while (1) {
		int rc = ftdi_read_data(&mpsse_ftdic, &data, 1);
		if (rc < 0) {
			fprintf(stderr, "Read error.\n");
			mpsse_error(2);
		}
		if (rc == 1)
			break;
		usleep(100);
	}
	return data;
}

void mpsse_send_byte(uint8_t data)
{
	int rc = ftdi_write_data(&mpsse_ftdic, &data, 1);
	if (rc != 1) {
		fprintf(stderr, "Write error (single byte, rc=%d, expected %d).\n", rc, 1);
		mpsse_error(2);
	}
}

void mpsse_send_spi(uint8_t *data, int n)
{
	if (n < 1)
		return;

	/* Output only, update data on negative clock edge. */
	mpsse_send_byte(MC_DATA_OUT | MC_DATA_OCN);
	mpsse_send_byte(n - 1);
	mpsse_send_byte((n - 1) >> 8);

	int rc = ftdi_write_data(&mpsse_ftdic, data, n);
	if (rc != n) {
		fprintf(stderr, "Write error (chunk, rc=%d, expected %d).\n", rc, n);
		mpsse_error(2);
	}
}

void mpsse_xfer_spi(uint8_t *data, int n)
{
	if (n < 1)
		return;

	/* Input and output, update data on negative edge read on positive. */
	mpsse_send_byte(MC_DATA_IN | MC_DATA_OUT | MC_DATA_OCN);
	mpsse_send_byte(n - 1);
	mpsse_send_byte((n - 1) >> 8);

	int rc = ftdi_write_data(&mpsse_ftdic, data, n);
	if (rc != n) {
		fprintf(stderr, "Write error (chunk, rc=%d, expected %d).\n", rc, n);
		mpsse_error(2);
	}

	for (int i = 0; i < n; i++)
		data[i] = mpsse_recv_byte();
}

uint8_t mpsse_xfer_spi_bits(uint8_t data, int n)
{
	if (n < 1)
		return 0;

	/* Input and output, update data on negative edge read on positive, bits. */
	mpsse_send_byte(MC_DATA_IN | MC_DATA_OUT | MC_DATA_OCN | MC_DATA_BITS);
	mpsse_send_byte(n - 1);
	mpsse_send_byte(data);

	return mpsse_recv_byte();
}

void mpsse_set_gpio(uint8_t gpio, uint8_t direction)
{
	mpsse_send_byte(MC_SETB_LOW);
	mpsse_send_byte(gpio); /* Value */
	mpsse_send_byte(direction); /* Direction */
}

int mpsse_readb_low(void)
{
	uint8_t data;
	mpsse_send_byte(MC_READB_LOW);
	data = mpsse_recv_byte();
	return data;
}

int mpsse_readb_high(void)
{
	uint8_t data;
	mpsse_send_byte(MC_READB_HIGH);
	data = mpsse_recv_byte();
	return data;
}

void mpsse_send_dummy_bytes(uint8_t n)
{
	// add 8 x count dummy bits (aka n bytes)
	mpsse_send_byte(MC_CLK_N8);
	mpsse_send_byte(n - 1);
	mpsse_send_byte(0x00);

}

void mpsse_send_dummy_bit(void)
{
	// add 1  dummy bit
	mpsse_send_byte(MC_CLK_N);
	mpsse_send_byte(0x00);
}

void mpsse_jtag_init(){
	mpsse_send_byte(MC_SETB_LOW);
	mpsse_send_byte(0x08); /* Value */
	mpsse_send_byte(0x0B); /* Direction */

	/* Reset JTAG State machine */
	jtag_init();
}

void mpsse_jtag_tms(uint8_t bits, uint8_t pattern){
	mpsse_send_byte(MC_DATA_TMS | MC_DATA_LSB | MC_DATA_BITS);
	mpsse_send_byte(bits-1);
	mpsse_send_byte(pattern);
}

void mpsse_init(int ifnum, const char *devstr, bool slow_clock)
{
	enum ftdi_interface ftdi_ifnum = INTERFACE_A;

	switch (ifnum) {
		case 0:
			ftdi_ifnum = INTERFACE_A;
			break;
		case 1:
			ftdi_ifnum = INTERFACE_B;
			break;
		case 2:
			ftdi_ifnum = INTERFACE_C;
			break;
		case 3:
			ftdi_ifnum = INTERFACE_D;
			break;
		default:
			ftdi_ifnum = INTERFACE_A;
			break;
	}

	ftdi_init(&mpsse_ftdic);
	ftdi_set_interface(&mpsse_ftdic, ftdi_ifnum);

	if (devstr != NULL) {
		if (ftdi_usb_open_string(&mpsse_ftdic, devstr)) {
			fprintf(stderr, "Can't find iCE FTDI USB device (device string %s).\n", devstr);
			mpsse_error(2);
		}
	} else {
		if (ftdi_usb_open(&mpsse_ftdic, 0x0403, 0x6010) && ftdi_usb_open(&mpsse_ftdic, 0x0403, 0x6014)) {
			fprintf(stderr, "Can't find iCE FTDI USB device (vendor_id 0x0403, device_id 0x6010 or 0x6014).\n");
			mpsse_error(2);
		}
	}

	mpsse_ftdic_open = true;

	if (ftdi_usb_reset(&mpsse_ftdic)) {
		fprintf(stderr, "Failed to reset iCE FTDI USB device.\n");
		mpsse_error(2);
	}

	if (ftdi_usb_purge_buffers(&mpsse_ftdic)) {
		fprintf(stderr, "Failed to purge buffers on iCE FTDI USB device.\n");
		mpsse_error(2);
	}

	if (ftdi_get_latency_timer(&mpsse_ftdic, &mpsse_ftdi_latency) < 0) {
		fprintf(stderr, "Failed to get latency timer (%s).\n", ftdi_get_error_string(&mpsse_ftdic));
		mpsse_error(2);
	}

	/* 1 is the fastest polling, it means 1 kHz polling */
	if (ftdi_set_latency_timer(&mpsse_ftdic, 1) < 0) {
		fprintf(stderr, "Failed to set latency timer (%s).\n", ftdi_get_error_string(&mpsse_ftdic));
		mpsse_error(2);
	}

	mpsse_ftdic_latency_set = true;

	/* Enter MPSSE (Multi-Protocol Synchronous Serial Engine) mode. Set all pins to output. */
	if (ftdi_set_bitmode(&mpsse_ftdic, 0xff, BITMODE_MPSSE) < 0) {
		fprintf(stderr, "Failed to set BITMODE_MPSSE on iCE FTDI USB device.\n");
		mpsse_error(2);
	}

	// enable clock divide by 5
	mpsse_send_byte(MC_TCK_D5);

	if (slow_clock) {
		// set 50 kHz clock
		mpsse_send_byte(MC_SET_CLK_DIV);
		mpsse_send_byte(119);
		mpsse_send_byte(0x00);
	} else {
		// set 6 MHz clock
		mpsse_send_byte(MC_SET_CLK_DIV);
		mpsse_send_byte(0x00);
		mpsse_send_byte(0x00);
	}
}

void mpsse_close(void)
{
	ftdi_set_latency_timer(&mpsse_ftdic, mpsse_ftdi_latency);
	ftdi_disable_bitbang(&mpsse_ftdic);
	ftdi_usb_close(&mpsse_ftdic);
	ftdi_deinit(&mpsse_ftdic);
}