/*
 * Copyright (C) 2012  Henrik Nordstrom <henrik@henriknordstrom.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common.h"
#include "portable_endian.h"
#include "progress.h"
#include "soc_info.h"

#include <libusb.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

static const uint16_t AW_USB_VENDOR_ID  = 0x1F3A;
static const uint16_t AW_USB_PRODUCT_ID = 0xEFE8;

/* a helper function to report libusb errors */
void usb_error(int rc, const char *caption, int exitcode)
{
	if (caption)
		fprintf(stderr, "%s ", caption);

#if defined(LIBUSBX_API_VERSION) && (LIBUSBX_API_VERSION >= 0x01000102)
	fprintf(stderr, "ERROR %d: %s\n", rc, libusb_strerror(rc));
#else
	/* assume that libusb_strerror() is missing in the libusb API */
	fprintf(stderr, "ERROR %d\n", rc);
#endif

	if (exitcode != 0)
		exit(exitcode);
}

struct  aw_usb_request {
	char signature[8];
	uint32_t length;
	uint32_t unknown1;	/* 0x0c000000 */
	uint16_t request;
	uint32_t length2;	/* Same as length */
	char	pad[10];
}  __attribute__((packed));

static const int AW_USB_READ = 0x11;
static const int AW_USB_WRITE = 0x12;

static int AW_USB_FEL_BULK_EP_OUT;
static int AW_USB_FEL_BULK_EP_IN;
static int timeout = 10000; /* 10 seconds */

static bool verbose = false; /* If set, makes the 'fel' tool more talkative */
static uint32_t uboot_entry = 0; /* entry point (address) of U-Boot */
static uint32_t uboot_size  = 0; /* size of U-Boot binary */

static void pr_info(const char *fmt, ...)
{
	va_list arglist;
	if (verbose) {
		va_start(arglist, fmt);
		vprintf(fmt, arglist);
		va_end(arglist);
	}
}

/*
 * AW_USB_MAX_BULK_SEND and the timeout constant are related.
 * Both need to be selected in a way that transferring the maximum chunk size
 * with (SoC-specific) slow transfer speed won't time out.
 *
 * The 512 KiB here are chosen based on the assumption that we want a 10 seconds
 * timeout, and "slow" transfers take place at approx. 64 KiB/sec - so we can
 * expect the maximum chunk being transmitted within 8 seconds or less.
 */
static const int AW_USB_MAX_BULK_SEND = 512 * 1024; /* 512 KiB per bulk request */

void usb_bulk_send(libusb_device_handle *usb, int ep, const void *data,
		   size_t length, bool progress)
{
	/*
	 * With no progress notifications, we'll use the maximum chunk size.
	 * Otherwise, it's useful to lower the size (have more chunks) to get
	 * more frequent status updates. 128 KiB per request seem suitable.
	 * (Worst case of "slow" transfers -> one update every two seconds.)
	 */
	size_t max_chunk = progress ? 128 * 1024 : AW_USB_MAX_BULK_SEND;

	size_t chunk;
	int rc, sent;
	while (length > 0) {
		chunk = length < max_chunk ? length : max_chunk;
		rc = libusb_bulk_transfer(usb, ep, (void *)data, chunk, &sent, timeout);
		if (rc != 0)
			usb_error(rc, "usb_bulk_send()", 2);
		length -= sent;
		data += sent;

		if (progress)
			progress_update(sent); /* notification after each chunk */
	}
}

void usb_bulk_recv(libusb_device_handle *usb, int ep, void *data, int length)
{
	int rc, recv;
	while (length > 0) {
		rc = libusb_bulk_transfer(usb, ep, data, length, &recv, timeout);
		if (rc != 0)
			usb_error(rc, "usb_bulk_recv()", 2);
		length -= recv;
		data += recv;
	}
}

/* Constants taken from ${U-BOOT}/include/image.h */
#define IH_MAGIC	0x27051956	/* Image Magic Number	*/
#define IH_ARCH_ARM		2	/* ARM			*/
#define IH_TYPE_INVALID		0	/* Invalid Image	*/
#define IH_TYPE_FIRMWARE	5	/* Firmware Image	*/
#define IH_TYPE_SCRIPT		6	/* Script file		*/
#define IH_NMLEN		32	/* Image Name Length	*/

/* Additional error codes, newly introduced for get_image_type() */
#define IH_TYPE_ARCH_MISMATCH	-1

#define HEADER_NAME_OFFSET	32	/* offset of name field	*/
#define HEADER_SIZE		(HEADER_NAME_OFFSET + IH_NMLEN)

/*
 * Utility function to determine the image type from a mkimage-compatible
 * header at given buffer (address).
 *
 * For invalid headers (insufficient size or 'magic' mismatch) the function
 * will return IH_TYPE_INVALID. Negative return values might indicate
 * special error conditions, e.g. IH_TYPE_ARCH_MISMATCH signals that the
 * image doesn't match the expected (ARM) architecture.
 * Otherwise the function will return the "ih_type" field for valid headers.
 */
int get_image_type(const uint8_t *buf, size_t len)
{
	uint32_t *buf32 = (uint32_t *)buf;

	if (len <= HEADER_SIZE) /* insufficient length/size */
		return IH_TYPE_INVALID;
	if (be32toh(buf32[0]) != IH_MAGIC) /* signature mismatch */
		return IH_TYPE_INVALID;
	/* For sunxi, we always expect ARM architecture here */
	if (buf[29] != IH_ARCH_ARM)
		return IH_TYPE_ARCH_MISMATCH;

	/* assume a valid header, and return ih_type */
	return buf[30];
}

void aw_send_usb_request(libusb_device_handle *usb, int type, int length)
{
	struct aw_usb_request req = {
		.signature = "AWUC",
		.request = htole16(type),
		.length = htole32(length),
		.unknown1 = htole32(0x0c000000)
	};
	req.length2 = req.length;
	usb_bulk_send(usb, AW_USB_FEL_BULK_EP_OUT, &req, sizeof(req), false);
}

void aw_read_usb_response(libusb_device_handle *usb)
{
	char buf[13];
	usb_bulk_recv(usb, AW_USB_FEL_BULK_EP_IN, &buf, sizeof(buf));
	assert(strcmp(buf, "AWUS") == 0);
}

void aw_usb_write(libusb_device_handle *usb, const void *data, size_t len,
		  bool progress)
{
	aw_send_usb_request(usb, AW_USB_WRITE, len);
	usb_bulk_send(usb, AW_USB_FEL_BULK_EP_OUT, data, len, progress);
	aw_read_usb_response(usb);
}

void aw_usb_read(libusb_device_handle *usb, const void *data, size_t len)
{
	aw_send_usb_request(usb, AW_USB_READ, len);
	usb_bulk_send(usb, AW_USB_FEL_BULK_EP_IN, data, len, false);
	aw_read_usb_response(usb);
}

struct aw_fel_request {
	uint32_t request;
	uint32_t address;
	uint32_t length;
	uint32_t pad;
};

static const int AW_FEL_VERSION = 0x001;
static const int AW_FEL_1_WRITE = 0x101;
static const int AW_FEL_1_EXEC  = 0x102;
static const int AW_FEL_1_READ  = 0x103;

void aw_send_fel_request(libusb_device_handle *usb, int type, uint32_t addr, uint32_t length)
{
	struct aw_fel_request req = {
		.request = htole32(type),
		.address = htole32(addr),
		.length = htole32(length)
	};
	aw_usb_write(usb, &req, sizeof(req), false);
}

void aw_read_fel_status(libusb_device_handle *usb)
{
	char buf[8];
	aw_usb_read(usb, &buf, sizeof(buf));
}

void aw_fel_get_version(libusb_device_handle *usb, struct aw_fel_version *buf)
{
	aw_send_fel_request(usb, AW_FEL_VERSION, 0, 0);
	aw_usb_read(usb, buf, sizeof(*buf));
	aw_read_fel_status(usb);

	buf->soc_id = (le32toh(buf->soc_id) >> 8) & 0xFFFF;
	buf->unknown_0a = le32toh(buf->unknown_0a);
	buf->protocol = le32toh(buf->protocol);
	buf->scratchpad = le16toh(buf->scratchpad);
	buf->pad[0] = le32toh(buf->pad[0]);
	buf->pad[1] = le32toh(buf->pad[1]);
}

