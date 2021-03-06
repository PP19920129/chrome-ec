/* Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "atomic.h"
#include "board.h"
#include "common.h"
#include "console.h"
#include "gpio.h"
#include "hwtimer.h"
#include "hooks.h"
#include "i2c.h"
#include "registers.h"
#include "spi.h"
#include "task.h"
#include "timer.h"
#include "touchpad.h"
#include "touchpad_st.h"
#include "update_fw.h"
#include "usb_api.h"
#include "usb_hid_touchpad.h"
#include "usb_isochronous.h"
#include "util.h"

/* Console output macros */
#define CC_TOUCHPAD CC_USB
#define CPUTS(outstr) cputs(CC_TOUCHPAD, outstr)
#define CPRINTF(format, args...) cprintf(CC_TOUCHPAD, format, ## args)
#define CPRINTS(format, args...) cprints(CC_TOUCHPAD, format, ## args)

#define TASK_EVENT_POWERON  TASK_EVENT_CUSTOM(1)
#define TASK_EVENT_POWEROFF  TASK_EVENT_CUSTOM(2)

#define SPI (&(spi_devices[SPI_ST_TP_DEVICE_ID]))

BUILD_ASSERT(sizeof(struct st_tp_event_t) == 8);

/* Function prototypes */
static int st_tp_read_all_events(void);
static int st_tp_read_host_buffer_header(void);
static int st_tp_send_ack(void);
static int st_tp_start_scan(void);
static int st_tp_stop_scan(void);
static int st_tp_update_system_state(int new_state, int mask);

/* Global variables */
/*
 * Current system state, meaning of each bit is defined below.
 */
static int system_state;

#define SYSTEM_STATE_DEBUG_MODE		(1 << 0)
#define SYSTEM_STATE_ENABLE_HEAT_MAP	(1 << 1)
#define SYSTEM_STATE_ENABLE_DOME_SWITCH	(1 << 2)
#define SYSTEM_STATE_ACTIVE_MODE	(1 << 3)
#define SYSTEM_STATE_DOME_SWITCH_LEVEL  (1 << 4)

/*
 * Timestamp of last interrupt (32 bits are enough as we divide the value by 100
 * and then put it in a 16-bit field).
 */
static uint32_t irq_ts;

/*
 * Cached system info.
 */
static struct st_tp_system_info_t system_info;

static struct {
#if ST_TP_DUMMY_BYTE == 1
	uint8_t dummy;
#endif
	union {
		uint8_t bytes[512];
		struct st_tp_host_buffer_header_t buffer_header;
		struct st_tp_host_buffer_heat_map_t heat_map;
		struct st_tp_host_data_header_t data_header;
		struct st_tp_event_t events[32];
	} /* anonymous */;
} __packed rx_buf;


#ifdef CONFIG_USB_ISOCHRONOUS
#define USB_ISO_PACKET_SIZE 256
/*
 * Header of each USB pacaket.
 */
struct packet_header_t {
	uint8_t index;

#define HEADER_FLAGS_NEW_FRAME	(1 << 0)
	uint8_t flags;
} __packed;
BUILD_ASSERT(sizeof(struct packet_header_t) < USB_ISO_PACKET_SIZE);

static struct packet_header_t packet_header;

/* What will be sent to USB interface. */
struct st_tp_usb_packet_t {
#define USB_FRAME_FLAGS_BUTTON	(1 << 0)
	/*
	 * This will be true if user clicked on touchpad.
	 * TODO(b/70482333): add corresponding code for button signal.
	 */
	uint8_t flags;

	/*
	 * This will be `st_tp_host_buffer_heat_map_t.frame` but each pixel
	 * will be scaled to 8 bits value.
	 */
	uint8_t frame[ST_TOUCH_ROWS * ST_TOUCH_COLS];
} __packed;

/* Next buffer index SPI will write to. */
static volatile uint32_t spi_buffer_index;
/* Next buffer index USB will read from. */
static volatile uint32_t usb_buffer_index;
static struct st_tp_usb_packet_t usb_packet[2]; /* double buffering */
/* How many bytes we have transmitted. */
static size_t transmit_report_offset;

/* Function prototypes */
static int get_heat_map_addr(void);
static void print_frame(void);
static void st_tp_disable_heat_map(void);
static void st_tp_enable_heat_map(void);
static int st_tp_read_frame(void);
static void st_tp_interrupt_send(void);
DECLARE_DEFERRED(st_tp_interrupt_send);
#endif


/* Function implementations */

static void set_bits(int *lvalue, int rvalue, int mask)
{
	*lvalue &= ~mask;
	*lvalue |= rvalue & mask;
}

/*
 * Parse a finger report from ST event and save it to (report)->finger.
 *
 * @param report: pointer to a USB HID touchpad report.
 * @param event: a pointer event from ST.
 * @param i: array index for next finger.
 *
 * @return array index of next finger (i.e. (i + 1) if a finger is added).
 */
