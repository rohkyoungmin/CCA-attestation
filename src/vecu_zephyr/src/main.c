#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include "rsi.h"

#ifdef CONFIG_SHELL
#include <zephyr/shell/shell.h>
#ifdef REALM_MINI_SHELL
#include <zephyr/shell/shell_dummy.h>
#endif
#endif

#define REALM_STATUS_MAGIC       0x524d5354UL
#define REALM_PAYLOAD_PHASE_MAIN 0x20U
#define REALM_PAYLOAD_PHASE_HB   0x21U
#define REALM_PAYLOAD_PHASE_INIT_PRE_KERNEL_1 0x60U
#define REALM_PAYLOAD_PHASE_INIT_PRE_KERNEL_2 0x61U
#define REALM_PAYLOAD_PHASE_INIT_POST_KERNEL  0x62U
#define REALM_PAYLOAD_PHASE_INIT_APPLICATION  0x63U
#define REALM_PAYLOAD_PHASE_INIT_AFTER_INTC   0x64U
#define REALM_PAYLOAD_PHASE_INIT_AFTER_SERIAL 0x65U
#define REALM_PAYLOAD_PHASE_SHELL_PING        0x66U
#define REALM_PAYLOAD_PHASE_UART_SMOKE_BEFORE 0x67U
#define REALM_PAYLOAD_PHASE_UART_SMOKE_AFTER  0x68U
#define REALM_PAYLOAD_PHASE_MINI_SHELL_READY  0x70U
#define REALM_PAYLOAD_PHASE_ATTEST_OK         0x71U
#define REALM_PAYLOAD_PHASE_ATTEST_FAIL       0x72U
#define REALM_PAYLOAD_PHASE_MINI_SHELL_PROMPT 0x73U
#define REALM_PAYLOAD_PHASE_MINI_UART_INIT_BEFORE 0x74U
#define REALM_PAYLOAD_PHASE_MINI_UART_INIT_AFTER  0x75U
#define REALM_PAYLOAD_PHASE_MINI_UART_CFG_BEFORE  0x76U
#define REALM_PAYLOAD_PHASE_MINI_UART_CFG_AFTER   0x77U
#define REALM_PAYLOAD_PHASE_MINI_BANNER_BEFORE    0x78U
#define REALM_PAYLOAD_PHASE_MINI_BANNER_AFTER     0x79U
#define REALM_PAYLOAD_PHASE_MINI_CHAR_BEFORE      0x7aU
#define REALM_PAYLOAD_PHASE_MINI_CHAR_AFTER       0x7bU
#define REALM_PAYLOAD_PHASE_HOST_REQ              0x7eU
#define REALM_PAYLOAD_PHASE_HOST_ACK              0x7fU

#define REALM_UART_PRIVATE_BASE 0x01000000UL
#define REALM_UART_REG_RBR      0x0U
#define REALM_UART_REG_THR      0x0U
#define REALM_UART_REG_IER      0x1U
#define REALM_UART_REG_FCR      0x2U
#define REALM_UART_REG_LCR      0x3U
#define REALM_UART_REG_MCR      0x4U
#define REALM_UART_REG_LSR      0x5U
#define REALM_UART_REG_DLL      0x0U
#define REALM_UART_REG_DLM      0x1U
#define REALM_UART_LSR_DR       BIT(0)
#define REALM_UART_LSR_THRE     BIT(5)
#define REALM_UART_LCR_8N1      0x03U
#define REALM_UART_LCR_DLAB     BIT(7)
#define REALM_UART_MCR_DTR      BIT(0)
#define REALM_UART_MCR_RTS      BIT(1)
#define REALM_UART_MCR_OUT2     BIT(3)
#define REALM_UART_FCR_ENABLE   BIT(0)
#define REALM_UART_FCR_RXRST    BIT(1)
#define REALM_UART_FCR_TXRST    BIT(2)
#define REALM_UART_CLOCK_HZ     1843200U
#define REALM_UART_BAUDRATE     115200U
#define REALM_UART_TX_SPIN_MAX  1000000U
#define REALM_MINI_SHELL_REPROMPT_SPINS 50000U
#define REALM_MINI_SHELL_PROMPT "realm:~$ "
#define REALM_ATTEST_BENCH_MAX_ITER 100U

struct realm_status_page {
	uint32_t magic;
	uint32_t phase;
	uint32_t heartbeat;
	uint32_t reserved;
	uint64_t value0;
	uint64_t value1;
	char text[64];
};

uintptr_t realm_payload_status_shared_addr;
uintptr_t realm_payload_shared_alias_bit;
uintptr_t realm_payload_dtb_addr;

static uint8_t realm_private_token_buf[RSI_ATTEST_TOKEN_MAX_SIZE]
	__attribute__((aligned(RSI_GRANULE_SIZE)));
static uint32_t realm_attest_generation_count;
static uint32_t realm_host_generation_count;
static bool realm_shared_mem_ready;
static uint64_t realm_bench_gen_ns[REALM_ATTEST_BENCH_MAX_ITER];
static uint64_t realm_bench_publish_ns[REALM_ATTEST_BENCH_MAX_ITER];
static uint64_t realm_bench_normal_ns[REALM_ATTEST_BENCH_MAX_ITER];
static uint64_t realm_bench_agl_verify_ns[REALM_ATTEST_BENCH_MAX_ITER];
static uint64_t realm_bench_total_ns[REALM_ATTEST_BENCH_MAX_ITER];
static uint64_t realm_bench_sort_buf[REALM_ATTEST_BENCH_MAX_ITER];

