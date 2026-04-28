#include <zephyr/arch/arm64/arm_mmu.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/cache.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/util.h>
#include <stdint.h>
#include <stdbool.h>

#define REALM_STATUS_MAGIC 0x524d5354UL

#define REALM_PAYLOAD_PHASE_NEW_TABLE_DONE      0x28U
#define REALM_PAYLOAD_PHASE_ZEPHYR_RANGES_DONE  0x29U
#define REALM_PAYLOAD_PHASE_DEVICE_RANGES_DONE  0x2aU
#define REALM_PAYLOAD_PHASE_MMU_SYNC_DONE       0x2bU
#define REALM_PAYLOAD_PHASE_ZEPHYR_RANGE_MAP    0x2cU
#define REALM_PAYLOAD_PHASE_DEVICE_RANGE_MAP    0x2dU
#define REALM_PAYLOAD_PHASE_ZEPHYR_RANGE_CHUNK  0x2eU
#define REALM_PAYLOAD_PHASE_SCRATCH_MAP_BEFORE  0x2fU
#define REALM_PAYLOAD_PHASE_SCRATCH_MAP_AFTER   0x30U
#define REALM_PAYLOAD_PHASE_STATUS_MAP_BEFORE   0x31U
#define REALM_PAYLOAD_PHASE_STATUS_MAP_AFTER    0x32U
#define REALM_PAYLOAD_PHASE_FINAL_MAIR_BEFORE   0x34U
#define REALM_PAYLOAD_PHASE_FINAL_MAIR_AFTER    0x35U
#define REALM_PAYLOAD_PHASE_FINAL_TCR_AFTER     0x36U
#define REALM_PAYLOAD_PHASE_FINAL_TTBR0_AFTER   0x37U
#define REALM_PAYLOAD_PHASE_FINAL_SCTLR_BEFORE  0x38U
#define REALM_PAYLOAD_PHASE_FINAL_SCTLR_AFTER   0x39U
#define REALM_PAYLOAD_PHASE_FINAL_ISB_AFTER     0x3aU
#define REALM_PAYLOAD_PHASE_UART_ALIAS_MAP_BEFORE 0x7cU
#define REALM_PAYLOAD_PHASE_UART_ALIAS_MAP_AFTER  0x7dU

/*
 * Runtime decoding of Zephyr's MMU helper layout.
 *
 * Hardcoding BL offsets repeatedly regressed whenever the payload ELF changed
 * between headless, shell, and mini-shell builds. Keep the kernel_ptables
 * decode fixed for now, but discover the BL targets by scanning the current
 * z_arm64_mm_init() body:
 *   1st BL -> new_table()
 *   2nd BL -> __add_map.constprop.0()
 *   3rd BL -> invalidate_tlb_all()
 */
#define REALM_MMU_CALL_SCAN_LIMIT           0x180UL
#define REALM_MMU_KERNEL_PTABLES_ADRP_OFFSET 0x014UL
#define REALM_MMU_KERNEL_PTABLES_LDR_OFFSET  0x044UL
#define REALM_MMU_SCRATCH_PAGE            0x80400000UL
#define REALM_MMU_UART_ALIAS_BASE         DT_REG_ADDR(DT_NODELABEL(uart0))
#define REALM_MMU_UART_ALIAS_SIZE         0x1000UL
#define REALM_MMU_SHARED_MEM_BASE         0x83ffe000UL
#define REALM_MMU_SHARED_MEM_SIZE         0x2000UL
#define REALM_MMU_DEVICE_ATTRS            (MT_DEVICE_nGnRnE | MT_P_RW_U_NA | \
					   MT_DEFAULT_SECURE_STATE)
#define REALM_MMU_SHARED_ATTRS            (MT_NORMAL | MT_P_RW_U_NA | MT_NS)

struct realm_status_page {
	uint32_t magic;
	uint32_t phase;
	uint32_t heartbeat;
	uint32_t reserved;
	uint64_t value0;
	uint64_t value1;
	char text[64];
};

struct realm_payload_flat_range {
	uintptr_t start;
	uintptr_t end;
	uint32_t attrs;
};

extern char _image_ram_start[];
extern char _image_ram_end[];
extern char __bss_start[];
extern char __text_region_start[];
extern char __text_region_end[];
extern char __rodata_region_start[];
extern char __rodata_region_end[];