void aw_fel_print_version(libusb_device_handle *usb)
{
	struct aw_fel_version buf;
	aw_fel_get_version(usb, &buf);

	const char *soc_name="unknown";
	switch (buf.soc_id) {
	case 0x1623: soc_name="A10"; break;
	case 0x1625: soc_name="A13"; break;
	case 0x1633: soc_name="A31"; break;
	case 0x1651: soc_name="A20"; break;
	case 0x1650: soc_name="A23"; break;
	case 0x1689: soc_name="A64"; break;
	case 0x1639: soc_name="A80"; break;
	case 0x1667: soc_name="A33"; break;
	case 0x1673: soc_name="A83T"; break;
	case 0x1680: soc_name="H3"; break;
	case 0x1718: soc_name="H5"; break;
	}

	printf("%.8s soc=%08x(%s) %08x ver=%04x %02x %02x scratchpad=%08x %08x %08x\n",
		buf.signature, buf.soc_id, soc_name, buf.unknown_0a,
		buf.protocol, buf.unknown_12, buf.unknown_13,
		buf.scratchpad, buf.pad[0], buf.pad[1]);
}

void aw_fel_read(libusb_device_handle *usb, uint32_t offset, void *buf, size_t len)
{
	aw_send_fel_request(usb, AW_FEL_1_READ, offset, len);
	aw_usb_read(usb, buf, len);
	aw_read_fel_status(usb);
}

void aw_fel_write(libusb_device_handle *usb, void *buf, uint32_t offset, size_t len)
{
	aw_send_fel_request(usb, AW_FEL_1_WRITE, offset, len);
	aw_usb_write(usb, buf, len, false);
	aw_read_fel_status(usb);
}

void aw_fel_execute(libusb_device_handle *usb, uint32_t offset)
{
	aw_send_fel_request(usb, AW_FEL_1_EXEC, offset, 0);
	aw_read_fel_status(usb);
}

/*
 * This function is a higher-level wrapper for the FEL write functionality.
 * Unlike aw_fel_write() above - which is reserved for internal use - this
 * routine is meant to be called from "user" code, and supports (= allows)
 * progress callbacks.
 * The return value represents elapsed time in seconds (needed for execution).
 */
double aw_write_buffer(libusb_device_handle *usb, void *buf, uint32_t offset,
		       size_t len, bool progress)
{
	/* safeguard against overwriting an already loaded U-Boot binary */
	if (uboot_size > 0 && offset <= uboot_entry + uboot_size
			   && offset + len >= uboot_entry)
	{
		fprintf(stderr, "ERROR: Attempt to overwrite U-Boot! "
			"Request 0x%08X-0x%08X overlaps 0x%08X-0x%08X.\n",
			offset, (uint32_t)(offset + len),
			uboot_entry, uboot_entry + uboot_size);
		exit(1);
	}
	double start = gettime();
	aw_send_fel_request(usb, AW_FEL_1_WRITE, offset, len);
	aw_usb_write(usb, buf, len, progress);
	aw_read_fel_status(usb);
	return gettime() - start;
}

void hexdump(void *data, uint32_t offset, size_t size)
{
	size_t j;
	unsigned char *buf = data;
	for (j = 0; j < size; j+=16) {
		size_t i;
		printf("%08zx: ", offset + j);
		for (i = 0; i < 16; i++) {
			if (j + i < size)
				printf("%02x ", buf[j+i]);
			else
				printf("__ ");
		}
		putchar(' ');
		for (i = 0; i < 16; i++) {
			if (j + i >= size)
				putchar('.');
			else
				putchar(isprint(buf[j+i]) ? buf[j+i] : '.');
		}
		putchar('\n');
	}
}

unsigned int file_size(const char *filename)
{
	struct stat st;
	if (stat(filename, &st) != 0) {
		fprintf(stderr, "stat() error on file \"%s\": %s\n", filename,
			strerror(errno));
		exit(1);
	}
	if (!S_ISREG(st.st_mode)) {
		fprintf(stderr, "error: \"%s\" is not a regular file\n", filename);
		exit(1);
	}
	return st.st_size;
}

int save_file(const char *name, void *data, size_t size)
{
	FILE *out = fopen(name, "wb");
	int rc;
	if (!out) {
		perror("Failed to open output file");
		exit(1);
	}
	rc = fwrite(data, size, 1, out);
	fclose(out);
	return rc;
}

void *load_file(const char *name, size_t *size)
{
	size_t bufsize = 8192;
	size_t offset = 0;
	char *buf = malloc(bufsize);
	FILE *in;
	if (strcmp(name, "-") == 0)
		in = stdin;
	else
		in = fopen(name, "rb");
	if (!in) {
		perror("Failed to open input file");
		exit(1);
	}
	
	while (true) {
		ssize_t len = bufsize - offset;
		ssize_t n = fread(buf+offset, 1, len, in);
		offset += n;
		if (n < len)
			break;
		bufsize <<= 1;
		buf = realloc(buf, bufsize);
	}
	if (size) 
		*size = offset;
	if (in != stdin)
		fclose(in);
	return buf;
}

void aw_fel_hexdump(libusb_device_handle *usb, uint32_t offset, size_t size)
{
	unsigned char buf[size];
	aw_fel_read(usb, offset, buf, size);
	hexdump(buf, offset, size);
}

void aw_fel_dump(libusb_device_handle *usb, uint32_t offset, size_t size)
{
	unsigned char buf[size];
	aw_fel_read(usb, offset, buf, size);
	fwrite(buf, size, 1, stdout);
}
void aw_fel_fill(libusb_device_handle *usb, uint32_t offset, size_t size, unsigned char value)
{
	unsigned char buf[size];
	memset(buf, value, size);
	aw_write_buffer(usb, buf, offset, size, false);
}

soc_info_t *aw_fel_get_soc_info(libusb_device_handle *usb)
{
	/* persistent SoC info, retrieves result pointer once and caches it */
	static soc_info_t *result = NULL;
	if (result == NULL) {
		struct aw_fel_version buf;
		aw_fel_get_version(usb, &buf);

		result = get_soc_info_from_version(&buf);
	}
	return result;
}

static uint32_t fel_to_spl_thunk[] = {
	#include "fel-to-spl-thunk.h"
};

#define	DRAM_BASE		0x40000000
#define	DRAM_SIZE		0x80000000

uint32_t aw_read_arm_cp_reg(libusb_device_handle *usb, soc_info_t *soc_info,
			    uint32_t coproc, uint32_t opc1, uint32_t crn,
			    uint32_t crm, uint32_t opc2)
{
	uint32_t val = 0;
	uint32_t opcode = 0xEE000000 | (1 << 20) | (1 << 4) |
			  ((opc1 & 7) << 21)    |
			  ((crn & 15) << 16)    |
			  ((coproc & 15) << 8)  |
			  ((opc2 & 7) << 5)     |
			  (crm & 15);
	uint32_t arm_code[] = {
		htole32(opcode),     /* mrc  coproc, opc1, r0, crn, crm, opc2 */
		htole32(0xe58f0000), /* str  r0, [pc]                         */
		htole32(0xe12fff1e), /* bx   lr                               */
	};
	aw_fel_write(usb, arm_code, soc_info->scratch_addr, sizeof(arm_code));
	aw_fel_execute(usb, soc_info->scratch_addr);
	aw_fel_read(usb, soc_info->scratch_addr + 12, &val, sizeof(val));
	return le32toh(val);
}

void aw_write_arm_cp_reg(libusb_device_handle *usb, soc_info_t *soc_info,
			 uint32_t coproc, uint32_t opc1, uint32_t crn,
			 uint32_t crm, uint32_t opc2, uint32_t val)
{
	uint32_t opcode = 0xEE000000 | (0 << 20) | (1 << 4) |
			  ((opc1 & 7) << 21)                |
			  ((crn & 15) << 16)                |
			  ((coproc & 15) << 8)              |
			  ((opc2 & 7) << 5)                 |
			  (crm & 15);
	uint32_t arm_code[] = {
		htole32(0xe59f000c), /* ldr  r0, [pc, #12]                    */
		htole32(opcode),     /* mcr  coproc, opc1, r0, crn, crm, opc2 */
		htole32(0xf57ff04f), /* dsb  sy                               */
		htole32(0xf57ff06f), /* isb  sy                               */
		htole32(0xe12fff1e), /* bx   lr                               */
		htole32(val)
	};
	aw_fel_write(usb, arm_code, soc_info->scratch_addr, sizeof(arm_code));
	aw_fel_execute(usb, soc_info->scratch_addr);
}