static inline uint64_t payload_read_counter(void)
{
	uint64_t val;

	__asm__ volatile("mrs %0, cntvct_el0" : "=r" (val));
	return val;
}

static inline uint64_t payload_read_counter_freq(void)
{
	uint64_t val;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r" (val));
	return val;
}

static uint64_t payload_counter_delta_to_ns(uint64_t cycles)
{
	uint64_t freq = payload_read_counter_freq();

	if (freq == 0U) {
		return 0U;
	}

	return (cycles * 1000000000ULL) / freq;
}

static volatile struct realm_status_page *payload_status_page(void)
{
	if (realm_payload_status_shared_addr == 0U) {
		return NULL;
	}

	return (volatile struct realm_status_page *)realm_payload_status_shared_addr;
}

static void payload_publish_status(uint32_t phase, uint64_t value0,
				       uint64_t value1, const char *text)
{
	volatile struct realm_status_page *page = payload_status_page();
	size_t i;

	if (page == NULL) {
		return;
	}

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

static void payload_phase_hold(uint32_t spins)
{
	for (volatile uint32_t i = 0; i < spins; i++) {
		/* busy wait */
	}
}

static void payload_publish_status_sticky(uint32_t phase, uint64_t value0,
					      uint64_t value1, const char *text)
{
	payload_publish_status(phase, value0, value1, text);
	payload_phase_hold(5000000U);
}

static int payload_ensure_shared_mem(void)
{
	int ret;

	if (realm_shared_mem_ready) {
		return 0;
	}

	ret = rsi_shared_mem_init();
	if (ret == 0) {
		realm_shared_mem_ready = true;
	}

	return ret;
}

static int payload_generate_attestation_measured(uint64_t *token_size_out,
						 uintptr_t *token_buf_out,
						 uint64_t *gen_cycles_out,
						 uint64_t *gen_ns_out,
						 uint64_t *publish_cycles_out,
						 uint64_t *publish_ns_out)
{
	uint8_t challenge[RSI_CHALLENGE_SIZE];
	uint8_t *token_buf = realm_private_token_buf;
	uint8_t *shared_token_buf =
		(uint8_t *)(realm_payload_shared_alias_bit | ATTEST_TOKEN_BASE);
	comm_ctrl_t *ctrl =
		(comm_ctrl_t *)(realm_payload_shared_alias_bit | COMM_CTRL_BASE);
	uint64_t token_size = 0;
	uint64_t t0;
	uint64_t t1;
	uint64_t cycles;
	uint64_t ns;
	uint64_t publish_t0;
	uint64_t publish_t1;
	uint64_t publish_cycles;
	uint64_t publish_ns;
	int ret;
	size_t i;

	for (i = 0; i < sizeof(challenge); i++) {
		challenge[i] = (uint8_t)i;
	}

	memset((void *)token_buf, 0, RSI_ATTEST_TOKEN_MAX_SIZE);

	t0 = payload_read_counter();
	ret = rsi_attestation_get_token(challenge, token_buf,
					RSI_ATTEST_TOKEN_MAX_SIZE, &token_size);
	t1 = payload_read_counter();
	cycles = t1 - t0;
	ns = payload_counter_delta_to_ns(cycles);
	if (ret != 0) {
		payload_publish_status(REALM_PAYLOAD_PHASE_ATTEST_FAIL, (uint64_t)ret, 0,
				       "attest token fail");
		return ret;
	}

	publish_t0 = payload_read_counter();
	ret = payload_ensure_shared_mem();
	if (ret != 0) {
		payload_publish_status(REALM_PAYLOAD_PHASE_ATTEST_FAIL, (uint64_t)ret, 0,
				       "attest shared init fail");
		return ret;
	}

	memset((void *)shared_token_buf, 0, RSI_ATTEST_TOKEN_MAX_SIZE);
	memcpy((void *)shared_token_buf, token_buf, (size_t)token_size);
	memset((void *)ctrl, 0, sizeof(*ctrl));
	memcpy(ctrl->challenge, challenge, sizeof(challenge));
	ctrl->magic = COMM_MAGIC;
	ctrl->token_ready = 1U;
	ctrl->token_size = (uint32_t)token_size;
	ctrl->gen_count = ++realm_attest_generation_count;
	ctrl->gen_time_ns = ns;
	publish_t1 = payload_read_counter();
	publish_cycles = publish_t1 - publish_t0;
	publish_ns = payload_counter_delta_to_ns(publish_cycles);
	payload_publish_status(REALM_PAYLOAD_PHASE_ATTEST_OK, token_size,
			       ns,
			       "attest token ready");

	if (token_size_out != NULL) {
		*token_size_out = token_size;
	}

	if (token_buf_out != NULL) {
		*token_buf_out = (uintptr_t)shared_token_buf;
	}

	if (gen_cycles_out != NULL) {
		*gen_cycles_out = cycles;
	}

	if (gen_ns_out != NULL) {
		*gen_ns_out = ns;
	}

	if (publish_cycles_out != NULL) {
		*publish_cycles_out = publish_cycles;
	}

	if (publish_ns_out != NULL) {
		*publish_ns_out = publish_ns;
	}

	return 0;
}

static int payload_generate_attestation(uint64_t *token_size_out,
					uintptr_t *token_buf_out,
					uint64_t *cycles_out,
					uint64_t *ns_out)
{
	return payload_generate_attestation_measured(token_size_out,
						    token_buf_out,
						    cycles_out,
						    ns_out,
						    NULL,
						    NULL);
}

static int payload_request_normal_world(const char *msg,
					uint32_t *ack_out,
					uint32_t *status_out,
					uint64_t *host_arg0_out,
					uint64_t *host_arg1_out)
{
	comm_ctrl_t *ctrl_mem =
		(comm_ctrl_t *)(realm_payload_shared_alias_bit | COMM_CTRL_BASE);
	volatile comm_ctrl_t *ctrl = ctrl_mem;
	uint32_t gen;
	size_t i;
	int ret;

	ret = payload_ensure_shared_mem();
	if (ret != 0) {
		payload_publish_status(REALM_PAYLOAD_PHASE_ATTEST_FAIL,
				       (uint64_t)ret, 0,
				       "host shared init fail");
		return ret;
	}

	if (ctrl->magic != COMM_MAGIC) {
		memset(ctrl_mem, 0, sizeof(*ctrl_mem));
		ctrl->magic = COMM_MAGIC;
	}

	gen = ++realm_host_generation_count;
	ctrl->host_req = COMM_HOST_REQ_NORMAL;
	ctrl->host_ack = 0U;
	ctrl->host_gen = gen;
	ctrl->host_status = UINT32_MAX;
	ctrl->host_arg0 = realm_payload_shared_alias_bit;
	ctrl->host_arg1 = realm_payload_dtb_addr;

	for (i = 0; i < COMM_HOST_MSG_SIZE; i++) {
		ctrl->host_msg[i] = '\0';
	}

	if (msg == NULL || msg[0] == '\0') {
		msg = "hello from Realm mini shell";
	}

	for (i = 0; i < COMM_HOST_MSG_SIZE - 1U && msg[i] != '\0'; i++) {
		ctrl->host_msg[i] = msg[i];
	}

	__asm__ volatile("dsb sy" ::: "memory");
	payload_publish_status(REALM_PAYLOAD_PHASE_HOST_REQ, gen,
			       COMM_HOST_REQ_NORMAL, "host req");

	for (uint32_t attempt = 0; attempt < 3000U; attempt++) {
		__asm__ volatile("dsb sy" ::: "memory");
		if (ctrl->host_ack == gen) {
			if (ack_out != NULL) {
				*ack_out = ctrl->host_ack;
			}
			if (status_out != NULL) {
				*status_out = ctrl->host_status;
			}
			if (host_arg0_out != NULL) {
				*host_arg0_out = ctrl->host_arg0;
			}
			if (host_arg1_out != NULL) {
				*host_arg1_out = ctrl->host_arg1;
			}
			payload_publish_status(REALM_PAYLOAD_PHASE_HOST_ACK, gen,
					       ctrl->host_status, "host ack");
			return 0;
		}
		k_busy_wait(1000U);
	}

	return -1;
}

#if defined(CONFIG_SHELL_BACKEND_SERIAL) && DT_NODE_HAS_STATUS(DT_CHOSEN(zephyr_shell_uart), okay)
static void payload_uart_smoke(void)
{
	const struct device *uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_shell_uart));
	static const char smoke[] = "\r\n[realm-uart] post-kernel smoke\r\n";
	const char *p = smoke;

	payload_publish_status(REALM_PAYLOAD_PHASE_UART_SMOKE_BEFORE,
			       (uint64_t)(uintptr_t)uart,
			       device_is_ready(uart) ? 1U : 0U,
			       "uart_smoke_before");

	if (!device_is_ready(uart)) {
		payload_publish_status(REALM_PAYLOAD_PHASE_UART_SMOKE_AFTER,
				       (uint64_t)(uintptr_t)uart, 0U,
				       "uart_smoke_not_ready");
		return;
	}

	while (*p != '\0') {
		uart_poll_out(uart, *p++);
	}

	payload_publish_status(REALM_PAYLOAD_PHASE_UART_SMOKE_AFTER,
			       (uint64_t)(uintptr_t)uart, 1U,
			       "uart_smoke_after");
}
#else
static void payload_uart_smoke(void)
{
}
#endif