static int st_tp_parse_finger(struct usb_hid_touchpad_report *report,
			      struct st_tp_event_t *event,
			      int i)
{
	/* We cannot report more fingers */
	if (i >= ARRAY_SIZE(report->finger))
		return i;

	/* This is not a finger */
	if (event->finger.touch_type == ST_TP_TOUCH_TYPE_INVALID)
		return i;

	switch (event->evt_id) {
	case ST_TP_EVENT_ID_ENTER_POINTER:
	case ST_TP_EVENT_ID_MOTION_POINTER:
		report->finger[i].tip = 1;
		report->finger[i].inrange = 1;
		report->finger[i].id = event->finger.touch_id;
		/* z is 8 bit uint, while pressure is 10 bits. */
		report->finger[i].pressure = event->finger.z << 2;
		/* width and height are 12 bits, ST only reports 6 bits */
		report->finger[i].width = (event->finger.minor |
					   (event->minor_high << 4)) << 6;
		report->finger[i].height = (event->finger.major |
					    (event->major_high << 4)) << 6;
		report->finger[i].x = (CONFIG_USB_HID_TOUCHPAD_LOGICAL_MAX_X -
				       event->finger.x);
		report->finger[i].y = (CONFIG_USB_HID_TOUCHPAD_LOGICAL_MAX_Y -
				       event->finger.y);
		break;
	case ST_TP_EVENT_ID_LEAVE_POINTER:
		report->finger[i].id = event->finger.touch_id;
		break;
	}
	return i + 1;
}

static int st_tp_write_hid_report(void)
{
	int ret, i, num_finger, num_events, domeswitch_changed = 0;
	struct usb_hid_touchpad_report report;

	ret = st_tp_read_host_buffer_header();
	if (ret)
		return ret;

	if (rx_buf.buffer_header.flags & ST_TP_BUFFER_HEADER_DOMESWITCH_CHG) {
		/*
		 * dome_switch_level from device is inverted.
		 * That is, 0 => pressed, 1 => released.
		 */
		set_bits(&system_state,
			 (rx_buf.buffer_header.dome_switch_level ?
			  0 : SYSTEM_STATE_DOME_SWITCH_LEVEL),
			 SYSTEM_STATE_DOME_SWITCH_LEVEL);
		domeswitch_changed = 1;
	}

	num_events = st_tp_read_all_events();
	if (num_events < 0)
		return -num_events;

	memset(&report, 0, sizeof(report));
	report.id = 0x1;
	num_finger = 0;

	for (i = 0; i < num_events; i++) {
		struct st_tp_event_t *e = &rx_buf.events[i];

		switch (e->evt_id) {
		case ST_TP_EVENT_ID_ENTER_POINTER:
		case ST_TP_EVENT_ID_MOTION_POINTER:
		case ST_TP_EVENT_ID_LEAVE_POINTER:
			num_finger = st_tp_parse_finger(&report, e, num_finger);
			break;
		default:
			break;
		}
	}

	if (!num_finger && !domeswitch_changed)  /* nothing changed */
		return 0;

	report.button = !!(system_state & SYSTEM_STATE_DOME_SWITCH_LEVEL);
	report.count = num_finger;
	report.timestamp = irq_ts / USB_HID_TOUCHPAD_TIMESTAMP_UNIT;

	set_touchpad_report(&report);
	return 0;
}

static int st_tp_read_report(void)
{
	if (system_state & SYSTEM_STATE_ENABLE_HEAT_MAP) {
#ifdef CONFIG_USB_ISOCHRONOUS
		/*
		 * Because we are using double buffering, so, if
		 * usb_buffer_index = N
		 *
		 * 1. spi_buffer_index == N      => ok, both slots are empty
		 * 2. spi_buffer_index == N + 1  => ok, second slot is empty
		 * 3. spi_buffer_index == N + 2  => not ok, need to wait for USB
		 */
		if (spi_buffer_index - usb_buffer_index <= 1) {
			if (st_tp_read_frame() == EC_SUCCESS) {
				spi_buffer_index++;
				if (system_state & SYSTEM_STATE_DEBUG_MODE) {
					print_frame();
					usb_buffer_index++;
				}
			}
		}
		if (spi_buffer_index > usb_buffer_index)
			hook_call_deferred(&st_tp_interrupt_send_data, 0);
#endif
	} else {
		st_tp_write_hid_report();
	}
	return st_tp_send_ack();
}

static int st_tp_read_host_buffer_header(void)
{
	const uint8_t tx_buf[] = { ST_TP_CMD_READ_SPI_HOST_BUFFER, 0x00, 0x00 };
	int rx_len = ST_TP_DUMMY_BYTE + sizeof(rx_buf.buffer_header);

	return spi_transaction(SPI, tx_buf, sizeof(tx_buf),
			       (uint8_t *)&rx_buf, rx_len);
}

static int st_tp_send_ack(void)
{
	uint8_t tx_buf[] = { ST_TP_CMD_SPI_HOST_BUFFER_ACK };

	return spi_transaction(SPI, tx_buf, sizeof(tx_buf), NULL, 0);
}