/*
 * We don't want the scratch code/buffer to exceed a maximum size of 0x400 bytes
 * (256 32-bit words) on readl_n/writel_n transfers. To guarantee this, we have
 * to account for the amount of space the ARM code uses.
 */
#define LCODE_ARM_WORDS  12 /* word count of the [read/write]l_n scratch code */
#define LCODE_ARM_SIZE   (LCODE_ARM_WORDS << 2) /* code size in bytes */
#define LCODE_MAX_TOTAL  0x100 /* max. words in buffer */
#define LCODE_MAX_WORDS  (LCODE_MAX_TOTAL - LCODE_ARM_WORDS) /* data words */

/* multiple "readl" from sequential addresses to a destination buffer */
void aw_fel_readl_n(libusb_device_handle *usb, uint32_t addr,
		    uint32_t *dst, size_t count)
{
	if (count == 0) return;
	if (count > LCODE_MAX_WORDS) {
		fprintf(stderr,
			"ERROR: Max. word count exceeded, truncating aw_fel_readl_n() transfer\n");
		count = LCODE_MAX_WORDS;
	}
	soc_info_t *soc_info = aw_fel_get_soc_info(usb);

	assert(LCODE_MAX_WORDS < 256); /* protect against corruption of ARM code */
	uint32_t arm_code[] = {
		htole32(0xe59f0020), /* ldr  r0, [pc, #32] ; ldr r0,[read_addr]  */
		htole32(0xe28f1024), /* add  r1, pc, #36   ; adr r1, read_data   */
		htole32(0xe59f201c), /* ldr  r2, [pc, #28] ; ldr r2,[read_count] */
		htole32(0xe3520000 + LCODE_MAX_WORDS), /* cmp	r2, #LCODE_MAX_WORDS */
		htole32(0xc3a02000 + LCODE_MAX_WORDS), /* movgt	r2, #LCODE_MAX_WORDS */
		/* read_loop: */
		htole32(0xe2522001), /* subs r2, r2, #1    ; r2 -= 1             */
		htole32(0x412fff1e), /* bxmi lr            ; return if (r2 < 0)  */
		htole32(0xe4903004), /* ldr  r3, [r0], #4  ; load and post-inc   */
		htole32(0xe4813004), /* str  r3, [r1], #4  ; store and post-inc  */
		htole32(0xeafffffa), /* b    read_loop                           */
		htole32(addr),       /* read_addr */
		htole32(count)       /* read_count */
		/* read_data (buffer) follows, i.e. values go here */
	};
	assert(sizeof(arm_code) == LCODE_ARM_SIZE);

	/* scratch buffer setup: transfers ARM code, including addr and count */
	aw_fel_write(usb, arm_code, soc_info->scratch_addr, sizeof(arm_code));
	/* execute code, read back the result */
	aw_fel_execute(usb, soc_info->scratch_addr);
	uint32_t buffer[count];
	aw_fel_read(usb, soc_info->scratch_addr + LCODE_ARM_SIZE,
		    buffer, sizeof(buffer));
	/* extract values to destination buffer */
	uint32_t *val = buffer;
	while (count-- > 0)
		*dst++ = le32toh(*val++);
}

/* "readl" of a single value */
uint32_t aw_fel_readl(libusb_device_handle *usb, uint32_t addr)
{
	uint32_t val;
	aw_fel_readl_n(usb, addr, &val, 1);
	return val;
}

/*
 * aw_fel_readl_n() wrapper that can handle large transfers. If necessary,
 * those will be done in separate 'chunks' of no more than LCODE_MAX_WORDS.
 */
void fel_readl_n(libusb_device_handle *usb, uint32_t addr,
		 uint32_t *dst, size_t count)
{
	while (count > 0) {
		size_t n = count > LCODE_MAX_WORDS ? LCODE_MAX_WORDS : count;
		aw_fel_readl_n(usb, addr, dst, n);
		addr += n * sizeof(uint32_t);
		dst += n;
		count -= n;
	}
}

/* multiple "writel" from a source buffer to sequential addresses */
void aw_fel_writel_n(libusb_device_handle *usb, uint32_t addr,
		     uint32_t *src, size_t count)
{
	if (count == 0) return;
	if (count > LCODE_MAX_WORDS) {
		fprintf(stderr,
			"ERROR: Max. word count exceeded, truncating aw_fel_writel_n() transfer\n");
		count = LCODE_MAX_WORDS;
	}
	soc_info_t *soc_info = aw_fel_get_soc_info(usb);

	assert(LCODE_MAX_WORDS < 256); /* protect against corruption of ARM code */
	/*
	 * We need a fixed array size to allow for (partial) initialization,
	 * so we'll claim the maximum total number of words (0x100) here.
	 */
	uint32_t arm_code[LCODE_MAX_TOTAL] = {
		htole32(0xe59f0020), /* ldr  r0, [pc, #32] ; ldr r0,[write_addr] */
		htole32(0xe28f1024), /* add  r1, pc, #36   ; adr r1, write_data  */
		htole32(0xe59f201c), /* ldr  r2, [pc, #28] ; ldr r2,[write_count]*/
		htole32(0xe3520000 + LCODE_MAX_WORDS), /* cmp	r2, #LCODE_MAX_WORDS */
		htole32(0xc3a02000 + LCODE_MAX_WORDS), /* movgt	r2, #LCODE_MAX_WORDS */
		/* write_loop: */
		htole32(0xe2522001), /* subs r2, r2, #1    ; r2 -= 1             */
		htole32(0x412fff1e), /* bxmi lr            ; return if (r2 < 0)  */
		htole32(0xe4913004), /* ldr  r3, [r1], #4  ; load and post-inc   */
		htole32(0xe4803004), /* str  r3, [r0], #4  ; store and post-inc  */
		htole32(0xeafffffa), /* b    write_loop                          */
		htole32(addr),       /* write_addr */
		htole32(count)       /* write_count */
		/* write_data (buffer) follows, i.e. values taken from here */
	};

	/* copy values from source buffer */
	size_t i;
	for (i = 0; i < count; i++)
		arm_code[LCODE_ARM_WORDS + i] = htole32(*src++);
	/* scratch buffer setup: transfers ARM code and data */
	aw_fel_write(usb, arm_code, soc_info->scratch_addr,
	             (LCODE_ARM_WORDS + count) * sizeof(uint32_t));
	/* execute, and we're done */
	aw_fel_execute(usb, soc_info->scratch_addr);
}

/* "writel" of a single value */
void aw_fel_writel(libusb_device_handle *usb, uint32_t addr, uint32_t val)
{
	aw_fel_writel_n(usb, addr, &val, 1);
}

/*
 * aw_fel_writel_n() wrapper that can handle large transfers. If necessary,
 * those will be done in separate 'chunks' of no more than LCODE_MAX_WORDS.
 */
void fel_writel_n(libusb_device_handle *usb, uint32_t addr,
		  uint32_t *src, size_t count)
{
	while (count > 0) {
		size_t n = count > LCODE_MAX_WORDS ? LCODE_MAX_WORDS : count;
		aw_fel_writel_n(usb, addr, src, n);
		addr += n * sizeof(uint32_t);
		src += n;
		count -= n;
	}
}

void aw_fel_print_sid(libusb_device_handle *usb)
{
	soc_info_t *soc_info = aw_fel_get_soc_info(usb);
	if (soc_info->sid_addr) {
		pr_info("SID key (e-fuses) at 0x%08X\n", soc_info->sid_addr);

		uint32_t key[4];
		aw_fel_readl_n(usb, soc_info->sid_addr, key, 4);

		unsigned int i;
		/* output SID in "xxxxxxxx:xxxxxxxx:xxxxxxxx:xxxxxxxx" format */
		for (i = 0; i <= 3; i++)
			printf("%08x%c", key[i], i < 3 ? ':' : '\n');
	} else {
		printf("SID registers for your SoC (id=%04X) are unknown or inaccessible.\n",
			soc_info->soc_id);
	}
}