#ifdef REALM_MINI_SHELL
static inline uintptr_t mini_shell_uart_base(void)
{
	return realm_payload_shared_alias_bit | REALM_UART_PRIVATE_BASE;
}

static inline uint8_t mini_shell_uart_read8(uint32_t reg)
{
	return sys_read8(mini_shell_uart_base() + reg);
}

static inline void mini_shell_uart_write8(uint32_t reg, uint8_t val)
{
	sys_write8(val, mini_shell_uart_base() + reg);
}

static void mini_shell_uart_init(void)
{
	/*
	 * The Realm payload already proved that raw THR/LSR accesses work.
	 * Reprogramming the emulated UART here causes the same early bring-up
	 * stalls we saw with the PRE_KERNEL driver path, so keep the mini-shell
	 * transport side-effect free and let poll_in/poll_out use the existing
	 * shared-alias MMIO state directly.
	 */
	payload_publish_status(REALM_PAYLOAD_PHASE_MINI_UART_INIT_BEFORE,
			       mini_shell_uart_base(),
			       realm_payload_shared_alias_bit,
			       "mini_uart_init_before");
	payload_publish_status(REALM_PAYLOAD_PHASE_MINI_UART_INIT_AFTER,
			       mini_shell_uart_base(),
			       realm_payload_shared_alias_bit,
			       "mini_uart_init_after");
}