static int st_tp_update_system_state(int new_state, int mask)
{
	int ret = EC_SUCCESS;
	int need_locked_scan_mode = 0;

	/* copy reserved bits */
	set_bits(&new_state, system_state, ~mask);

	mask = SYSTEM_STATE_DEBUG_MODE;
	if ((new_state & mask) != (system_state & mask))
		set_bits(&system_state, new_state, mask);

	mask = SYSTEM_STATE_ENABLE_HEAT_MAP | SYSTEM_STATE_ENABLE_DOME_SWITCH;
	if ((new_state & mask) != (system_state & mask)) {
		uint8_t tx_buf[] = {
			ST_TP_CMD_WRITE_FEATURE_SELECT,
			0x05,
			0
		};
		if (new_state & SYSTEM_STATE_ENABLE_HEAT_MAP) {
			tx_buf[2] |= 1 << 0;
			need_locked_scan_mode = 1;
		}
		if (new_state & SYSTEM_STATE_ENABLE_DOME_SWITCH)
			tx_buf[2] |= 1 << 1;
		ret = spi_transaction(SPI, tx_buf, sizeof(tx_buf), NULL, 0);
		if (ret)
			return ret;
		set_bits(&system_state, new_state, mask);
	}

	mask = SYSTEM_STATE_ACTIVE_MODE;
	if ((new_state & mask) != (system_state & mask)) {
		uint8_t tx_buf[] = {
			ST_TP_CMD_WRITE_SCAN_MODE_SELECT,
			ST_TP_SCAN_MODE_ACTIVE,
			!!(new_state & SYSTEM_STATE_ACTIVE_MODE),
		};
		CPRINTS("Enable Multi-Touch: %d", tx_buf[2]);
		ret = spi_transaction(SPI, tx_buf, sizeof(tx_buf), NULL, 0);
		if (ret)
			return ret;
		set_bits(&system_state, new_state, mask);
	}

	/* We need to lock scan mode to prevent scan rate drop when heat map
	 * mode is enabled.
	 */
	if (need_locked_scan_mode) {
		uint8_t tx_buf[] = {
			ST_TP_CMD_WRITE_SCAN_MODE_SELECT,
			ST_TP_SCAN_MODE_LOCKED,
			0x0,
		};

		ret = spi_transaction(SPI, tx_buf, sizeof(tx_buf), NULL, 0);
		if (ret)
			return ret;
	}
	return ret;
}

static void st_tp_enable_interrupt(int enable)
{
	uint8_t tx_buf[] = {
		ST_TP_CMD_WRITE_SYSTEM_COMMAND, 0x01, enable ? 1 : 0};
	if (enable)
		gpio_enable_interrupt(GPIO_TOUCHPAD_INT);
	spi_transaction(SPI, tx_buf, sizeof(tx_buf), NULL, 0);
	if (!enable)
		gpio_disable_interrupt(GPIO_TOUCHPAD_INT);
}

static int st_tp_start_scan(void)
{
	int new_state = (SYSTEM_STATE_ACTIVE_MODE |
			 SYSTEM_STATE_ENABLE_DOME_SWITCH);
	int mask = new_state;
	int ret;

	ret = st_tp_update_system_state(new_state, mask);
	if (ret)
		return ret;
	st_tp_send_ack();
	st_tp_enable_interrupt(1);
	return ret;
}

static int st_tp_read_host_data_memory(uint16_t addr, void *rx_buf, int len)
{
	uint8_t tx_buf[] = {
		ST_TP_CMD_READ_HOST_DATA_MEMORY, addr >> 8, addr & 0xFF
	};

	return spi_transaction(SPI, tx_buf, sizeof(tx_buf), rx_buf, len);
}

static int st_tp_stop_scan(void)
{
	int new_state = 0;
	int mask = SYSTEM_STATE_ACTIVE_MODE;
	int ret;

	ret = st_tp_update_system_state(new_state, mask);
	st_tp_enable_interrupt(0);
	return ret;
}

static int st_tp_load_host_data(uint8_t mem_id)
{
	uint8_t tx_buf[] = {
		ST_TP_CMD_WRITE_SYSTEM_COMMAND, 0x06, mem_id
	};
	int retry, ret;
	uint16_t count;
	struct st_tp_host_data_header_t *header = &rx_buf.data_header;
	int rx_len = sizeof(*header) + ST_TP_DUMMY_BYTE;

	st_tp_read_host_data_memory(0x0000, &rx_buf, rx_len);
	if (header->host_data_mem_id == mem_id)
		return EC_SUCCESS; /* already loaded no need to reload */

	count = header->count;

	ret = spi_transaction(SPI, tx_buf, sizeof(tx_buf), NULL, 0);
	if (ret)
		return ret;

	ret = EC_ERROR_TIMEOUT;
	retry = 5;
	while (retry--) {
		st_tp_read_host_data_memory(0x0000, &rx_buf, rx_len);
		if (header->magic == ST_TP_HEADER_MAGIC &&
		    header->host_data_mem_id == mem_id &&
		    header->count != count) {
			ret = EC_SUCCESS;
			break;
		}
		msleep(10);
	}
	return ret;
}

/*
 * Read System Info from Host Data Memory.
 *
 * @param reload: true to force reloading system info into host data memory
 *                before reading.
 */
static int st_tp_read_system_info(int reload)
{
	int ret = EC_SUCCESS;
	int rx_len = ST_TP_DUMMY_BYTE + ST_TP_SYSTEM_INFO_LEN;
	uint8_t *ptr = rx_buf.bytes;

	if (reload)
		ret = st_tp_load_host_data(ST_TP_MEM_ID_SYSTEM_INFO);
	if (ret)
		return ret;
	ret = st_tp_read_host_data_memory(0x0000, &rx_buf, rx_len);
	if (ret)
		return ret;

	/* Parse the content */
	memcpy(&system_info, ptr, ST_TP_SYSTEM_INFO_PART_1_SIZE);

	/* Check header */
	if (system_info.header.magic != ST_TP_HEADER_MAGIC ||
	    system_info.header.host_data_mem_id != ST_TP_MEM_ID_SYSTEM_INFO)
		return EC_ERROR_UNKNOWN;

	ptr += ST_TP_SYSTEM_INFO_PART_1_SIZE;
	ptr += ST_TP_SYSTEM_INFO_PART_1_RESERVED;
	memcpy(&system_info.scr_res_x, ptr, ST_TP_SYSTEM_INFO_PART_2_SIZE);

#define ST_TP_SHOW(attr) CPRINTS(#attr ": %04x", system_info.attr)
	ST_TP_SHOW(chip0_id[0]);
	ST_TP_SHOW(chip0_id[1]);
	ST_TP_SHOW(chip0_ver);
	ST_TP_SHOW(scr_tx_len);
	ST_TP_SHOW(scr_rx_len);
	ST_TP_SHOW(release_info);
#undef ST_TP_SHOW
	return ret;
}