void aw_enable_l2_cache(libusb_device_handle *usb, soc_info_t *soc_info)
{
	uint32_t arm_code[] = {
		htole32(0xee112f30), /* mrc        15, 0, r2, cr1, cr0, {1}  */
		htole32(0xe3822002), /* orr        r2, r2, #2                */
		htole32(0xee012f30), /* mcr        15, 0, r2, cr1, cr0, {1}  */
		htole32(0xe12fff1e), /* bx         lr                        */
	};

	aw_fel_write(usb, arm_code, soc_info->scratch_addr, sizeof(arm_code));
	aw_fel_execute(usb, soc_info->scratch_addr);
}

void aw_get_stackinfo(libusb_device_handle *usb, soc_info_t *soc_info,
                      uint32_t *sp_irq, uint32_t *sp)
{
	uint32_t results[2] = { 0 };
#if 0
	/* Does not work on Cortex-A8 (needs Virtualization Extensions) */
	uint32_t arm_code[] = {
		htole32(0xe1010300), /* mrs        r0, SP_irq                */
		htole32(0xe58f0004), /* str        r0, [pc, #4]              */
		htole32(0xe58fd004), /* str        sp, [pc, #4]              */
		htole32(0xe12fff1e), /* bx         lr                        */
	};

	aw_fel_write(usb, arm_code, soc_info->scratch_addr, sizeof(arm_code));
	aw_fel_execute(usb, soc_info->scratch_addr);
	aw_fel_read(usb, soc_info->scratch_addr + 0x10, results, 8);
#else
	/* Works everywhere */
	uint32_t arm_code[] = {
		htole32(0xe10f0000), /* mrs        r0, CPSR                  */
		htole32(0xe3c0101f), /* bic        r1, r0, #31               */
		htole32(0xe3811012), /* orr        r1, r1, #18               */
		htole32(0xe121f001), /* msr        CPSR_c, r1                */
		htole32(0xe1a0100d), /* mov        r1, sp                    */
		htole32(0xe121f000), /* msr        CPSR_c, r0                */
		htole32(0xe58f1004), /* str        r1, [pc, #4]              */
		htole32(0xe58fd004), /* str        sp, [pc, #4]              */
		htole32(0xe12fff1e), /* bx         lr                        */
	};

	aw_fel_write(usb, arm_code, soc_info->scratch_addr, sizeof(arm_code));
	aw_fel_execute(usb, soc_info->scratch_addr);
	aw_fel_read(usb, soc_info->scratch_addr + 0x24, results, 8);
#endif
	*sp_irq = le32toh(results[0]);
	*sp     = le32toh(results[1]);
}

uint32_t aw_get_ttbr0(libusb_device_handle *usb, soc_info_t *soc_info)
{
	return aw_read_arm_cp_reg(usb, soc_info, 15, 0, 2, 0, 0);
}

uint32_t aw_get_ttbcr(libusb_device_handle *usb, soc_info_t *soc_info)
{
	return aw_read_arm_cp_reg(usb, soc_info, 15, 0, 2, 0, 2);
}

uint32_t aw_get_dacr(libusb_device_handle *usb, soc_info_t *soc_info)
{
	return aw_read_arm_cp_reg(usb, soc_info, 15, 0, 3, 0, 0);
}

uint32_t aw_get_sctlr(libusb_device_handle *usb, soc_info_t *soc_info)
{
	return aw_read_arm_cp_reg(usb, soc_info, 15, 0, 1, 0, 0);
}

void aw_set_ttbr0(libusb_device_handle *usb, soc_info_t *soc_info,
		  uint32_t ttbr0)
{
	return aw_write_arm_cp_reg(usb, soc_info, 15, 0, 2, 0, 0, ttbr0);
}

void aw_set_ttbcr(libusb_device_handle *usb, soc_info_t *soc_info,
		  uint32_t ttbcr)
{
	return aw_write_arm_cp_reg(usb, soc_info, 15, 0, 2, 0, 2, ttbcr);
}

void aw_set_dacr(libusb_device_handle *usb, soc_info_t *soc_info,
		 uint32_t dacr)
{
	aw_write_arm_cp_reg(usb, soc_info, 15, 0, 3, 0, 0, dacr);
}

void aw_set_sctlr(libusb_device_handle *usb, soc_info_t *soc_info,
		  uint32_t sctlr)
{
	aw_write_arm_cp_reg(usb, soc_info, 15, 0, 1, 0, 0, sctlr);
}

/*
 * Reconstruct the same MMU translation table as used by the A20 BROM.
 * We are basically reverting the changes, introduced in newer SoC
 * variants. This works fine for the SoC variants with the memory
 * layout similar to A20 (the SRAM is in the first megabyte of the
 * address space and the BROM is in the last megabyte of the address
 * space).
 */
uint32_t *aw_generate_mmu_translation_table(void)
{
	uint32_t *tt = malloc(4096 * sizeof(uint32_t));
	uint32_t i;

	/*
	 * Direct mapping using 1MB sections with TEXCB=00000 (Strongly
	 * ordered) for all memory except the first and the last sections,
	 * which have TEXCB=00100 (Normal). Domain bits are set to 1111
	 * and AP bits are set to 11, but this is mostly irrelevant.
	 */
	for (i = 0; i < 4096; i++)
		tt[i] = 0x00000DE2 | (i << 20);
	tt[0x000] |= 0x1000;
	tt[0xFFF] |= 0x1000;

	return tt;
}

uint32_t *aw_backup_and_disable_mmu(libusb_device_handle *usb,
                                    soc_info_t *soc_info)
{
	uint32_t *tt = NULL;
	uint32_t sctlr, ttbr0, ttbcr, dacr;
	uint32_t i;

	uint32_t arm_code[] = {
		/* Disable I-cache, MMU and branch prediction */
		htole32(0xee110f10), /* mrc        15, 0, r0, cr1, cr0, {0}  */
		htole32(0xe3c00001), /* bic        r0, r0, #1                */
		htole32(0xe3c00a01), /* bic        r0, r0, #4096             */
		htole32(0xe3c00b02), /* bic        r0, r0, #2048             */
		htole32(0xee010f10), /* mcr        15, 0, r0, cr1, cr0, {0}  */
		/* Return back to FEL */
		htole32(0xe12fff1e), /* bx         lr                        */
	};

	/*
	 * Below are some checks for the register values, which are known
	 * to be initialized in this particular way by the existing BROM
	 * implementations. We don't strictly need them to exactly match,
	 * but still have these safety guards in place in order to detect
	 * and review any potential configuration changes in future SoC
	 * variants (if one of these checks fails, then it is not a serious
	 * problem but more likely just an indication that one of these
	 * checks needs to be relaxed).
	 */

	/* Basically, ignore M/Z/I/V/UNK bits and expect no TEX remap */
	sctlr = aw_get_sctlr(usb, soc_info);
	if ((sctlr & ~((0x7 << 11) | (1 << 6) | 1)) != 0x00C50038) {
		fprintf(stderr, "Unexpected SCTLR (%08X)\n", sctlr);
		exit(1);
	}

	if (!(sctlr & 1)) {
		pr_info("MMU is not enabled by BROM\n");
		return NULL;
	}

	dacr = aw_get_dacr(usb, soc_info);
	if (dacr != 0x55555555) {
		fprintf(stderr, "Unexpected DACR (%08X)\n", dacr);
		exit(1);
	}

	ttbcr = aw_get_ttbcr(usb, soc_info);
	if (ttbcr != 0x00000000) {
		fprintf(stderr, "Unexpected TTBCR (%08X)\n", ttbcr);
		exit(1);
	}

	ttbr0 = aw_get_ttbr0(usb, soc_info);
	if (ttbr0 & 0x3FFF) {
		fprintf(stderr, "Unexpected TTBR0 (%08X)\n", ttbr0);
		exit(1);
	}

	tt = malloc(16 * 1024);
	pr_info("Reading the MMU translation table from 0x%08X\n", ttbr0);
	aw_fel_read(usb, ttbr0, tt, 16 * 1024);
	for (i = 0; i < 4096; i++)
		tt[i] = le32toh(tt[i]);

	/* Basic sanity checks to be sure that this is a valid table */
	for (i = 0; i < 4096; i++) {
		if (((tt[i] >> 1) & 1) != 1 || ((tt[i] >> 18) & 1) != 0) {
			fprintf(stderr, "MMU: not a section descriptor\n");
			exit(1);
		}
		if ((tt[i] >> 20) != i) {
			fprintf(stderr, "MMU: not a direct mapping\n");
			exit(1);
		}
	}

	pr_info("Disabling I-cache, MMU and branch prediction...");
	aw_fel_write(usb, arm_code, soc_info->scratch_addr, sizeof(arm_code));
	aw_fel_execute(usb, soc_info->scratch_addr);
	pr_info(" done.\n");

	return tt;
}