static void mini_shell_uart_late_configure(void)
{
	payload_publish_status(REALM_PAYLOAD_PHASE_MINI_UART_CFG_BEFORE,
			       mini_shell_uart_base(),
			       0U,
			       "mini_uart_cfg_before");
	/*
	 * Any additional NS16550 programming here reintroduces the same stall
	 * pattern we saw in PRE_KERNEL_1. Treat the UART state as already
	 * usable and only use direct THR writes for visibility/debug.
	 */
	payload_publish_status(REALM_PAYLOAD_PHASE_MINI_UART_CFG_AFTER,
			       mini_shell_uart_base(),
			       0U,
			       "mini_uart_cfg_after");
}

static void mini_shell_putc(char c)
{
	/*
	 * At this stage we care more about proving prompt visibility than about
	 * conservative FIFO pacing. Direct THR writes avoid getting stuck on
	 * Realm-specific LSR polling behavior.
	 */
	mini_shell_uart_write8(REALM_UART_REG_THR, (uint8_t)c);
}

static void mini_shell_puts(const char *s)
{
	for (; *s != '\0'; s++) {
		if (*s == '\n') {
			mini_shell_putc('\r');
		}
		mini_shell_putc(*s);
	}
}

static int mini_shell_try_getc(char *out)
{
	if ((mini_shell_uart_read8(REALM_UART_REG_LSR) & REALM_UART_LSR_DR) == 0U) {
		return -1;
	}

	*out = (char)mini_shell_uart_read8(REALM_UART_REG_RBR);
	return 0;
}

static void mini_shell_print_hex_u64(uint64_t val)
{
	static const char hex[] = "0123456789abcdef";
	char buf[18];
	int i;

	buf[0] = '0';
	buf[1] = 'x';
	for (i = 0; i < 16; i++) {
		buf[2 + i] = hex[(val >> ((15 - i) * 4)) & 0xf];
	}
	for (i = 0; i < 18; i++) {
		mini_shell_putc(buf[i]);
	}
}

static void mini_shell_print_dec_u64(uint64_t val)
{
	char buf[21];
	size_t pos = sizeof(buf);

	if (val == 0U) {
		mini_shell_putc('0');
		return;
	}

	while (val != 0U && pos > 0U) {
		buf[--pos] = (char)('0' + (val % 10U));
		val /= 10U;
	}

	while (pos < sizeof(buf)) {
		mini_shell_putc(buf[pos++]);
	}
}

static bool mini_shell_parse_u32(const char *text, uint32_t *value)
{
	uint32_t out = 0U;

	if (text == NULL || text[0] == '\0') {
		return false;
	}

	while (*text == ' ') {
		text++;
	}

	if (*text == '\0') {
		return false;
	}

	while (*text != '\0') {
		if (*text < '0' || *text > '9') {
			return false;
		}
		out = out * 10U + (uint32_t)(*text - '0');
		text++;
	}

	*value = out;
	return true;
}

static void mini_shell_sort_u64(uint64_t *values, uint32_t n)
{
	for (uint32_t i = 1U; i < n; i++) {
		uint64_t key = values[i];
		uint32_t j = i;

		while (j > 0U && values[j - 1U] > key) {
			values[j] = values[j - 1U];
			j--;
		}
		values[j] = key;
	}
}

static void mini_shell_print_stats(const char *label,
				   const uint64_t *values,
				   uint32_t n)
{
	uint64_t sum = 0U;
	uint64_t min;
	uint64_t max;
	uint32_t p95_index;

	if (n == 0U) {
		return;
	}

	min = values[0];
	max = values[0];
	for (uint32_t i = 0U; i < n; i++) {
		uint64_t value = values[i];

		realm_bench_sort_buf[i] = value;
		sum += value;
		if (value < min) {
			min = value;
		}
		if (value > max) {
			max = value;
		}
	}

	mini_shell_sort_u64(realm_bench_sort_buf, n);
	p95_index = (n * 95U) / 100U;
	if (p95_index >= n) {
		p95_index = n - 1U;
	}

	mini_shell_puts("summary,");
	mini_shell_puts(label);
	mini_shell_puts(",mean_ns=");
	mini_shell_print_dec_u64(sum / n);
	mini_shell_puts(",min_ns=");
	mini_shell_print_dec_u64(min);
	mini_shell_puts(",p50_ns=");
	mini_shell_print_dec_u64(realm_bench_sort_buf[n / 2U]);
	mini_shell_puts(",p95_ns=");
	mini_shell_print_dec_u64(realm_bench_sort_buf[p95_index]);
	mini_shell_puts(",max_ns=");
	mini_shell_print_dec_u64(max);
	mini_shell_puts("\n");
}