/*
 * Handles error reports.
 *
 * @return 0 for minor errors, non-zero for major errors (must halt).
 * TODO(stimim): check for major errors.
 */
static int st_tp_handle_error_report(struct st_tp_event_t *e)
{
	if (e->magic != ST_TP_EVENT_MAGIC ||
	    e->evt_id != ST_TP_EVENT_ID_ERROR_REPORT)
		return 0;

	CPRINTS("Touchpad error: %x %x", e->report.report_type,
		((e->report.info[0] << 24) | (e->report.info[1] << 16) |
		 (e->report.info[2] << 8) | (e->report.info[3] << 0)));

	return 0;
}

/*
 * Read all events, and handle errors.
 *
 * @return number of events available on success, or negative error code on
 *         failure.
 */
static int st_tp_read_all_events(void)
{
	uint8_t cmd = ST_TP_CMD_READ_ALL_EVENTS;
	int rx_len = sizeof(rx_buf.events) + ST_TP_DUMMY_BYTE;
	int ret;
	int i;

	ret = spi_transaction(SPI, &cmd, 1, (uint8_t *)&rx_buf, rx_len);
	if (ret)
		return -ret;

	for (i = 0; i < ARRAY_SIZE(rx_buf.events); i++) {
		struct st_tp_event_t *e = &rx_buf.events[i];

		if (e->magic != ST_TP_EVENT_MAGIC)
			break;

		if (e->evt_id == ST_TP_EVENT_ID_ERROR_REPORT) {
			ret = st_tp_handle_error_report(e);
			if (ret)
				return -ret;
		}
	}
	return i;
}

/*
 * Reset touchpad.  This function will wait for "controller ready" event after
 * the touchpad is reset.
 */
static int st_tp_reset(void)
{
	int i, num_events, retry = 100;

	board_touchpad_reset();

	while (retry--) {
		num_events = st_tp_read_all_events();
		if (num_events < 0)
			return -num_events;

		for (i = 0; i < num_events; i++) {
			struct st_tp_event_t *e = &rx_buf.events[i];

			if (e->evt_id == ST_TP_EVENT_ID_CONTROLLER_READY) {
				CPRINTS("Touchpad ready");
				return 0;
			}
		}

		msleep(10);
	}
	CPRINTS("Timeout waiting for controller ready.");
	return EC_ERROR_TIMEOUT;
}

/* Initialize the controller ICs after reset */
static void st_tp_init(void)
{
	if (st_tp_reset())
		return;
	/*
	 * On boot, ST firmware will load system info to host data memory,
	 * So we don't need to reload it.
	 */
	st_tp_read_system_info(0);

	system_state = 0;

	st_tp_start_scan();
}

#ifdef CONFIG_USB_UPDATE
int touchpad_get_info(struct touchpad_info *tp)
{
	if (st_tp_read_system_info(1)) {
		tp->status = EC_RES_SUCCESS;
		tp->vendor = ST_VENDOR_ID;
		/*
		 * failed to get system info, FW corrupted, return some default
		 * values.
		 */
		tp->st.id = 0x3936;
		tp->st.fw_version = 0;
		tp->st.fw_checksum = 0;
		return sizeof(*tp);
	}

	tp->status = EC_RES_SUCCESS;
	tp->vendor = ST_VENDOR_ID;
	tp->st.id = (system_info.chip0_id[0] << 8) | system_info.chip0_id[1];
	tp->st.fw_version = system_info.release_info;
	tp->st.fw_checksum = system_info.fw_crc;

	return sizeof(*tp);
}

/*
 * Helper functions for firmware update
 *
 * There is no documentation about ST_TP_CMD_WRITE_HW_REG (0xFA).
 * All implementations below are based on sample code from ST.
 */
static int write_hwreg_cmd32(uint32_t address, uint32_t data)
{
	uint8_t tx_buf[] = {
		ST_TP_CMD_WRITE_HW_REG,
		(address >> 24) & 0xFF,
		(address >> 16) & 0xFF,
		(address >> 8) & 0xFF,
		(address >> 0) & 0xFF,
		(data >> 24) & 0xFF,
		(data >> 16) & 0xFF,
		(data >> 8) & 0xFF,
		(data >> 0) & 0xFF,
	};

	return spi_transaction(SPI, tx_buf, sizeof(tx_buf), NULL, 0);
}

static int write_hwreg_cmd8(uint32_t address, uint8_t data)
{
	uint8_t tx_buf[] = {
		ST_TP_CMD_WRITE_HW_REG,
		(address >> 24) & 0xFF,
		(address >> 16) & 0xFF,
		(address >> 8) & 0xFF,
		(address >> 0) & 0xFF,
		data,
	};

	return spi_transaction(SPI, tx_buf, sizeof(tx_buf), NULL, 0);
}