extern const struct arm_mmu_config mmu_config;
extern void z_arm64_mm_init(bool is_primary_core);
extern uintptr_t realm_payload_shared_alias_bit;

typedef uint64_t *(*realm_new_table_fn_t)(void);
typedef int (*realm_add_map_fn_t)(uintptr_t phys, uintptr_t virt, size_t size,
				  uint32_t attrs);
typedef void (*realm_invalidate_tlb_all_fn_t)(void);

static inline uint32_t realm_read_insn(uintptr_t addr)
{
	return *(const uint32_t *)addr;
}

static inline uintptr_t realm_decode_bl_target(uintptr_t insn_addr, uint32_t insn)
{
	int32_t imm26 = (int32_t)(insn & 0x03ffffffU);

	if ((imm26 & BIT(25)) != 0) {
		imm26 |= ~0x03ffffff;
	}

	return insn_addr + ((intptr_t)imm26 << 2);
}

static inline uintptr_t realm_decode_adrp_target(uintptr_t insn_addr, uint32_t insn)
{
	int64_t immhi = (int64_t)((insn >> 5) & 0x7ffffU);
	int64_t immlo = (int64_t)((insn >> 29) & 0x3U);
	int64_t imm = (immhi << 2) | immlo;

	if ((imm & BIT64(20)) != 0) {
		imm |= ~((1LL << 21) - 1);
	}

	return (insn_addr & ~0xfffUL) + ((uintptr_t)imm << 12);
}

static inline uintptr_t realm_decode_ldr_uimm64_offset(uint32_t insn)
{
	return (uintptr_t)(((insn >> 10) & 0x0fffU) << 3);
}

static inline bool realm_is_bl(uint32_t insn)
{
	return (insn & 0xfc000000U) == 0x94000000U;
}

static inline uintptr_t realm_find_bl_target(uintptr_t start, size_t scan_limit,
					     unsigned int ordinal)
{
	size_t off;
	unsigned int seen = 0U;

	for (off = 0U; off < scan_limit; off += sizeof(uint32_t)) {
		uintptr_t insn_addr = start + off;
		uint32_t insn = realm_read_insn(insn_addr);

		if (!realm_is_bl(insn)) {
			continue;
		}

		if (seen == ordinal) {
			return realm_decode_bl_target(insn_addr, insn);
		}

		seen++;
	}

	return 0U;
}

static inline realm_new_table_fn_t realm_new_table_fn(void)
{
	return (realm_new_table_fn_t)realm_find_bl_target(
		(uintptr_t)z_arm64_mm_init, REALM_MMU_CALL_SCAN_LIMIT, 0U);
}

static inline realm_add_map_fn_t realm_add_map_fn(void)
{
	return (realm_add_map_fn_t)realm_find_bl_target(
		(uintptr_t)z_arm64_mm_init, REALM_MMU_CALL_SCAN_LIMIT, 1U);
}

static inline realm_invalidate_tlb_all_fn_t realm_invalidate_tlb_all_fn(void)
{
	return (realm_invalidate_tlb_all_fn_t)realm_find_bl_target(
		(uintptr_t)z_arm64_mm_init, REALM_MMU_CALL_SCAN_LIMIT, 2U);
}

static inline struct arm_mmu_ptables *realm_kernel_ptables(void)
{
	uintptr_t adrp_addr = (uintptr_t)z_arm64_mm_init +
			      REALM_MMU_KERNEL_PTABLES_ADRP_OFFSET;
	uintptr_t ldr_addr = (uintptr_t)z_arm64_mm_init +
			     REALM_MMU_KERNEL_PTABLES_LDR_OFFSET;
	uintptr_t page_base = realm_decode_adrp_target(adrp_addr,
						       realm_read_insn(adrp_addr));
	uintptr_t ldr_off = realm_decode_ldr_uimm64_offset(
		realm_read_insn(ldr_addr));

	return (struct arm_mmu_ptables *)(page_base + ldr_off);
}