static bool mini_shell_streq(const char *a, const char *b)
{
	while (*a == *b) {
		if (*a == '\0') {
			return true;
		}
		a++;
		b++;
	}

	return false;
}

static bool mini_shell_parse_arg(const char *line, const char *cmd,
				 const char **arg)
{
	size_t i;

	for (i = 0; cmd[i] != '\0'; i++) {
		if (line[i] != cmd[i]) {
			return false;
		}
	}

	if (line[i] == '\0') {
		*arg = "";
		return true;
	}

	if (line[i] != ' ') {
		return false;
	}

	while (line[i] == ' ') {
		i++;
	}

	*arg = &line[i];
	return true;
}

static void mini_shell_run_normal_attest_bench(const char *arg)
{
	uint32_t iterations = 1U;
	uint32_t warmup = 0U;

	if (arg != NULL && arg[0] != '\0') {
		if (!mini_shell_parse_u32(arg, &iterations) || iterations == 0U) {
			mini_shell_puts("normal attest: expected decimal iteration count\n");
			return;
		}
	}

	if (iterations > REALM_ATTEST_BENCH_MAX_ITER) {
		mini_shell_puts("normal attest: max iterations is ");
		mini_shell_print_dec_u64(REALM_ATTEST_BENCH_MAX_ITER);
		mini_shell_puts("\n");
		return;
	}

	if (iterations >= 50U) {
		warmup = 10U;
	} else if (iterations >= 10U) {
		warmup = 3U;
	}

	mini_shell_puts("normal attest: paper measurement\n");
	mini_shell_puts("warmup=");
	mini_shell_print_dec_u64(warmup);
	mini_shell_puts(",iterations=");
	mini_shell_print_dec_u64(iterations);
	mini_shell_puts("\n");

	for (uint32_t i = 0U; i < warmup; i++) {
		uint64_t token_size = 0U;
		uintptr_t token_buf = 0U;
		uint64_t gen_cycles = 0U;
		uint64_t gen_ns = 0U;
		uint64_t publish_cycles = 0U;
		uint64_t publish_ns = 0U;
		uint32_t ack = 0U;
		uint32_t status = COMM_HOST_STATUS_ERROR;
		int ret;

		ret = payload_generate_attestation_measured(&token_size, &token_buf,
							    &gen_cycles, &gen_ns,
							    &publish_cycles,
							    &publish_ns);
		if (ret == 0) {
			ret = payload_request_normal_world("attest", &ack, &status,
							   NULL, NULL);
		}
		if (ret != 0 || status != COMM_HOST_STATUS_OK) {
			mini_shell_puts("normal attest: warmup failed rc=");
			mini_shell_print_hex_u64((uint64_t)(uint32_t)ret);
			mini_shell_puts(" status=");
			mini_shell_print_hex_u64(status);
			mini_shell_puts("\n");
			return;
		}
	}

	mini_shell_puts("csv,iter,token_size,gen_cycles,gen_ns,publish_cycles,publish_ns,normal_cycles,normal_ns,total_cycles,total_ns,agl_verify_ns,agl_hash_ns,status\n");

	for (uint32_t i = 0U; i < iterations; i++) {
		uint64_t token_size = 0U;
		uintptr_t token_buf = 0U;
		uint64_t gen_cycles = 0U;
		uint64_t gen_ns = 0U;
		uint64_t publish_cycles = 0U;
		uint64_t publish_ns = 0U;
		uint64_t normal_t0;
		uint64_t normal_t1;
		uint64_t total_t0;
		uint64_t total_t1;
		uint64_t normal_cycles;
		uint64_t normal_ns;
		uint64_t agl_verify_ns = 0U;
		uint64_t agl_hash_ns = 0U;
		uint64_t total_cycles;
		uint64_t total_ns;
		uint32_t ack = 0U;
		uint32_t status = COMM_HOST_STATUS_ERROR;
		int ret;

		total_t0 = payload_read_counter();
		ret = payload_generate_attestation_measured(&token_size, &token_buf,
							    &gen_cycles, &gen_ns,
							    &publish_cycles,
							    &publish_ns);
		if (ret != 0) {
			mini_shell_puts("normal attest: token generation failed rc=");
			mini_shell_print_hex_u64((uint64_t)(uint32_t)ret);
			mini_shell_puts("\n");
			return;
		}

		normal_t0 = payload_read_counter();
		ret = payload_request_normal_world("attest", &ack, &status,
						   &agl_verify_ns, &agl_hash_ns);
		normal_t1 = payload_read_counter();
		total_t1 = normal_t1;

		normal_cycles = normal_t1 - normal_t0;
		normal_ns = payload_counter_delta_to_ns(normal_cycles);
		total_cycles = total_t1 - total_t0;
		total_ns = payload_counter_delta_to_ns(total_cycles);

		realm_bench_gen_ns[i] = gen_ns;
		realm_bench_publish_ns[i] = publish_ns;
		realm_bench_normal_ns[i] = normal_ns;
		realm_bench_agl_verify_ns[i] = agl_verify_ns;
		realm_bench_total_ns[i] = total_ns;

		mini_shell_puts("csv,");
		mini_shell_print_dec_u64(i + 1U);
		mini_shell_puts(",");
		mini_shell_print_dec_u64(token_size);
		mini_shell_puts(",");
		mini_shell_print_dec_u64(gen_cycles);
		mini_shell_puts(",");
		mini_shell_print_dec_u64(gen_ns);
		mini_shell_puts(",");
		mini_shell_print_dec_u64(publish_cycles);
		mini_shell_puts(",");
		mini_shell_print_dec_u64(publish_ns);
		mini_shell_puts(",");
		mini_shell_print_dec_u64(normal_cycles);
		mini_shell_puts(",");
		mini_shell_print_dec_u64(normal_ns);
		mini_shell_puts(",");
		mini_shell_print_dec_u64(total_cycles);
		mini_shell_puts(",");
		mini_shell_print_dec_u64(total_ns);
		mini_shell_puts(",");
		mini_shell_print_dec_u64(agl_verify_ns);
		mini_shell_puts(",");
		mini_shell_print_dec_u64(agl_hash_ns);
		mini_shell_puts(",");
		mini_shell_print_hex_u64(status);
		mini_shell_puts("\n");

		if (ret != 0 || status != COMM_HOST_STATUS_OK || ack == 0U) {
			mini_shell_puts("normal attest: normal-world ack failed rc=");
			mini_shell_print_hex_u64((uint64_t)(uint32_t)ret);
			mini_shell_puts(" status=");
			mini_shell_print_hex_u64(status);
			mini_shell_puts("\n");
			return;
		}

		(void)token_buf;
	}

	mini_shell_print_stats("gen_ns", realm_bench_gen_ns, iterations);
	mini_shell_print_stats("publish_ns", realm_bench_publish_ns, iterations);
	mini_shell_print_stats("normal_ack_ns", realm_bench_normal_ns, iterations);
	mini_shell_print_stats("agl_verify_ns", realm_bench_agl_verify_ns, iterations);
	mini_shell_print_stats("total_ns", realm_bench_total_ns, iterations);
}