static int wait_for_flash_ready(uint8_t type)
{
	uint8_t tx_buf[] = {
		ST_TP_CMD_READ_HW_REG,
		0x20, 0x00, 0x00, type,
	};
	int ret = EC_SUCCESS, retry = 200;

	while (retry--) {
		ret = spi_transaction(SPI, tx_buf, sizeof(tx_buf),
				      (uint8_t *)&rx_buf, 2);
		if (ret == EC_SUCCESS && !(rx_buf.bytes[0] & 0x80))
			break;
		msleep(50);
	}
	return retry >= 0 ? ret : EC_ERROR_TIMEOUT;
}

static int erase_flash(void)
{
	int ret;

	/* Erase everything, except CX */
	ret = write_hwreg_cmd32(0x20000128, 0xFFFFFF83);
	if (ret)
		return ret;
	ret = write_hwreg_cmd8(0x2000006B, 0x00);
	if (ret)
		return ret;
	ret = write_hwreg_cmd8(0x2000006A, 0xA0);
	if (ret)
		return ret;
	return wait_for_flash_ready(0x6A);
}

static int st_tp_prepare_for_update(void)
{
	/* hold m3 */
	write_hwreg_cmd8(0x20000024, 0x01);
	/* unlock flash */
	write_hwreg_cmd8(0x20000025, 0x20);
	/* unlock flash erase */
	write_hwreg_cmd8(0x200000DE, 0x03);
	erase_flash();

	return EC_SUCCESS;
}

static int st_tp_start_flash_dma(void)
{
	int ret;

	ret = write_hwreg_cmd8(0x20000071, 0xC0);
	if (ret)
		return ret;
	ret = wait_for_flash_ready(0x71);
	return ret;
}

static int st_tp_write_one_chunk(const uint8_t *head,
				 uint32_t addr, uint32_t chunk_size)
{
	uint8_t tx_buf[ST_TP_DMA_CHUNK_SIZE + 5];
	uint32_t index = 0;
	int ret;

	index = 0;

	tx_buf[index++] = ST_TP_CMD_WRITE_HW_REG;
	tx_buf[index++] = (addr >> 24) & 0xFF;
	tx_buf[index++] = (addr >> 16) & 0xFF;
	tx_buf[index++] = (addr >> 8) & 0xFF;
	tx_buf[index++] = (addr >> 0) & 0xFF;
	memcpy(tx_buf + index, head, chunk_size);
	ret = spi_transaction(SPI, tx_buf, chunk_size + 5, NULL, 0);

	return ret;
}

/*
 * @param offset: offset in memory to copy the data (in bytes).
 * @param size: length of data (in bytes).
 * @param data: pointer to data bytes.
 */
static int st_tp_write_flash(int offset, int size, const uint8_t *data)
{
	uint8_t tx_buf[12] = {0};
	const uint8_t *head = data, *tail = data + size;
	uint32_t addr, index, chunk_size;
	uint32_t flash_buffer_size;
	int ret;

	offset >>= 2;  /* offset should be count in words */
	/*
	 * To write to flash, the data has to be separated into several chunks.
	 * Each chunk will be no more than `ST_TP_DMA_CHUNK_SIZE` bytes.
	 * The chunks will first be saved into a buffer, the buffer can only
	 * holds `ST_TP_FLASH_BUFFER_SIZE` bytes.  We have to flush the buffer
	 * when the capacity is reached.
	 */
	while (head < tail) {
		addr = 0x00100000;
		flash_buffer_size = 0;
		while (flash_buffer_size < ST_TP_FLASH_BUFFER_SIZE) {
			chunk_size = MIN(ST_TP_DMA_CHUNK_SIZE, tail - head);
			ret = st_tp_write_one_chunk(head, addr, chunk_size);
			if (ret)
				return ret;

			flash_buffer_size += chunk_size;
			addr += chunk_size;
			head += chunk_size;

			if (head >= tail)
				break;
		}

		/* configuring the DMA */
		flash_buffer_size = flash_buffer_size / 4 - 1;
		index = 0;

		tx_buf[index++] = ST_TP_CMD_WRITE_HW_REG;
		tx_buf[index++] = 0x20;
		tx_buf[index++] = 0x00;
		tx_buf[index++] = 0x00;
		tx_buf[index++] = 0x72;  /* flash DMA config */
		tx_buf[index++] = 0x00;
		tx_buf[index++] = 0x00;

		tx_buf[index++] = offset & 0xFF;
		tx_buf[index++] = (offset >> 8) & 0xFF;
		tx_buf[index++] = flash_buffer_size & 0xFF;
		tx_buf[index++] = (flash_buffer_size >> 8) & 0xFF;
		tx_buf[index++] = 0x00;

		ret = spi_transaction(SPI, tx_buf, index, NULL, 0);
		if (ret)
			return ret;
		ret = st_tp_start_flash_dma();
		if (ret)
			return ret;

		offset += ST_TP_FLASH_BUFFER_SIZE / 4;
	}
	return EC_SUCCESS;
}

static int st_tp_check_command_echo(const uint8_t *cmd,
				    const size_t len)
{
	int num_events, i;
	num_events = st_tp_read_all_events();
	if (num_events < 0)
		return -num_events;

	for (i = 0; i < num_events; i++) {
		struct st_tp_event_t *e = &rx_buf.events[i];

		if (e->evt_id == ST_TP_EVENT_ID_STATUS_REPORT &&
		    e->report.report_type == ST_TP_STATUS_CMD_ECHO &&
		    memcmp(e->report.info, cmd, MIN(4, len)) == 0)
			return 0;
	}
	return -EC_ERROR_BUSY;
}

