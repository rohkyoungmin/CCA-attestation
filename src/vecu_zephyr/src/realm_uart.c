#define DT_DRV_COMPAT lkvm_realm_uart

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define REALM_STATUS_MAGIC 0x524d5354UL
#define REALM_PAYLOAD_PHASE_UART_DRV_INIT_BEFORE 0x69U
#define REALM_PAYLOAD_PHASE_UART_DRV_INIT_AFTER  0x6aU

#define REALM_UART_REG_RBR 0x0
#define REALM_UART_REG_THR 0x0
#define REALM_UART_REG_IER 0x1
#define REALM_UART_REG_FCR 0x2
#define REALM_UART_REG_LCR 0x3
#define REALM_UART_REG_MCR 0x4
#define REALM_UART_REG_LSR 0x5
#define REALM_UART_REG_DLL 0x0
#define REALM_UART_REG_DLM 0x1

#define REALM_UART_LCR_8N1 0x03U
#define REALM_UART_LCR_DLAB BIT(7)

#define REALM_UART_MCR_DTR BIT(0)
#define REALM_UART_MCR_RTS BIT(1)
#define REALM_UART_MCR_OUT2 BIT(3)

#define REALM_UART_FCR_ENABLE BIT(0)
#define REALM_UART_FCR_RXRST BIT(1)
#define REALM_UART_FCR_TXRST BIT(2)

#define REALM_UART_LSR_DR   BIT(0)
#define REALM_UART_LSR_OE   BIT(1)
#define REALM_UART_LSR_PE   BIT(2)
#define REALM_UART_LSR_FE   BIT(3)
#define REALM_UART_LSR_THRE BIT(5)

struct realm_uart_config {
	mem_addr_t base;
	uint32_t clock_frequency;
	uint32_t current_speed;
	uint8_t reg_shift;
};

struct realm_uart_data {
	struct uart_config uart_cfg;
};

struct realm_status_page {
	uint32_t magic;
	uint32_t phase;
	uint32_t heartbeat;
	uint32_t reserved;
	uint64_t value0;
	uint64_t value1;
	char text[64];
};

extern uintptr_t realm_payload_status_shared_addr;

static void realm_uart_publish_status(uint32_t phase, uint64_t value0,
				      uint64_t value1, const char *text)
{
	volatile struct realm_status_page *page;
	size_t i;

	if (realm_payload_status_shared_addr == 0U) {
		return;
	}

	page = (volatile struct realm_status_page *)realm_payload_status_shared_addr;
	page->magic = REALM_STATUS_MAGIC;
	page->phase = phase;
	page->value0 = value0;
	page->value1 = value1;

	for (i = 0; i < sizeof(page->text); i++) {
		page->text[i] = '\0';
	}

	if (text != NULL) {
		for (i = 0; i < sizeof(page->text) - 1 && text[i] != '\0'; i++) {
			page->text[i] = text[i];
		}
	}
}

static inline mem_addr_t realm_uart_reg_addr(const struct realm_uart_config *cfg,
					       uint32_t reg)
{
	return cfg->base + ((mem_addr_t)reg << cfg->reg_shift);
}

static inline uint8_t realm_uart_read8(const struct realm_uart_config *cfg,
					 uint32_t reg)
{
	return sys_read8(realm_uart_reg_addr(cfg, reg));
}

static inline void realm_uart_write8(const struct realm_uart_config *cfg,
					  uint32_t reg, uint8_t val)
{
	sys_write8(val, realm_uart_reg_addr(cfg, reg));
}

static int realm_uart_configure_hw(const struct realm_uart_config *cfg,
				     const struct uart_config *uart_cfg)
{
	uint32_t baudrate;
	uint32_t divisor;
	uint8_t lcr = REALM_UART_LCR_8N1;

	if (uart_cfg->parity != UART_CFG_PARITY_NONE ||
	    uart_cfg->stop_bits != UART_CFG_STOP_BITS_1 ||
	    uart_cfg->data_bits != UART_CFG_DATA_BITS_8 ||
	    uart_cfg->flow_ctrl != UART_CFG_FLOW_CTRL_NONE) {
		return -ENOTSUP;
	}

	baudrate = uart_cfg->baudrate ? uart_cfg->baudrate : cfg->current_speed;
	if (baudrate == 0U || cfg->clock_frequency == 0U) {
		return -EINVAL;
	}

	divisor = cfg->clock_frequency / (16U * baudrate);
	if (divisor == 0U) {
		divisor = 1U;
	}

	realm_uart_write8(cfg, REALM_UART_REG_IER, 0U);
	realm_uart_write8(cfg, REALM_UART_REG_LCR, REALM_UART_LCR_DLAB);
	realm_uart_write8(cfg, REALM_UART_REG_DLL, divisor & 0xffU);
	realm_uart_write8(cfg, REALM_UART_REG_DLM, (divisor >> 8) & 0xffU);
	realm_uart_write8(cfg, REALM_UART_REG_LCR, lcr);
	realm_uart_write8(cfg, REALM_UART_REG_FCR,
			  REALM_UART_FCR_ENABLE | REALM_UART_FCR_RXRST |
				  REALM_UART_FCR_TXRST);
	realm_uart_write8(cfg, REALM_UART_REG_MCR,
			  REALM_UART_MCR_DTR | REALM_UART_MCR_RTS |
				  REALM_UART_MCR_OUT2);

	return 0;
}