static bool mini_shell_execute_builtin_command(const char *line)
{
	if (mini_shell_streq(line, "help")) {
		mini_shell_puts("Realm mini shell commands:\n");
		mini_shell_puts("  help          show this message\n");
		mini_shell_puts("  ping          prove command dispatch\n");
		mini_shell_puts("  status        show Realm handoff values\n");
		mini_shell_puts("  attest        request RSI attestation token\n");
		mini_shell_puts("  normal [msg]  send a request to Normal World\n");
		mini_shell_puts("  normal attest [N]  measure token generation + Normal World ack\n");
		mini_shell_puts("Aliases: realm ping, realm status, realm attest, realm normal\n");
		return true;
	}

	if (mini_shell_streq(line, "ping") ||
	    mini_shell_streq(line, "realm ping")) {
		payload_publish_status(REALM_PAYLOAD_PHASE_SHELL_PING,
				       realm_payload_shared_alias_bit,
				       realm_payload_dtb_addr,
				       "mini shell ping");
		mini_shell_puts("pong\n");
		return true;
	}

	if (mini_shell_streq(line, "status") ||
	    mini_shell_streq(line, "realm status")) {
		mini_shell_puts("status_shared=");
		mini_shell_print_hex_u64(realm_payload_status_shared_addr);
		mini_shell_puts(" alias_bit=");
		mini_shell_print_hex_u64(realm_payload_shared_alias_bit);
		mini_shell_puts(" dtb=");
		mini_shell_print_hex_u64(realm_payload_dtb_addr);
		mini_shell_puts("\n");
		return true;
	}

	{
		const char *arg;

		if (mini_shell_parse_arg(line, "normal attest", &arg) ||
		    mini_shell_parse_arg(line, "realm normal attest", &arg)) {
			mini_shell_run_normal_attest_bench(arg);
			return true;
		}

		if (mini_shell_parse_arg(line, "normal", &arg) ||
		    mini_shell_parse_arg(line, "realm normal", &arg)) {
			uint32_t ack = 0U;
			uint32_t status = 0U;
			int ret;

			mini_shell_puts("normal: sending request\n");
			ret = payload_request_normal_world(arg, &ack, &status,
							   NULL, NULL);
			if (ret != 0) {
				mini_shell_puts("normal: no ack rc=");
				mini_shell_print_hex_u64((uint64_t)(uint32_t)ret);
				mini_shell_puts("\n");
				return true;
			}

			mini_shell_puts("normal: ack gen=");
			mini_shell_print_hex_u64(ack);
			mini_shell_puts(" status=");
			mini_shell_print_hex_u64(status);
			mini_shell_puts("\n");
			return true;
		}
	}

	if (mini_shell_streq(line, "attest") ||
	    mini_shell_streq(line, "realm attest")) {
		uint64_t token_size = 0U;
		uintptr_t token_buf = 0U;
		uint64_t cycles = 0U;
		uint64_t ns = 0U;
		int ret;

		mini_shell_puts("attest: requesting token\n");
		ret = payload_generate_attestation(&token_size, &token_buf,
						   &cycles, &ns);
		if (ret != 0) {
			mini_shell_puts("attest: failed rc=");
			mini_shell_print_hex_u64((uint64_t)(uint32_t)ret);
			mini_shell_puts("\n");
			return true;
		}

		mini_shell_puts("attest: token ready size=");
		mini_shell_print_hex_u64(token_size);
		mini_shell_puts(" buffer=");
		mini_shell_print_hex_u64(token_buf);
		mini_shell_puts(" cycles=");
		mini_shell_print_hex_u64(cycles);
		mini_shell_puts(" ns=");
		mini_shell_print_hex_u64(ns);
		mini_shell_puts("\n");
		return true;
	}

	return false;
}