void aw_restore_and_enable_mmu(libusb_device_handle *usb,
                               soc_info_t *soc_info,
                               uint32_t *tt)
{
	uint32_t i;
	uint32_t ttbr0 = aw_get_ttbr0(usb, soc_info);

	uint32_t arm_code[] = {
		/* Invalidate I-cache, TLB and BTB */
		htole32(0xe3a00000), /* mov        r0, #0                    */
		htole32(0xee080f17), /* mcr        15, 0, r0, cr8, cr7, {0}  */
		htole32(0xee070f15), /* mcr        15, 0, r0, cr7, cr5, {0}  */
		htole32(0xee070fd5), /* mcr        15, 0, r0, cr7, cr5, {6}  */
		htole32(0xf57ff04f), /* dsb        sy                        */
		htole32(0xf57ff06f), /* isb        sy                        */
		/* Enable I-cache, MMU and branch prediction */
		htole32(0xee110f10), /* mrc        15, 0, r0, cr1, cr0, {0}  */
		htole32(0xe3800001), /* orr        r0, r0, #1                */
		htole32(0xe3800a01), /* orr        r0, r0, #4096             */
		htole32(0xe3800b02), /* orr        r0, r0, #2048             */
		htole32(0xee010f10), /* mcr        15, 0, r0, cr1, cr0, {0}  */
		/* Return back to FEL */
		htole32(0xe12fff1e), /* bx         lr                        */
	};

	pr_info("Setting write-combine mapping for DRAM.\n");
	for (i = (DRAM_BASE >> 20); i < ((DRAM_BASE + DRAM_SIZE) >> 20); i++) {
		/* Clear TEXCB bits */
		tt[i] &= ~((7 << 12) | (1 << 3) | (1 << 2));
		/* Set TEXCB to 00100 (Normal uncached mapping) */
		tt[i] |= (1 << 12);
	}

	pr_info("Setting cached mapping for BROM.\n");
	/* Clear TEXCB bits first */
	tt[0xFFF] &= ~((7 << 12) | (1 << 3) | (1 << 2));
	/* Set TEXCB to 00111 (Normal write-back cached mapping) */
	tt[0xFFF] |= (1 << 12) | /* TEX */
		     (1 << 3)  | /* C */
		     (1 << 2);   /* B */

	pr_info("Writing back the MMU translation table.\n");
	for (i = 0; i < 4096; i++)
		tt[i] = htole32(tt[i]);
	aw_fel_write(usb, tt, ttbr0, 16 * 1024);

	pr_info("Enabling I-cache, MMU and branch prediction...");
	aw_fel_write(usb, arm_code, soc_info->scratch_addr, sizeof(arm_code));
	aw_fel_execute(usb, soc_info->scratch_addr);
	pr_info(" done.\n");

	free(tt);
}

/*
 * Maximum size of SPL, at the same time this is the start offset
 * of the main U-Boot image within u-boot-sunxi-with-spl.bin
 */
#define SPL_LEN_LIMIT 0x8000

void aw_fel_write_and_execute_spl(libusb_device_handle *usb,
				  uint8_t *buf, size_t len)
{
	soc_info_t *soc_info = aw_fel_get_soc_info(usb);
	sram_swap_buffers *swap_buffers;
	char header_signature[9] = { 0 };
	size_t i, thunk_size;
	uint32_t *thunk_buf;
	uint32_t sp, sp_irq;
	uint32_t spl_checksum, spl_len, spl_len_limit = SPL_LEN_LIMIT;
	uint32_t *buf32 = (uint32_t *)buf;
	uint32_t cur_addr = soc_info->spl_addr;
	uint32_t *tt = NULL;

	if (!soc_info || !soc_info->swap_buffers) {
		fprintf(stderr, "SPL: Unsupported SoC type\n");
		exit(1);
	}

	if (len < 32 || memcmp(buf + 4, "eGON.BT0", 8) != 0) {
		fprintf(stderr, "SPL: eGON header is not found\n");
		exit(1);
	}

	spl_checksum = 2 * le32toh(buf32[3]) - 0x5F0A6C39;
	spl_len = le32toh(buf32[4]);

	if (spl_len > len || (spl_len % 4) != 0) {
		fprintf(stderr, "SPL: bad length in the eGON header\n");
		exit(1);
	}

	len = spl_len;
	for (i = 0; i < len / 4; i++)
		spl_checksum -= le32toh(buf32[i]);

	if (spl_checksum != 0) {
		fprintf(stderr, "SPL: checksum check failed\n");
		exit(1);
	}

	if (soc_info->needs_l2en) {
		pr_info("Enabling the L2 cache\n");
		aw_enable_l2_cache(usb, soc_info);
	}

	aw_get_stackinfo(usb, soc_info, &sp_irq, &sp);
	pr_info("Stack pointers: sp_irq=0x%08X, sp=0x%08X\n", sp_irq, sp);

	tt = aw_backup_and_disable_mmu(usb, soc_info);
	if (!tt && soc_info->mmu_tt_addr) {
		if (soc_info->mmu_tt_addr & 0x3FFF) {
			fprintf(stderr, "SPL: 'mmu_tt_addr' must be 16K aligned\n");
			exit(1);
		}
		pr_info("Generating the new MMU translation table at 0x%08X\n",
		        soc_info->mmu_tt_addr);
		/*
		 * These settings are used by the BROM in A10/A13/A20 and
		 * we replicate them here when enabling the MMU. The DACR
		 * value 0x55555555 means that accesses are checked against
		 * the permission bits in the translation tables for all
		 * domains. The TTBCR value 0x00000000 means that the short
		 * descriptor translation table format is used, TTBR0 is used
		 * for all the possible virtual addresses (N=0) and that the
		 * translation table must be aligned at a 16K boundary.
		 */
		aw_set_dacr(usb, soc_info, 0x55555555);
		aw_set_ttbcr(usb, soc_info, 0x00000000);
		aw_set_ttbr0(usb, soc_info, soc_info->mmu_tt_addr);
		tt = aw_generate_mmu_translation_table();
	}

	swap_buffers = soc_info->swap_buffers;
	for (i = 0; swap_buffers[i].size; i++) {
		if ((swap_buffers[i].buf2 >= soc_info->spl_addr) &&
		    (swap_buffers[i].buf2 < soc_info->spl_addr + spl_len_limit))
			spl_len_limit = swap_buffers[i].buf2 - soc_info->spl_addr;
		if (len > 0 && cur_addr < swap_buffers[i].buf1) {
			uint32_t tmp = swap_buffers[i].buf1 - cur_addr;
			if (tmp > len)
				tmp = len;
			aw_fel_write(usb, buf, cur_addr, tmp);
			cur_addr += tmp;
			buf += tmp;
			len -= tmp;
		}
		if (len > 0 && cur_addr == swap_buffers[i].buf1) {
			uint32_t tmp = swap_buffers[i].size;
			if (tmp > len)
				tmp = len;
			aw_fel_write(usb, buf, swap_buffers[i].buf2, tmp);
			cur_addr += tmp;
			buf += tmp;
			len -= tmp;
		}
	}

	/* Clarify the SPL size limitations, and bail out if they are not met */
	if (soc_info->thunk_addr < spl_len_limit)
		spl_len_limit = soc_info->thunk_addr;

	if (spl_len > spl_len_limit) {
		fprintf(stderr, "SPL: too large (need %d, have %d)\n",
			(int)spl_len, (int)spl_len_limit);
		exit(1);
	}

	/* Write the remaining part of the SPL */
	if (len > 0)
		aw_fel_write(usb, buf, cur_addr, len);

	thunk_size = sizeof(fel_to_spl_thunk) + sizeof(soc_info->spl_addr) +
		     (i + 1) * sizeof(*swap_buffers);

	if (thunk_size > soc_info->thunk_size) {
		fprintf(stderr, "SPL: bad thunk size (need %d, have %d)\n",
			(int)sizeof(fel_to_spl_thunk), soc_info->thunk_size);
		exit(1);
	}

	thunk_buf = malloc(thunk_size);
	memcpy(thunk_buf, fel_to_spl_thunk, sizeof(fel_to_spl_thunk));
	memcpy(thunk_buf + sizeof(fel_to_spl_thunk) / sizeof(uint32_t),
	       &soc_info->spl_addr, sizeof(soc_info->spl_addr));
	memcpy(thunk_buf + sizeof(fel_to_spl_thunk) / sizeof(uint32_t) + 1,
	       swap_buffers, (i + 1) * sizeof(*swap_buffers));

	for (i = 0; i < thunk_size / sizeof(uint32_t); i++)
		thunk_buf[i] = htole32(thunk_buf[i]);

	pr_info("=> Executing the SPL...");
	aw_fel_write(usb, thunk_buf, soc_info->thunk_addr, thunk_size);
	aw_fel_execute(usb, soc_info->thunk_addr);
	pr_info(" done.\n");

	free(thunk_buf);

	/* TODO: Try to find and fix the bug, which needs this workaround */
	usleep(250000);

	/* Read back the result and check if everything was fine */
	aw_fel_read(usb, soc_info->spl_addr + 4, header_signature, 8);
	if (strcmp(header_signature, "eGON.FEL") != 0) {
		fprintf(stderr, "SPL: failure code '%s'\n",
			header_signature);
		exit(1);
	}

	/* re-enable the MMU if it was enabled by BROM */
	if (tt != NULL)
		aw_restore_and_enable_mmu(usb, soc_info, tt);
}