static int realm_uart_poll_in(const struct device *dev, unsigned char *c)
{
	const struct realm_uart_config *cfg = dev->config;

	if ((realm_uart_read8(cfg, REALM_UART_REG_LSR) & REALM_UART_LSR_DR) == 0U) {
		return -1;
	}

	*c = realm_uart_read8(cfg, REALM_UART_REG_RBR);
	return 0;
}

static void realm_uart_poll_out(const struct device *dev, unsigned char c)
{
	const struct realm_uart_config *cfg = dev->config;

	while ((realm_uart_read8(cfg, REALM_UART_REG_LSR) & REALM_UART_LSR_THRE) == 0U) {
		/* Busy wait; shell backend uses polling mode in Realm bring-up. */
	}

	realm_uart_write8(cfg, REALM_UART_REG_THR, c);
}

static int realm_uart_err_check(const struct device *dev)
{
	const struct realm_uart_config *cfg = dev->config;
	uint8_t lsr = realm_uart_read8(cfg, REALM_UART_REG_LSR);
	int err = 0;

	if ((lsr & REALM_UART_LSR_OE) != 0U) {
		err |= UART_ERROR_OVERRUN;
	}
	if ((lsr & REALM_UART_LSR_PE) != 0U) {
		err |= UART_ERROR_PARITY;
	}
	if ((lsr & REALM_UART_LSR_FE) != 0U) {
		err |= UART_ERROR_FRAMING;
	}

	return err;
}

#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
static int realm_uart_configure(const struct device *dev,
				 const struct uart_config *uart_cfg)
{
	struct realm_uart_data *data = dev->data;
	const struct realm_uart_config *cfg = dev->config;
	int ret;

	ret = realm_uart_configure_hw(cfg, uart_cfg);
	if (ret == 0) {
		data->uart_cfg = *uart_cfg;
	}

	return ret;
}

static int realm_uart_config_get(const struct device *dev,
				   struct uart_config *uart_cfg)
{
	const struct realm_uart_data *data = dev->data;

	*uart_cfg = data->uart_cfg;
	return 0;
}
#endif

static int realm_uart_init(const struct device *dev)
{
	struct realm_uart_data *data = dev->data;
	const struct realm_uart_config *cfg = dev->config;
	struct uart_config uart_cfg = {
		.baudrate = cfg->current_speed,
		.parity = UART_CFG_PARITY_NONE,
		.stop_bits = UART_CFG_STOP_BITS_1,
		.data_bits = UART_CFG_DATA_BITS_8,
		.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
	};
	/*
	 * Realm shell bring-up is currently blocked in PRE_KERNEL_1 when the
	 * driver programs the UART registers this early. For the polling shell
	 * path, the emulated UART is already usable enough for raw THR/LSR
	 * access, so keep init side-effect free and let later poll_in/poll_out
	 * use the shared-alias MMIO path directly.
	 */
	realm_uart_publish_status(REALM_PAYLOAD_PHASE_UART_DRV_INIT_BEFORE,
				  cfg->base, cfg->current_speed,
				  "uart_drv_init_before");

	data->uart_cfg = uart_cfg;
	realm_uart_publish_status(REALM_PAYLOAD_PHASE_UART_DRV_INIT_AFTER,
				  cfg->base, cfg->current_speed,
				  "uart_drv_init_after");
	return 0;
}

static const struct uart_driver_api realm_uart_driver_api = {
	.poll_in = realm_uart_poll_in,
	.poll_out = realm_uart_poll_out,
	.err_check = realm_uart_err_check,
#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
	.configure = realm_uart_configure,
	.config_get = realm_uart_config_get,
#endif
};

#define REALM_UART_INIT(n)                                                     \
	static const struct realm_uart_config realm_uart_cfg_##n = {           \
		.base = DT_INST_REG_ADDR(n),                                   \
		.clock_frequency = DT_INST_PROP_OR(n, clock_frequency, 1843200),\
		.current_speed = DT_INST_PROP_OR(n, current_speed, 115200),    \
		.reg_shift = DT_INST_PROP_OR(n, reg_shift, 0),                 \
	};                                                                   \
	static struct realm_uart_data realm_uart_data_##n;                    \
	DEVICE_DT_INST_DEFINE(n, realm_uart_init, NULL,                       \
			      &realm_uart_data_##n, &realm_uart_cfg_##n,      \
			      PRE_KERNEL_1, CONFIG_SERIAL_INIT_PRIORITY,      \
			      &realm_uart_driver_api)

DT_INST_FOREACH_STATUS_OKAY(REALM_UART_INIT);