#ifdef CONFIG_SHELL
static void mini_shell_write_buf(const char *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (buf[i] == '\n') {
			mini_shell_putc('\r');
		}
		mini_shell_putc(buf[i]);
	}
}

static void mini_shell_execute_shell_command(const char *line)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();
	size_t out_len = 0U;
	const char *out;
	int ret;

	shell_backend_dummy_clear_output(sh);
	ret = shell_execute_cmd(sh, line);
	out = shell_backend_dummy_get_output(sh, &out_len);
	if (out_len > 0U) {
		mini_shell_write_buf(out, out_len);
		if (out[out_len - 1] != '\n') {
			mini_shell_puts("\n");
		}
	}

	if (ret < 0 && out_len == 0U) {
		mini_shell_puts("shell error rc=");
		mini_shell_print_hex_u64((uint64_t)(uint32_t)ret);
		mini_shell_puts("\n");
	}
}
#endif

static void mini_shell_loop(void)
{
	char line[128];
	size_t len = 0U;
	uint32_t idle_spins = 0U;

	mini_shell_uart_init();
	payload_publish_status_sticky(REALM_PAYLOAD_PHASE_MINI_SHELL_READY,
				      realm_payload_shared_alias_bit,
				      realm_payload_dtb_addr,
				      "mini shell ready");
	mini_shell_uart_late_configure();
	payload_publish_status_sticky(REALM_PAYLOAD_PHASE_MINI_BANNER_BEFORE,
				      realm_payload_shared_alias_bit,
				      realm_payload_dtb_addr,
				      "mini_banner_before");
	payload_publish_status_sticky(REALM_PAYLOAD_PHASE_MINI_CHAR_BEFORE,
				      realm_payload_shared_alias_bit,
				      '>',
				      "mini_char_before");
	mini_shell_putc('>');
	payload_publish_status_sticky(REALM_PAYLOAD_PHASE_MINI_CHAR_AFTER,
				      realm_payload_shared_alias_bit,
				      '>',
				      "mini_char_after");
	mini_shell_puts("\nRealm mini shell ready\n");
	payload_publish_status_sticky(REALM_PAYLOAD_PHASE_MINI_BANNER_AFTER,
				      realm_payload_shared_alias_bit,
				      realm_payload_dtb_addr,
				      "mini_banner_after");
	mini_shell_puts(REALM_MINI_SHELL_PROMPT);
	payload_publish_status_sticky(REALM_PAYLOAD_PHASE_MINI_SHELL_PROMPT,
				      realm_payload_shared_alias_bit,
				      realm_payload_dtb_addr,
				      "mini shell prompt shown");

	while (1) {
		char c;

		if (mini_shell_try_getc(&c) != 0) {
			if (len == 0U && ++idle_spins >= REALM_MINI_SHELL_REPROMPT_SPINS) {
				mini_shell_puts("\r");
				mini_shell_puts(REALM_MINI_SHELL_PROMPT);
				idle_spins = 0U;
			}
			continue;
		}
		idle_spins = 0U;

		if (c == '\r' || c == '\n') {
			mini_shell_puts("\n");
			line[len] = '\0';
			if (len > 0U) {
				if (mini_shell_execute_builtin_command(line)) {
					len = 0U;
					mini_shell_puts(REALM_MINI_SHELL_PROMPT);
					continue;
				}
#ifdef CONFIG_SHELL
				mini_shell_execute_shell_command(line);
#else
				mini_shell_puts("shell core disabled\n");
#endif
			}
			len = 0U;
			mini_shell_puts(REALM_MINI_SHELL_PROMPT);
			continue;
		}

		if ((c == '\b' || c == 0x7f) && len > 0U) {
			len--;
			mini_shell_puts("\b \b");
			continue;
		}

		if (c >= 0x20 && c < 0x7f && len < sizeof(line) - 1U) {
			line[len++] = c;
			mini_shell_putc(c);
		}
	}
}
#endif

static int payload_init_pre_kernel_1(void)
{
	payload_publish_status(REALM_PAYLOAD_PHASE_INIT_PRE_KERNEL_1,
			       realm_payload_shared_alias_bit,
			       realm_payload_dtb_addr,
			       "init_pre_kernel_1");
	return 0;
}

static int payload_init_pre_kernel_2(void)
{
	payload_publish_status(REALM_PAYLOAD_PHASE_INIT_PRE_KERNEL_2,
			       realm_payload_shared_alias_bit,
			       realm_payload_dtb_addr,
			       "init_pre_kernel_2");
	return 0;
}

static int payload_init_after_intc(void)
{
	payload_publish_status(REALM_PAYLOAD_PHASE_INIT_AFTER_INTC,
			       realm_payload_shared_alias_bit,
			       realm_payload_dtb_addr,
			       "init_after_intc");
	return 0;
}