/*
 * This function tests a given buffer address and length for a valid U-Boot
 * image. Upon success, the image data gets transferred to the default memory
 * address stored within the image header; and the function preserves the
 * U-Boot entry point (offset) and size values.
 */
void aw_fel_write_uboot_image(libusb_device_handle *usb,
		uint8_t *buf, size_t len)
{
	if (len <= HEADER_SIZE)
		return; /* Insufficient size (no actual data), just bail out */

	uint32_t *buf32 = (uint32_t *)buf;

	/* Check for a valid mkimage header */
	int image_type = get_image_type(buf, len);
	if (image_type <= IH_TYPE_INVALID) {
		switch (image_type) {
		case IH_TYPE_INVALID:
			fprintf(stderr, "Invalid U-Boot image: bad size or signature\n");
			break;
		case IH_TYPE_ARCH_MISMATCH:
			fprintf(stderr, "Invalid U-Boot image: wrong architecture\n");
			break;
		default:
			fprintf(stderr, "Invalid U-Boot image: error code %d\n",
				image_type);
		}
		exit(1);
	}
	if (image_type != IH_TYPE_FIRMWARE) {
		fprintf(stderr, "U-Boot image type mismatch: "
			"expected IH_TYPE_FIRMWARE, got %02X\n", image_type);
		exit(1);
	}
	uint32_t data_size = be32toh(buf32[3]); /* Image Data Size */
	uint32_t load_addr = be32toh(buf32[4]); /* Data Load Address */
	if (data_size != len - HEADER_SIZE) {
		fprintf(stderr, "U-Boot image data size mismatch: "
			"expected %zu, got %u\n", len - HEADER_SIZE, data_size);
		exit(1);
	}
	/* TODO: Verify image data integrity using the checksum field ih_dcrc,
	 * available from be32toh(buf32[6])
	 *
	 * However, this requires CRC routines that mimic their U-Boot
	 * counterparts, namely image_check_dcrc() in ${U-BOOT}/common/image.c
	 * and crc_wd() in ${U-BOOT}/lib/crc32.c
	 *
	 * It should be investigated if existing CRC routines in sunxi-tools
	 * could be factored out and reused for this purpose - e.g. calc_crc32()
	 * from nand-part-main.c
	 */

	/* If we get here, we're "good to go" (i.e. actually write the data) */
	pr_info("Writing image \"%.*s\", %u bytes @ 0x%08X.\n",
		IH_NMLEN, buf + HEADER_NAME_OFFSET, data_size, load_addr);

	aw_write_buffer(usb, buf + HEADER_SIZE, load_addr, data_size, false);

	/* keep track of U-Boot memory region in global vars */
	uboot_entry = load_addr;
	uboot_size = data_size;
}

/*
 * This function handles the common part of both "spl" and "uboot" commands.
 */
void aw_fel_process_spl_and_uboot(libusb_device_handle *usb,
		const char *filename)
{
	/* load file into memory buffer */
	size_t size;
	uint8_t *buf = load_file(filename, &size);
	/* write and execute the SPL from the buffer */
	aw_fel_write_and_execute_spl(usb, buf, size);
	/* check for optional main U-Boot binary (and transfer it, if applicable) */
	if (size > SPL_LEN_LIMIT)
		aw_fel_write_uboot_image(usb, buf + SPL_LEN_LIMIT, size - SPL_LEN_LIMIT);
	free(buf);
}

/*
 * Test the SPL header for our "sunxi" variant. We want to make sure that
 * we can safely use specific header fields to pass information to U-Boot.
 * In case of a missing signature (e.g. Allwinner boot0) or header version
 * mismatch, this function will return "false". If all seems fine,
 * the result is "true".
 */
#define SPL_SIGNATURE			"SPL" /* marks "sunxi" header */
#define SPL_MIN_VERSION			1 /* minimum required version */
#define SPL_MAX_VERSION			1 /* maximum supported version */
bool have_sunxi_spl(libusb_device_handle *usb, uint32_t spl_addr)
{
	uint8_t spl_signature[4];

	aw_fel_read(usb, spl_addr + 0x14,
		&spl_signature, sizeof(spl_signature));

	if (memcmp(spl_signature, SPL_SIGNATURE, 3) != 0)
		return false; /* signature mismatch, no "sunxi" SPL */

	if (spl_signature[3] < SPL_MIN_VERSION) {
		fprintf(stderr, "sunxi SPL version mismatch: "
			"found 0x%02X < required minimum 0x%02X\n",
			spl_signature[3], SPL_MIN_VERSION);
		fprintf(stderr, "You need to update your U-Boot (mksunxiboot) to a more recent version.\n");
		return false;
	}
	if (spl_signature[3] > SPL_MAX_VERSION) {
		fprintf(stderr, "sunxi SPL version mismatch: "
			"found 0x%02X > maximum supported 0x%02X\n",
			spl_signature[3], SPL_MAX_VERSION);
		fprintf(stderr, "You need a more recent version of this (sunxi-tools) fel utility.\n");
		return false;
	}
	return true; /* sunxi SPL and suitable version */
}

/*
 * Pass information to U-Boot via specialized fields in the SPL header
 * (see "boot_file_head" in ${U-BOOT}/arch/arm/include/asm/arch-sunxi/spl.h),
 * providing the boot script address (DRAM location of boot.scr).
 */
void pass_fel_information(libusb_device_handle *usb,
			  uint32_t script_address, uint32_t uEnv_length)
{
	soc_info_t *soc_info = aw_fel_get_soc_info(usb);

	/* write something _only_ if we have a suitable SPL header */
	if (have_sunxi_spl(usb, soc_info->spl_addr)) {
		pr_info("Passing boot info via sunxi SPL: "
			"script address = 0x%08X, uEnv length = %u\n",
			script_address, uEnv_length);
		uint32_t transfer[] = {
			htole32(script_address),
			htole32(uEnv_length)
		};
		aw_fel_write(usb, transfer,
			soc_info->spl_addr + 0x18, sizeof(transfer));
	}
}

static int aw_fel_get_endpoint(libusb_device_handle *usb)
{
	struct libusb_device *dev = libusb_get_device(usb);
	struct libusb_config_descriptor *config;
	int if_idx, set_idx, ep_idx, ret;

	ret = libusb_get_active_config_descriptor(dev, &config);
	if (ret)
		return ret;

	for (if_idx = 0; if_idx < config->bNumInterfaces; if_idx++) {
		const struct libusb_interface *iface = config->interface + if_idx;

		for (set_idx = 0; set_idx < iface->num_altsetting; set_idx++) {
			const struct libusb_interface_descriptor *setting =
				iface->altsetting + set_idx;

			for (ep_idx = 0; ep_idx < setting->bNumEndpoints; ep_idx++) {
				const struct libusb_endpoint_descriptor *ep =
					setting->endpoint + ep_idx;

				/* Test for bulk transfer endpoint */
				if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) !=
						LIBUSB_TRANSFER_TYPE_BULK)
					continue;

				if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) ==
						LIBUSB_ENDPOINT_IN)
					AW_USB_FEL_BULK_EP_IN = ep->bEndpointAddress;
				else
					AW_USB_FEL_BULK_EP_OUT = ep->bEndpointAddress;
			}
		}
	}

	libusb_free_config_descriptor(config);

	return 0;
}

