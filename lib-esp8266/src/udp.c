/**
 * @file udp.c
 *
 */
/* Copyright (C) 2016 by Arjan van Vught mailto:info@raspberrypi-dmx.nl
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <assert.h>
#include <stdint.h>
#include <stddef.h>

#include "esp8266.h"
#include "esp8266_cmd.h"

/**
 *
 * @param port
 */
void udp_begin(const uint16_t port) {
	esp8266_write_4bits((uint8_t)CMD_WIFI_UDP_BEGIN);

	esp8266_write_halfword(port);
}

/**
 *
 * @param buffer
 * @param length
 * @param ip_address
 * @param port
 * @return
 */
uint16_t udp_recvfrom(const uint8_t *buffer, const uint16_t length, uint32_t *ip_address, uint16_t *port) {
	uint16_t bytes_received;

	assert(buffer != NULL);
	assert(ip_address != NULL);
	assert(port != NULL);

	esp8266_write_4bits((uint8_t)CMD_WIFI_UDP_RECEIVE);

	bytes_received = esp8266_read_byte();
	bytes_received |= (esp8266_read_byte() << 8);

	if (bytes_received != 0) {

		if (ip_address != NULL) {
			*ip_address = esp8266_read_word();
		}

		if (port != NULL) {
			*port = esp8266_read_halfword();
		}

		esp8266_read_bytes(buffer, length);
	} else {
		if (ip_address != NULL) {
			*ip_address = 0;
		}
		if (port != NULL) {
			*port = 0;
		}
	}

	return bytes_received;
}

/**
 *
 * @param buffer
 * @param length
 * @param ip_address
 * @param port
 */
void udp_sendto(const uint8_t *buffer, const uint16_t length, const uint32_t ip_address, const uint16_t port) {
	assert(buffer != NULL);

	esp8266_write_4bits((uint8_t)CMD_WIFI_UDP_SEND);

	esp8266_write_halfword(length);
	esp8266_write_word(ip_address);
	esp8266_write_halfword(port);
	esp8266_write_bytes(buffer, length);
}