static inline void realm_publish_status(volatile struct realm_status_page *page,
					uint32_t phase, uint64_t value0,
					uint64_t value1, const char *text)
{
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

static inline void realm_inv_dcache_after_map(uintptr_t virt, size_t size,
					      uint32_t attrs)
{
	if ((attrs & MT_RW) != MT_RW) {
		return;
	}

	if (MT_TYPE(attrs) == MT_NORMAL || MT_TYPE(attrs) == MT_NORMAL_WT) {
		sys_cache_data_invd_range((void *)virt, size);
	}
}

static inline uint64_t realm_tcr_ps_bits(void)
{
#if defined(CONFIG_ARM64_PA_BITS_48)
	return TCR_PS_BITS_256TB;
#elif defined(CONFIG_ARM64_PA_BITS_44)
	return TCR_PS_BITS_16TB;
#elif defined(CONFIG_ARM64_PA_BITS_42)
	return TCR_PS_BITS_4TB;
#elif defined(CONFIG_ARM64_PA_BITS_40)
	return TCR_PS_BITS_1TB;
#elif defined(CONFIG_ARM64_PA_BITS_36)
	return TCR_PS_BITS_64GB;
#else
	return TCR_PS_BITS_4GB;
#endif
}

static inline uint64_t realm_tcr_el1_value(void)
{
	uint64_t tcr = realm_tcr_ps_bits() << TCR_EL1_IPS_SHIFT;

	tcr |= TCR_EPD1_DISABLE;
	tcr |= TCR_T0SZ(CONFIG_ARM64_VA_BITS);
	tcr |= TCR_TG1_4K | TCR_TG0_4K | TCR_SHARED_INNER |
	       TCR_ORGN_WBWA | TCR_IRGN_WBWA;

	return tcr;
}

void realm_payload_mmu_probe(uintptr_t status_shared_addr)
{
	volatile struct realm_status_page *page =
		(volatile struct realm_status_page *)status_shared_addr;
	struct arm_mmu_ptables *ptables = realm_kernel_ptables();
	realm_new_table_fn_t new_table = realm_new_table_fn();
	realm_add_map_fn_t add_map = realm_add_map_fn();
	realm_invalidate_tlb_all_fn_t invalidate_tlb_all =
		realm_invalidate_tlb_all_fn();
	static const struct realm_payload_flat_range zephyr_ranges[] = {
		{
			.start = (uintptr_t)__rodata_region_start,
			.end = (uintptr_t)__rodata_region_end,
			/*
			 * Diagnostic step: treat the rodata window as RW for now.
			 * The first and second rodata pages both stall when mapped
			 * with RO permissions, so use writable permissions to check
			 * whether the remaining blocker is permission-specific or
			 * address/range-specific.
			 */
			.attrs = MT_NORMAL | MT_P_RW_U_NA |
				 MT_DEFAULT_SECURE_STATE,
		},
		{
			.start = (uintptr_t)__rodata_region_end,
			.end = (uintptr_t)_image_ram_end,
			.attrs = MT_NORMAL | MT_P_RW_U_NA |
				 MT_DEFAULT_SECURE_STATE,
		},
		{
			.start = (uintptr_t)__text_region_start,
			.end = (uintptr_t)__text_region_end,
			.attrs = MT_NORMAL | MT_P_RX_U_RX |
				 MT_DEFAULT_SECURE_STATE,
		},
	};
	unsigned int index;
	uint64_t *base;
	uintptr_t current_text_page =
		((uintptr_t)realm_payload_mmu_probe) & ~(CONFIG_MMU_PAGE_SIZE - 1U);
	uintptr_t first_rodata_page =
		zephyr_ranges[0].start & ~(CONFIG_MMU_PAGE_SIZE - 1U);
	/*
	 * Keep the scratch-map probe out of the payload image entirely.
	 *
	 * Using `_image_ram_end - PAGE_SIZE` accidentally landed inside
	 * Zephyr's noinit stack region (`z_main_stack` / interrupt stacks),
	 * which made the diagnostic ambiguous. Use a fixed unused RAM page
	 * instead so a stall here really points at the generic add_map path.
	 */
		uintptr_t scratch_page = REALM_MMU_SCRATCH_PAGE;
		int ret;

	base = new_table();
	ptables->base_xlat_table = base;
	realm_publish_status(page, REALM_PAYLOAD_PHASE_NEW_TABLE_DONE,
			     (uint64_t)(uintptr_t)base,
			     (uint64_t)(uintptr_t)ptables, "new_table");
	if (base == NULL) {
		return;
	}

	realm_publish_status(page, REALM_PAYLOAD_PHASE_SCRATCH_MAP_BEFORE,
			     scratch_page, (uint64_t)(uintptr_t)__bss_start,
			     "scratch_map_before");
	(void)add_map(scratch_page, scratch_page, CONFIG_MMU_PAGE_SIZE,
		      MT_NORMAL | MT_P_RW_U_NA | MT_DEFAULT_SECURE_STATE);
	realm_publish_status(page, REALM_PAYLOAD_PHASE_SCRATCH_MAP_AFTER,
			     scratch_page, (uint64_t)(uintptr_t)__bss_start,
			     "scratch_map_after");

	/*
	 * Once the MMU is enabled, the payload stub continues to publish
	 * progress markers through the shared status page pointer passed in
	 * from the shim. Map that shared-alias page into the new page tables
	 * now so later phase writes remain valid after TTBR0/SCTLR update.
	 */
	realm_publish_status(page, REALM_PAYLOAD_PHASE_STATUS_MAP_BEFORE,
			     status_shared_addr, status_shared_addr,
			     "status_map_before");
		(void)add_map(status_shared_addr, status_shared_addr,
			      CONFIG_MMU_PAGE_SIZE,
			      MT_NORMAL | MT_P_RW_U_NA | MT_DEFAULT_SECURE_STATE);
		realm_publish_status(page, REALM_PAYLOAD_PHASE_STATUS_MAP_AFTER,
				     status_shared_addr, status_shared_addr,
				     "status_map_after");

		/*
		 * The mini-shell bypasses the Zephyr UART driver and writes
		 * directly to the Realm shared-alias UART. Make that mapping
		 * explicit here so visibility does not depend on whether the
		 * current Kconfig selected a serial driver-backed DT region.
		 */
		realm_publish_status(page, REALM_PAYLOAD_PHASE_UART_ALIAS_MAP_BEFORE,
				     REALM_MMU_UART_ALIAS_BASE,
				     REALM_MMU_UART_ALIAS_SIZE,
				     "uart_alias_map_before");
		ret = add_map(REALM_MMU_UART_ALIAS_BASE, REALM_MMU_UART_ALIAS_BASE,
			      REALM_MMU_UART_ALIAS_SIZE, REALM_MMU_DEVICE_ATTRS);
		realm_publish_status(page, REALM_PAYLOAD_PHASE_UART_ALIAS_MAP_AFTER,
				     REALM_MMU_UART_ALIAS_BASE,
				     (uint64_t)(uint32_t)ret,
				     "uart_alias_map_after");

		if (realm_payload_shared_alias_bit != 0U) {
			uintptr_t shared_alias_base =
				realm_payload_shared_alias_bit | REALM_MMU_SHARED_MEM_BASE;

			(void)add_map(shared_alias_base, shared_alias_base,
				      REALM_MMU_SHARED_MEM_SIZE,
				      REALM_MMU_SHARED_ATTRS);
		}

		for (index = 0U; index < ARRAY_SIZE(zephyr_ranges); index++) {
		uintptr_t start = zephyr_ranges[index].start;
		size_t size = zephyr_ranges[index].end - start;

		if (size != 0U) {
			realm_publish_status(page, REALM_PAYLOAD_PHASE_ZEPHYR_RANGE_MAP,
					     index, start, "map_zephyr_range");
			if (index == 0U || index == 1U || index == 2U) {
				uintptr_t addr;

				for (addr = start; addr < zephyr_ranges[index].end;
				     addr += CONFIG_MMU_PAGE_SIZE) {
					if (index == 0U && addr == first_rodata_page) {
						continue;
					}
					if (index == 2U && addr == current_text_page) {
						continue;
					}
					realm_publish_status(page,
							     REALM_PAYLOAD_PHASE_ZEPHYR_RANGE_CHUNK,
							     index, addr,
							     "map_zephyr_chunk");
					(void)add_map(addr, addr, CONFIG_MMU_PAGE_SIZE,
						      zephyr_ranges[index].attrs);
				}
				if (index == 2U &&
				    current_text_page >= start &&
				    current_text_page < zephyr_ranges[index].end) {
					realm_publish_status(page,
							     REALM_PAYLOAD_PHASE_ZEPHYR_RANGE_CHUNK,
							     index, current_text_page,
							     "map_zephyr_chunk_current");
					(void)add_map(current_text_page, current_text_page,
						      CONFIG_MMU_PAGE_SIZE,
						      zephyr_ranges[index].attrs);
				}
				if (index == 0U &&
				    first_rodata_page >= start &&
				    first_rodata_page < zephyr_ranges[index].end) {
					realm_publish_status(page,
							     REALM_PAYLOAD_PHASE_ZEPHYR_RANGE_CHUNK,
							     index, first_rodata_page,
							     "map_zephyr_chunk_first_rodata");
					(void)add_map(first_rodata_page, first_rodata_page,
						      CONFIG_MMU_PAGE_SIZE,
						      zephyr_ranges[index].attrs);
				}
			} else {
				(void)add_map(start, start, size,
					      zephyr_ranges[index].attrs);
			}
		}
	}

	realm_publish_status(page, REALM_PAYLOAD_PHASE_ZEPHYR_RANGES_DONE,
			     ARRAY_SIZE(zephyr_ranges),
			     (uint64_t)(uintptr_t)&zephyr_ranges[0],
			     "zephyr_ranges");

	for (index = 0U; index < mmu_config.num_regions; index++) {
		const struct arm_mmu_region *region = &mmu_config.mmu_regions[index];

		if (region->size != 0U || region->attrs != 0U) {
			realm_publish_status(page, REALM_PAYLOAD_PHASE_DEVICE_RANGE_MAP,
					     index, region->base_va,
					     "map_device_range");
			(void)add_map(region->base_pa, region->base_va, region->size,
				      region->attrs | MT_NO_OVERWRITE);
		}
	}

	realm_publish_status(page, REALM_PAYLOAD_PHASE_DEVICE_RANGES_DONE,
			     mmu_config.num_regions,
			     (uint64_t)(uintptr_t)mmu_config.mmu_regions,
			     "device_ranges");

	invalidate_tlb_all();

	for (index = 0U; index < ARRAY_SIZE(zephyr_ranges); index++) {
		uintptr_t start = zephyr_ranges[index].start;
		size_t size = zephyr_ranges[index].end - start;

		realm_inv_dcache_after_map(start, size, zephyr_ranges[index].attrs);
	}

	for (index = 0U; index < mmu_config.num_regions; index++) {
		const struct arm_mmu_region *region = &mmu_config.mmu_regions[index];

		realm_inv_dcache_after_map(region->base_va, region->size,
					   region->attrs);
	}

	realm_publish_status(page, REALM_PAYLOAD_PHASE_MMU_SYNC_DONE,
			     (uint64_t)(uintptr_t)ptables->base_xlat_table,
			     mmu_config.num_regions, "mmu_sync");
}

void realm_payload_enable_mmu_final(uintptr_t status_shared_addr)
{
	volatile struct realm_status_page *page =
		(volatile struct realm_status_page *)status_shared_addr;
	struct arm_mmu_ptables *ptables = realm_kernel_ptables();
	uint64_t tcr = realm_tcr_el1_value();
	uint64_t sctlr;

	realm_publish_status(page, REALM_PAYLOAD_PHASE_FINAL_MAIR_BEFORE,
			     (uint64_t)(uintptr_t)ptables,
			     (uint64_t)(uintptr_t)ptables->base_xlat_table,
			     "final_mair_before");
	write_mair_el1(MEMORY_ATTRIBUTES);

	realm_publish_status(page, REALM_PAYLOAD_PHASE_FINAL_MAIR_AFTER,
			     MEMORY_ATTRIBUTES, 0, "final_mair_after");
	write_tcr_el1(tcr);

	realm_publish_status(page, REALM_PAYLOAD_PHASE_FINAL_TCR_AFTER,
			     tcr, 0, "final_tcr_after");
	write_ttbr0_el1((uint64_t)ptables->base_xlat_table);

	realm_publish_status(page, REALM_PAYLOAD_PHASE_FINAL_TTBR0_AFTER,
			     (uint64_t)(uintptr_t)ptables->base_xlat_table,
			     0, "final_ttbr0_after");
	barrier_isync_fence_full();

	sctlr = read_sctlr_el1();
	realm_publish_status(page, REALM_PAYLOAD_PHASE_FINAL_SCTLR_BEFORE,
			     sctlr, SCTLR_M_BIT | SCTLR_C_BIT,
			     "final_sctlr_before");
	write_sctlr_el1(sctlr | SCTLR_M_BIT | SCTLR_C_BIT);

	realm_publish_status(page, REALM_PAYLOAD_PHASE_FINAL_SCTLR_AFTER,
			     read_sctlr_el1(), 0, "final_sctlr_after");
	barrier_isync_fence_full();

	realm_publish_status(page, REALM_PAYLOAD_PHASE_FINAL_ISB_AFTER,
			     read_sctlr_el1(), 0, "final_isb_after");
}