static void st_tp_full_initialize_end(void);
DECLARE_DEFERRED(st_tp_full_initialize_end);

static void st_tp_full_initialize_end(void)
{
	int ret;
	uint8_t tx_buf[] = { ST_TP_CMD_WRITE_SYSTEM_COMMAND, 0x00, 0x03 };

	ret = st_tp_check_command_echo(tx_buf, sizeof(tx_buf));
	if (ret == EC_SUCCESS) {
		CPRINTS("Full panel initialization completed.");
		st_tp_init();
	} else if (ret == -EC_ERROR_BUSY) {
		hook_call_deferred(&st_tp_full_initialize_end_data, 100 * MSEC);
	} else {
		CPRINTS("Full Panel initialization failed: %x", -ret);
	}
}

static void st_tp_full_initialize_start(void)
{
	uint8_t tx_buf[] = { ST_TP_CMD_WRITE_SYSTEM_COMMAND, 0x00, 0x03 };

	st_tp_stop_scan();
	if (st_tp_reset())
		return;

	CPRINTS("Start full initialization");
	spi_transaction(SPI, tx_buf, sizeof(tx_buf), NULL, 0);

	hook_call_deferred(&st_tp_full_initialize_end_data, 100 * MSEC);
}

/*
 * @param offset: should be address between 0 to 1M, aligned with
 *	ST_TP_DMA_CHUNK_SIZE.
 * @param size: length of `data` array.
 * @param data: content of new touchpad firmware.
 */
int touchpad_update_write(int offset, int size, const uint8_t *data)
{
	int ret;

	CPRINTS("%s %08x %d", __func__, offset, size);
	if (offset == 0) {
		/* stop scanning, interrupt, etc... */
		st_tp_stop_scan();

		ret = st_tp_prepare_for_update();
		if (ret)
			return ret;
	}

	if (offset % ST_TP_DMA_CHUNK_SIZE)
		return EC_ERROR_INVAL;

	if (offset >= ST_TP_FLASH_OFFSET_CX &&
	    offset < ST_TP_FLASH_OFFSET_CONFIG)
		/* don't update CX section */
		return EC_SUCCESS;

	ret = st_tp_write_flash(offset, size, data);
	if (ret)
		return ret;

	if (offset + size == CONFIG_TOUCHPAD_VIRTUAL_SIZE) {
		CPRINTS("%s: End update, wait for reset.", __func__);

		st_tp_full_initialize_start();
	}

	return EC_SUCCESS;
}

int touchpad_debug(const uint8_t *param, unsigned int param_size,
		   uint8_t **data, unsigned int *data_size)
{
	if (param_size != 1)
		return EC_RES_INVALID_PARAM;

	switch (*param) {
	case ST_TP_DEBUG_CMD_CALIBRATE:
		/* no return value */
		*data = NULL;
		*data_size = 0;
		st_tp_full_initialize_start();
		return EC_SUCCESS;
	}
	return EC_RES_INVALID_PARAM;
}
#endif

void touchpad_interrupt(enum gpio_signal signal)
{
	irq_ts = __hw_clock_source_read();

	task_wake(TASK_ID_TOUCHPAD);
}

void touchpad_task(void *u)
{
	uint32_t event;

	st_tp_init();

	while (1) {
		event = task_wait_event(-1);

		if (event & TASK_EVENT_WAKE)
			while (!gpio_get_level(GPIO_TOUCHPAD_INT))
				st_tp_read_report();

		if (event & TASK_EVENT_POWERON)
			st_tp_start_scan();
		else if (event & TASK_EVENT_POWEROFF)
			st_tp_stop_scan();
	}
}

#ifdef CONFIG_USB_SUSPEND
static void touchpad_usb_pm_change(void)
{
	if (usb_is_suspended() && !usb_is_remote_wakeup_enabled())
		task_set_event(TASK_ID_TOUCHPAD, TASK_EVENT_POWEROFF, 0);
	else
		task_set_event(TASK_ID_TOUCHPAD, TASK_EVENT_POWERON, 0);
}
DECLARE_HOOK(HOOK_USB_PM_CHANGE, touchpad_usb_pm_change, HOOK_PRIO_DEFAULT);
#endif

#ifdef CONFIG_USB_ISOCHRONOUS
static void st_tp_enable_heat_map(void)
{
	int new_state = (SYSTEM_STATE_ENABLE_HEAT_MAP |
			 SYSTEM_STATE_ENABLE_DOME_SWITCH |
			 SYSTEM_STATE_ACTIVE_MODE);
	int mask = new_state;

	st_tp_update_system_state(new_state, mask);
}
DECLARE_DEFERRED(st_tp_enable_heat_map);

static void st_tp_disable_heat_map(void)
{
	int new_state = 0;
	int mask = SYSTEM_STATE_ENABLE_HEAT_MAP;

	st_tp_update_system_state(new_state, mask);
}
DECLARE_DEFERRED(st_tp_disable_heat_map);