static int payload_init_after_serial(void)
{
	payload_publish_status(REALM_PAYLOAD_PHASE_INIT_AFTER_SERIAL,
			       realm_payload_shared_alias_bit,
			       realm_payload_dtb_addr,
			       "init_after_serial");
	return 0;
}

static int payload_init_post_kernel(void)
{
	payload_publish_status(REALM_PAYLOAD_PHASE_INIT_POST_KERNEL,
			       realm_payload_shared_alias_bit,
			       realm_payload_dtb_addr,
			       "init_post_kernel");
	payload_uart_smoke();
	return 0;
}

static int payload_init_application(void)
{
	payload_publish_status(REALM_PAYLOAD_PHASE_INIT_APPLICATION,
			       realm_payload_shared_alias_bit,
			       realm_payload_dtb_addr,
			       "init_application");
	return 0;
}

SYS_INIT(payload_init_pre_kernel_1, PRE_KERNEL_1, 0);
SYS_INIT(payload_init_after_intc, PRE_KERNEL_1, 45);
SYS_INIT(payload_init_after_serial, PRE_KERNEL_1, 55);
SYS_INIT(payload_init_pre_kernel_2, PRE_KERNEL_2, 0);
SYS_INIT(payload_init_post_kernel, POST_KERNEL, 0);
SYS_INIT(payload_init_application, APPLICATION, 0);

#ifdef CONFIG_SHELL
static int cmd_realm_ping(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	payload_publish_status(REALM_PAYLOAD_PHASE_SHELL_PING,
			       realm_payload_shared_alias_bit,
			       realm_payload_dtb_addr,
			       "shell ping");
	shell_print(sh, "realm pong alias=0x%lx dtb=0x%lx",
		    (unsigned long)realm_payload_shared_alias_bit,
		    (unsigned long)realm_payload_dtb_addr);
	return 0;
}

static int cmd_realm_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "status_shared=0x%lx alias_bit=0x%lx dtb=0x%lx",
		    (unsigned long)realm_payload_status_shared_addr,
		    (unsigned long)realm_payload_shared_alias_bit,
		    (unsigned long)realm_payload_dtb_addr);
	return 0;
}

static int cmd_realm_attest(const struct shell *sh, size_t argc, char **argv)
{
	uint64_t token_size = 0;
	uintptr_t token_buf = 0U;
	uint64_t cycles = 0U;
	uint64_t ns = 0U;
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	ret = payload_generate_attestation(&token_size, &token_buf, &cycles, &ns);
	if (ret != 0) {
		shell_error(sh, "attest failed rc=%d", ret);
		return ret;
	}

	shell_print(sh, "token ready size=0x%lx buffer=0x%lx cycles=0x%lx ns=0x%lx",
		    (unsigned long)token_size,
		    (unsigned long)token_buf,
		    (unsigned long)cycles,
		    (unsigned long)ns);
	return 0;
}

static int cmd_realm_normal(const struct shell *sh, size_t argc, char **argv)
{
	const char *msg = (argc > 1U) ? argv[1] : "";
	uint32_t ack = 0U;
	uint32_t status = 0U;
	int ret;

	ret = payload_request_normal_world(msg, &ack, &status, NULL, NULL);
	if (ret != 0) {
		shell_error(sh, "normal request failed rc=%d", ret);
		return ret;
	}

	shell_print(sh, "normal ack gen=0x%x status=0x%x", ack, status);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(realm_cmds,
	SHELL_CMD(ping, NULL, "Emit a simple Realm shell ping", cmd_realm_ping),
	SHELL_CMD(status, NULL, "Show Realm payload runtime context", cmd_realm_status),
	SHELL_CMD(attest, NULL, "Generate and publish a Realm attestation token", cmd_realm_attest),
	SHELL_CMD(normal, NULL, "Send a request to Normal World", cmd_realm_normal),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(realm, &realm_cmds, "Realm payload commands", NULL);
SHELL_CMD_REGISTER(ping, NULL, "Alias for realm ping", cmd_realm_ping);
SHELL_CMD_REGISTER(status, NULL, "Alias for realm status", cmd_realm_status);
SHELL_CMD_REGISTER(attest, NULL, "Alias for realm attest", cmd_realm_attest);
SHELL_CMD_REGISTER(normal, NULL, "Alias for realm normal", cmd_realm_normal);
#endif

/*
 * Shim-first bring-up payload.
 *
 * The Realm-specific translation is intentionally kept out of the Zephyr
 * kernel and out of this app. This payload is intentionally simple so that
 * the standalone shim owns the Realm boot contract.
 */
int main(void)
{
	printk("[zephyr-payload] main entered\n");
	payload_publish_status(REALM_PAYLOAD_PHASE_MAIN,
			       realm_payload_shared_alias_bit,
			       realm_payload_dtb_addr,
			       "payload main entered");

#ifdef REALM_MINI_SHELL
	mini_shell_loop();
#endif

	while (1) {
		for (volatile uint64_t delay = 0; delay < 50000000ULL; delay++) {
			__asm__ volatile("" ::: "memory");
		}
		printk("[zephyr-payload] heartbeat\n");
		payload_publish_status(REALM_PAYLOAD_PHASE_HB,
				       realm_payload_shared_alias_bit,
				       realm_payload_dtb_addr,
				       "payload heartbeat");
	}

	return 0;
}