/*
 * This function stores a given entry point to the RVBAR address for CPU0,
 * and then writes the Reset Management Register to request a warm boot.
 * It is useful with some AArch64 transitions, e.g. when passing control to
 * ARM Trusted Firmware (ATF) during the boot process of Pine64.
 *
 * The code was inspired by
 * https://github.com/apritzel/u-boot/commit/fda6bd1bf285c44f30ea15c7e6231bf53c31d4a8
 */
void aw_rmr_request(libusb_device_handle *usb, uint32_t entry_point, bool aarch64)
{
	soc_info_t *soc_info = aw_fel_get_soc_info(usb);
	if (!soc_info->rvbar_reg) {
		fprintf(stderr, "ERROR: Can't issue RMR request!\n"
			"RVBAR is not supported or unknown for your SoC (id=%04X).\n",
			soc_info->soc_id);
		return;
	}

	uint32_t rmr_mode = (1 << 1) | (aarch64 ? 1 : 0); /* RR, AA64 flag */
	uint32_t arm_code[] = {
		htole32(0xe59f0028), /* ldr        r0, [rvbar_reg]          */
		htole32(0xe59f1028), /* ldr        r1, [entry_point]        */
		htole32(0xe5801000), /* str        r1, [r0]                 */
		htole32(0xf57ff04f), /* dsb        sy                       */
		htole32(0xf57ff06f), /* isb        sy                       */

		htole32(0xe59f101c), /* ldr        r1, [rmr_mode]           */
		htole32(0xee1c0f50), /* mrc        15, 0, r0, cr12, cr0, {2}*/
		htole32(0xe1800001), /* orr        r0, r0, r1               */
		htole32(0xee0c0f50), /* mcr        15, 0, r0, cr12, cr0, {2}*/
		htole32(0xf57ff06f), /* isb        sy                       */

		htole32(0xe320f003), /* loop:      wfi                      */
		htole32(0xeafffffd), /* b          <loop>                   */

		htole32(soc_info->rvbar_reg),
		htole32(entry_point),
		htole32(rmr_mode)
	};
	/* scratch buffer setup: transfers ARM code and parameter values */
	aw_fel_write(usb, arm_code, soc_info->scratch_addr, sizeof(arm_code));
	/* execute the thunk code (triggering a warm reset on the SoC) */
	pr_info("Store entry point 0x%08X to RVBAR 0x%08X, "
		"and request warm reset with RMR mode %u...",
		entry_point, soc_info->rvbar_reg, rmr_mode);
	aw_fel_execute(usb, soc_info->scratch_addr);
	pr_info(" done.\n");
}

/* check buffer for magic "#=uEnv", indicating uEnv.txt compatible format */
static bool is_uEnv(void *buffer, size_t size)
{
	if (size <= 6)
		return false; /* insufficient size */
	return memcmp(buffer, "#=uEnv", 6) == 0;
}

/* private helper function, gets used for "write*" and "multi*" transfers */
static unsigned int file_upload(libusb_device_handle *handle, size_t count,
				size_t argc, char **argv, progress_cb_t callback)
{
	if (argc < count * 2) {
		fprintf(stderr, "error: too few arguments for uploading %zu files\n",
			count);
		exit(1);
	}

	/* get all file sizes, keeping track of total bytes */
	size_t size = 0;
	unsigned int i;
	for (i = 0; i < count; i++)
		size += file_size(argv[i * 2 + 1]);

	progress_start(callback, size); /* set total size and progress callback */

	/* now transfer each file in turn */
	for (i = 0; i < count; i++) {
		void *buf = load_file(argv[i * 2 + 1], &size);
		if (size > 0) {
			uint32_t offset = strtoul(argv[i * 2], NULL, 0);
			aw_write_buffer(handle, buf, offset, size, callback != NULL);

			/* If we transferred a script, try to inform U-Boot about its address. */
			if (get_image_type(buf, size) == IH_TYPE_SCRIPT)
				pass_fel_information(handle, offset, 0);
			if (is_uEnv(buf, size)) /* uEnv-style data */
				pass_fel_information(handle, offset, size);
		}
		free(buf);
	}

	return i; /* return number of files that were processed */
}

/* open libusb handle to desired FEL device */
static libusb_device_handle *open_fel_device(int busnum, int devnum,
		uint16_t vendor_id, uint16_t product_id)
{
	libusb_device_handle *result = NULL;

	if (busnum < 0 || devnum < 0) {
		/* With the default values (busnum -1, devnum -1) we don't care
		 * for a specific USB device; so let libusb open the first
		 * device that matches VID/PID.
		 */
		result = libusb_open_device_with_vid_pid(NULL, vendor_id, product_id);
		if (!result) {
			switch (errno) {
			case EACCES:
				fprintf(stderr, "ERROR: You don't have permission to access Allwinner USB FEL device\n");
				break;
			default:
				fprintf(stderr, "ERROR: Allwinner USB FEL device not found!\n");
				break;
			}
			exit(1);
		}
		return result;
	}

	/* look for specific bus and device number */
	pr_info("Selecting USB Bus %03d Device %03d\n", busnum, devnum);
	bool found = false;
	ssize_t rc, i;
	libusb_device **list;

	rc = libusb_get_device_list(NULL, &list);
	if (rc < 0)
		usb_error(rc, "libusb_get_device_list()", 1);
	for (i = 0; i < rc; i++) {
		if (libusb_get_bus_number(list[i]) == busnum
		    && libusb_get_device_address(list[i]) == devnum) {
			found = true; /* bus:devnum matched */
			struct libusb_device_descriptor desc;
			libusb_get_device_descriptor(list[i], &desc);
			if (desc.idVendor != vendor_id
			    || desc.idProduct != product_id) {
				fprintf(stderr, "ERROR: Bus %03d Device %03d not a FEL device "
					"(expected %04x:%04x, got %04x:%04x)\n", busnum, devnum,
					vendor_id, product_id, desc.idVendor, desc.idProduct);
				exit(1);
			}
			/* open handle to this specific device (incrementing its refcount) */
			rc = libusb_open(list[i], &result);
			if (rc != 0)
				usb_error(rc, "libusb_open()", 1);
			break;
		}
	}
	libusb_free_device_list(list, true);

	if (!found) {
		fprintf(stderr, "ERROR: Bus %03d Device %03d not found in libusb device list\n",
			busnum, devnum);
		exit(1);
	}
	return result;
}