static void print_frame(void)
{
	char debug_line[ST_TOUCH_COLS + 5];
	int i, j, index;
	int v;
	struct st_tp_usb_packet_t *packet = &usb_packet[usb_buffer_index & 1];

	if (usb_buffer_index == spi_buffer_index)
		/* buffer is empty. */
		return;

	/* We will have ~150 FPS, let's print ~4 frames per second */
	if (usb_buffer_index % 37 == 0) {
		/* move cursor back to top left corner */
		CPRINTF("\x1b[H");
		CPUTS("==============\n");
		for (i = 0; i < ST_TOUCH_ROWS; i++) {
			for (j = 0; j < ST_TOUCH_COLS; j++) {
				index = i * ST_TOUCH_COLS;
				index += (ST_TOUCH_COLS - j - 1); // flip X
				v = packet->frame[index];

				if (v > 0)
					debug_line[j] = '0' + v * 10 / 256;
				else
					debug_line[j] = ' ';
			}
			debug_line[j++] = '\n';
			debug_line[j++] = '\0';
			CPRINTF(debug_line);
		}
		CPUTS("==============\n");
	}
}

static int st_tp_read_frame(void)
{
	struct st_tp_host_buffer_heat_map_t *heat_map = &rx_buf.heat_map;
	int ret = EC_SUCCESS;
	int rx_len = sizeof(*heat_map) + ST_TP_DUMMY_BYTE;
	int heat_map_addr = get_heat_map_addr();
	uint8_t tx_buf[] = {
		ST_TP_CMD_READ_SPI_HOST_BUFFER,
		(heat_map_addr >> 8) & 0xFF,
		(heat_map_addr >> 0) & 0xFF,
	};

	if (heat_map_addr < 0)
		goto failed;
	/*
	 * Theoretically, we should read host buffer header to check if data is
	 * valid, but the data should always be ready when interrupt pin is low.
	 * Let's skip this check for now.
	 */
	ret = spi_transaction(SPI, tx_buf, sizeof(tx_buf),
			      (uint8_t *)&rx_buf, rx_len);
	if (ret == EC_SUCCESS) {
#if BYTES_PER_PIXEL == 1
		/*
		 * If BYTES_PER_PIXEL = 1, then we can memcpy directly.
		 * This takes about 0.1ms per frame.
		 */
		memcpy(dest, heat_map->frame, ST_TOUCH_COLS * ST_TOUCH_ROWS);
#elif BYTES_PER_PIXEL == 2
		/*
		 * Down scaling and move data into usb_packet, this takes
		 * about 0.35ms per frame
		 */
		int i;
		int16_t v;
		uint8_t *dest = usb_packet[spi_buffer_index & 1].frame;
		uint8_t max_value = 0;

		for (i = 0; i < ST_TOUCH_COLS * ST_TOUCH_ROWS; i++) {
			v = (heat_map->frame[i * 2] |
			     (heat_map->frame[i * 2 + 1] << 8));
			v = MAX(0, v);
			v = MIN(v >> (BITS_PER_PIXEL - 8), 255);
			if (v < ST_TP_HEAT_MAP_THRESHOLD)
				v = 0;
			dest[i] = v;
			max_value |= v;
		}

		if (max_value == 0) // empty frame
			return -1;
#else
#error "BYTES_PER_PIXEL can only be 1 or 2"
#endif
	}
failed:
	return ret;
}

/* Define USB interface for heat_map */

/* function prototypes */
static int st_tp_usb_set_interface(usb_uint alternate_setting,
				   usb_uint interface);
static int heatmap_send_packet(struct usb_isochronous_config const *config);
static void st_tp_usb_tx_callback(struct usb_isochronous_config const *config);

/* USB descriptors */
USB_ISOCHRONOUS_CONFIG_FULL(usb_st_tp_heatmap_config,
			    USB_IFACE_ST_TOUCHPAD,
			    USB_CLASS_VENDOR_SPEC,
			    0,  /* subclass */
			    0,  /* protocol */
			    USB_STR_HEATMAP_NAME,  /* interface name */
			    USB_EP_ST_TOUCHPAD,
			    USB_ISO_PACKET_SIZE,
			    st_tp_usb_tx_callback,
			    st_tp_usb_set_interface,
			    1 /* 1 extra EP for interrupts */)

/* ***This function will be executed in interrupt context*** */
void st_tp_usb_tx_callback(struct usb_isochronous_config const *config)
{
	task_wake(TASK_ID_HEATMAP);
}

void heatmap_task(void *unused)
{
	struct usb_isochronous_config const *config;

	config = &usb_st_tp_heatmap_config;

	while (1) {
		/* waiting st_tp_usb_tx_callback() */
		task_wait_event(-1);

		if (system_state & SYSTEM_STATE_DEBUG_MODE)
			continue;

		if (usb_buffer_index == spi_buffer_index)
			/* buffer is empty */
			continue;

		while (heatmap_send_packet(config))
			/* We failed to write a packet, try again later. */
			task_wait_event(100);
	}
}

/* USB interface has completed TX, it's asking for more data */
static int heatmap_send_packet(struct usb_isochronous_config const *config)
{
	size_t num_byte_available;
	size_t offset = 0;
	int ret, buffer_id = -1;
	struct st_tp_usb_packet_t *packet = &usb_packet[usb_buffer_index & 1];

	packet_header.flags = 0;
	num_byte_available = sizeof(*packet) - transmit_report_offset;
	if (num_byte_available > 0) {
		if (transmit_report_offset == 0)
			packet_header.flags |= HEADER_FLAGS_NEW_FRAME;
		ret = usb_isochronous_write_buffer(
				config,
				(uint8_t *)&packet_header,
				sizeof(packet_header),
				offset,
				&buffer_id,
				0);
		/*
		 * Since USB_ISO_PACKET_SIZE > sizeof(packet_header), this must
		 * be true.
		 */
		if (ret != sizeof(packet_header))
			return -1;

		offset += ret;
		packet_header.index++;

		ret = usb_isochronous_write_buffer(
				config,
				(uint8_t *)packet + transmit_report_offset,
				num_byte_available,
				offset,
				&buffer_id,
				1);
		if (ret < 0) {
			/* TODO(b/70482333): handle this error, it might be:
			 *   1. timeout (buffer_id changed)
			 *   2. invalid offset
			 *
			 * For now, let's just return an error and try again.
			 */
			CPRINTS("%s %d: %d", __func__, __LINE__, -ret);
			return ret;
		}

		/* We should have sent some bytes, update offset */
		transmit_report_offset += ret;
		if (transmit_report_offset == sizeof(*packet)) {
			transmit_report_offset = 0;
			usb_buffer_index++;
		}
	}
	return 0;
}

static int st_tp_usb_set_interface(usb_uint alternate_setting,
				   usb_uint interface)
{
	if (alternate_setting == 1) {
		hook_call_deferred(&st_tp_enable_heat_map_data, 0);
		return 0;
	} else if (alternate_setting == 0) {
		hook_call_deferred(&st_tp_disable_heat_map_data, 0);
		return 0;
	} else  /* we only have two settings. */
		return -1;
}

static int get_heat_map_addr(void)
{
	/*
	 * TODO(stimim): drop this when we are sure all trackpads are having the
	 * same config (e.g. after EVT).
	 */
	if (system_info.release_info >= 0x3)
		return 0x0120;
	else if (system_info.release_info == 0x1)
		return 0x20;
	else
		return -1; /* Unknown version */
}

struct st_tp_interrupt_t {
#define ST_TP_INT_FRAME_AVAILABLE	(1 << 0)
	uint8_t flags;
} __packed;

static usb_uint st_tp_usb_int_buffer[
	DIV_ROUND_UP(sizeof(struct st_tp_interrupt_t), 2)] __usb_ram;

const struct usb_endpoint_descriptor USB_EP_DESC(USB_IFACE_ST_TOUCHPAD, 81) = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = 0x80 | USB_EP_ST_TOUCHPAD_INT,
	.bmAttributes = 0x03 /* Interrupt endpoint */,
	.wMaxPacketSize = sizeof(struct st_tp_interrupt_t),
	.bInterval = 1 /* ms */,
};

static void st_tp_interrupt_send(void)
{
	struct st_tp_interrupt_t report;

	memset(&report, 0, sizeof(report));

	if (usb_buffer_index < spi_buffer_index)
		report.flags |= ST_TP_INT_FRAME_AVAILABLE;
	memcpy_to_usbram((void *)usb_sram_addr(st_tp_usb_int_buffer),
			 &report, sizeof(report));
	/* enable TX */
	STM32_TOGGLE_EP(USB_EP_ST_TOUCHPAD_INT, EP_TX_MASK, EP_TX_VALID, 0);
	usb_wake();
}

static void st_tp_interrupt_tx(void)
{
	STM32_USB_EP(USB_EP_ST_TOUCHPAD_INT) &= EP_MASK;

	if (usb_buffer_index < spi_buffer_index)
		/* pending frames */
		hook_call_deferred(&st_tp_interrupt_send_data, 0);
}

static void st_tp_interrupt_event(enum usb_ep_event evt)
{
	int ep = USB_EP_ST_TOUCHPAD_INT;

	if (evt == USB_EVENT_RESET) {
		btable_ep[ep].tx_addr = usb_sram_addr(st_tp_usb_int_buffer);
		btable_ep[ep].tx_count = sizeof(struct st_tp_interrupt_t);

		STM32_USB_EP(ep) = ((ep << 0) |
				    EP_TX_VALID |
				    (3 << 9) /* interrupt EP */ |
				    EP_RX_DISAB);
	}
}

USB_DECLARE_EP(USB_EP_ST_TOUCHPAD_INT, st_tp_interrupt_tx, st_tp_interrupt_tx,
	       st_tp_interrupt_event);

#endif

/* Debugging commands */
static int command_touchpad_st(int argc, char **argv)
{
	if (argc != 2)
		return EC_ERROR_PARAM_COUNT;
	if (strcasecmp(argv[1], "version") == 0) {
		st_tp_read_system_info(1);
		return EC_SUCCESS;
	} else if (strcasecmp(argv[1], "calibrate") == 0) {
		st_tp_full_initialize_start();
		return EC_SUCCESS;
	} else if (strcasecmp(argv[1], "enable") == 0) {
#ifdef CONFIG_USB_ISOCHRONOUS
		set_bits(&system_state, SYSTEM_STATE_DEBUG_MODE,
			 SYSTEM_STATE_DEBUG_MODE);
		hook_call_deferred(&st_tp_enable_heat_map_data, 0);
		return 0;
#else
		return EC_ERROR_NOT_HANDLED;
#endif
	} else if (strcasecmp(argv[1], "disable") == 0) {
#ifdef CONFIG_USB_ISOCHRONOUS
		set_bits(&system_state, 0, SYSTEM_STATE_DEBUG_MODE);
		hook_call_deferred(&st_tp_disable_heat_map_data, 0);
		return 0;
#else
		return EC_ERROR_NOT_HANDLED;
#endif
	} else {
		return EC_ERROR_PARAM1;
	}
}
DECLARE_CONSOLE_COMMAND(touchpad_st, command_touchpad_st,
			"<enable|disable|version>",
			"Read write spi. id is spi_devices array index");