int main(int argc, char **argv)
{
	bool uboot_autostart = false; /* flag for "uboot" command = U-Boot autostart */
	bool pflag_active = false; /* -p switch, causing "write" to output progress */
	libusb_device_handle *handle;
	int busnum = -1, devnum = -1;
#if defined(__linux__)
	int iface_detached = -1;
#endif

	if (argc <= 1) {
		puts("sunxi-fel " VERSION "\n");
		printf("Usage: %s [options] command arguments... [command...]\n"
			"	-v, --verbose			Verbose logging\n"
			"	-p, --progress			\"write\" transfers show a progress bar\n"
			"	-d, --dev bus:devnum		Use specific USB bus and device number\n"
			"\n"
			"	spl file			Load and execute U-Boot SPL\n"
			"		If file additionally contains a main U-Boot binary\n"
			"		(u-boot-sunxi-with-spl.bin), this command also transfers that\n"
			"		to memory (default address from image), but won't execute it.\n"
			"\n"
			"	uboot file-with-spl		like \"spl\", but actually starts U-Boot\n"
			"		U-Boot execution will take place when the fel utility exits.\n"
			"		This allows combining \"uboot\" with further \"write\" commands\n"
			"		(to transfer other files needed for the boot).\n"
			"\n"
			"	hex[dump] address length	Dumps memory region in hex\n"
			"	dump address length		Binary memory dump\n"
			"	exe[cute] address		Call function address\n"
			"	reset64 address			RMR request for AArch64 warm boot\n"
			"	readl address			Read 32-bit value from device memory\n"
			"	writel address value		Write 32-bit value to device memory\n"
			"	read address length file	Write memory contents into file\n"
			"	write address file		Store file contents into memory\n"
			"	write-with-progress addr file	\"write\" with progress bar\n"
			"	write-with-gauge addr file	Output progress for \"dialog --gauge\"\n"
			"	write-with-xgauge addr file	Extended gauge output (updates prompt)\n"
			"	multi[write] # addr file ...	\"write-with-progress\" multiple files,\n"
			"					sharing a common progress status\n"
			"	multi[write]-with-gauge ...	like their \"write-with-*\" counterpart,\n"
			"	multi[write]-with-xgauge ...	  but following the 'multi' syntax:\n"
			"					  <#> addr file [addr file [...]]\n"
			"	echo-gauge \"some text\"		Update prompt/caption for gauge output\n"
			"	ver[sion]			Show BROM version\n"
			"	sid				Retrieve and output 128-bit SID key\n"
			"	clear address length		Clear memory\n"
			"	fill address length value	Fill memory\n"
			, argv[0]
		);
		exit(0);
	}

	/* process all "prefix"-type arguments first */
	while (argc > 1) {
		if (strcmp(argv[1], "--verbose") == 0 || strcmp(argv[1], "-v") == 0)
			verbose = true;
		else if (strcmp(argv[1], "--progress") == 0 || strcmp(argv[1], "-p") == 0)
			pflag_active = true;
		else if (strncmp(argv[1], "--dev", 5) == 0 || strncmp(argv[1], "-d", 2) == 0) {
			char *dev_arg = argv[1];
			dev_arg += strspn(dev_arg, "-dev="); /* skip option chars, ignore '=' */
			if (*dev_arg == 0 && argc > 2) { /* at end of argument, use the next one instead */
				dev_arg = argv[2];
				argc -= 1;
				argv += 1;
			}
			if (sscanf(dev_arg, "%d:%d", &busnum, &devnum) != 2
			    || busnum <= 0 || devnum <= 0) {
				fprintf(stderr, "ERROR: Expected 'bus:devnum', got '%s'.\n", dev_arg);
				exit(1);
			}
		} else
			break; /* no valid (prefix) option detected, exit loop */
		argc -= 1;
		argv += 1;
	}

	int rc = libusb_init(NULL);
	assert(rc == 0);
	handle = open_fel_device(busnum, devnum, AW_USB_VENDOR_ID, AW_USB_PRODUCT_ID);
	assert(handle != NULL);
	rc = libusb_claim_interface(handle, 0);
#if defined(__linux__)
	if (rc != LIBUSB_SUCCESS) {
		libusb_detach_kernel_driver(handle, 0);
		iface_detached = 0;
		rc = libusb_claim_interface(handle, 0);
	}
#endif
	assert(rc == 0);

	if (aw_fel_get_endpoint(handle)) {
		fprintf(stderr, "ERROR: Failed to get FEL mode endpoint addresses!\n");
		exit(1);
	}

	while (argc > 1 ) {
		int skip = 1;

		if (strncmp(argv[1], "hex", 3) == 0 && argc > 3) {
			aw_fel_hexdump(handle, strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0));
			skip = 3;
		} else if (strncmp(argv[1], "dump", 4) == 0 && argc > 3) {
			aw_fel_dump(handle, strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0));
			skip = 3;
		} else if (strcmp(argv[1], "readl") == 0 && argc > 2) {
			printf("0x%08x\n", aw_fel_readl(handle, strtoul(argv[2], NULL, 0)));
			skip = 2;
		} else if (strcmp(argv[1], "writel") == 0 && argc > 3) {
			aw_fel_writel(handle, strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0));
			skip = 3;
		} else if (strncmp(argv[1], "exe", 3) == 0 && argc > 2) {
			aw_fel_execute(handle, strtoul(argv[2], NULL, 0));
			skip=3;
		} else if (strcmp(argv[1], "reset64") == 0 && argc > 2) {
			aw_rmr_request(handle, strtoul(argv[2], NULL, 0), true);
			/* Cancel U-Boot autostart, and stop processing args */
			uboot_autostart = false;
			break;
		} else if (strncmp(argv[1], "ver", 3) == 0) {
			aw_fel_print_version(handle);
		} else if (strcmp(argv[1], "sid") == 0) {
			aw_fel_print_sid(handle);
		} else if (strcmp(argv[1], "write") == 0 && argc > 3) {
			skip += 2 * file_upload(handle, 1, argc - 2, argv + 2,
					pflag_active ? progress_bar : NULL);
		} else if (strcmp(argv[1], "write-with-progress") == 0 && argc > 3) {
			skip += 2 * file_upload(handle, 1, argc - 2, argv + 2,
						progress_bar);
		} else if (strcmp(argv[1], "write-with-gauge") == 0 && argc > 3) {
			skip += 2 * file_upload(handle, 1, argc - 2, argv + 2,
						progress_gauge);
		} else if (strcmp(argv[1], "write-with-xgauge") == 0 && argc > 3) {
			skip += 2 * file_upload(handle, 1, argc - 2, argv + 2,
						progress_gauge_xxx);
		} else if ((strcmp(argv[1], "multiwrite") == 0 ||
			    strcmp(argv[1], "multi") == 0) && argc > 4) {
			size_t count = strtoul(argv[2], NULL, 0); /* file count */
			skip = 2 + 2 * file_upload(handle, count, argc - 3,
						   argv + 3, progress_bar);
		} else if ((strcmp(argv[1], "multiwrite-with-gauge") == 0 ||
			    strcmp(argv[1], "multi-with-gauge") == 0) && argc > 4) {
			size_t count = strtoul(argv[2], NULL, 0); /* file count */
			skip = 2 + 2 * file_upload(handle, count, argc - 3,
						   argv + 3, progress_gauge);
		} else if ((strcmp(argv[1], "multiwrite-with-xgauge") == 0 ||
			    strcmp(argv[1], "multi-with-xgauge") == 0) && argc > 4) {
			size_t count = strtoul(argv[2], NULL, 0); /* file count */
			skip = 2 + 2 * file_upload(handle, count, argc - 3,
						   argv + 3, progress_gauge_xxx);
		} else if ((strcmp(argv[1], "echo-gauge") == 0) && argc > 2) {
			skip = 2;
			printf("XXX\n0\n%s\nXXX\n", argv[2]);
			fflush(stdout);
		} else if (strcmp(argv[1], "read") == 0 && argc > 4) {
			size_t size = strtoul(argv[3], NULL, 0);
			void *buf = malloc(size);
			aw_fel_read(handle, strtoul(argv[2], NULL, 0), buf, size);
			save_file(argv[4], buf, size);
			free(buf);
			skip=4;
		} else if (strcmp(argv[1], "clear") == 0 && argc > 2) {
			aw_fel_fill(handle, strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0), 0);
			skip=3;
		} else if (strcmp(argv[1], "fill") == 0 && argc > 3) {
			aw_fel_fill(handle, strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0), (unsigned char)strtoul(argv[4], NULL, 0));
			skip=4;
		} else if (strcmp(argv[1], "spl") == 0 && argc > 2) {
			aw_fel_process_spl_and_uboot(handle, argv[2]);
			skip=2;
		} else if (strcmp(argv[1], "uboot") == 0 && argc > 2) {
			aw_fel_process_spl_and_uboot(handle, argv[2]);
			uboot_autostart = (uboot_entry > 0 && uboot_size > 0);
			if (!uboot_autostart)
				printf("Warning: \"uboot\" command failed to detect image! Can't execute U-Boot.\n");
			skip=2;
		} else {
			fprintf(stderr,"Invalid command %s\n", argv[1]);
			exit(1);
		}
		argc-=skip;
		argv+=skip;
	}

	/* auto-start U-Boot if requested (by the "uboot" command) */
	if (uboot_autostart) {
		pr_info("Starting U-Boot (0x%08X).\n", uboot_entry);
		aw_fel_execute(handle, uboot_entry);
	}

	libusb_release_interface(handle, 0);
#if defined(__linux__)
	if (iface_detached >= 0)
		libusb_attach_kernel_driver(handle, iface_detached);
#endif
	libusb_close(handle);
	libusb_exit(NULL);

	return 0;
}
