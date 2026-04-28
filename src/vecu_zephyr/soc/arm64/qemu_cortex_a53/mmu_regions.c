/*
 * Realm payload variant of the qemu_cortex_a53 SoC.
 *
 * Keep Zephyr kernel sources unmodified by overriding the SoC MMU region
 * table from an out-of-tree SOC_ROOT. All device regions are taken from the
 * payload DTS so Realm can map them through the shared alias addresses.
 */
#include <zephyr/arch/arm64/arm_mmu.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

static const struct arm_mmu_region mmu_regions[] = {
	MMU_REGION_FLAT_ENTRY("GICD",
			      DT_REG_ADDR_BY_IDX(DT_INST(0, arm_gic), 0),
			      DT_REG_SIZE_BY_IDX(DT_INST(0, arm_gic), 0),
			      MT_DEVICE_nGnRnE | MT_P_RW_U_NA |
				      MT_DEFAULT_SECURE_STATE),

	MMU_REGION_FLAT_ENTRY("GICR",
			      DT_REG_ADDR_BY_IDX(DT_INST(0, arm_gic), 1),
			      DT_REG_SIZE_BY_IDX(DT_INST(0, arm_gic), 1),
			      MT_DEVICE_nGnRnE | MT_P_RW_U_NA |
				      MT_DEFAULT_SECURE_STATE),

	MMU_REGION_FLAT_ENTRY("UART0",
			      DT_REG_ADDR(DT_NODELABEL(uart0)),
			      0x1000,
			      MT_DEVICE_nGnRnE | MT_P_RW_U_NA |
				      MT_DEFAULT_SECURE_STATE),
};

const struct arm_mmu_config mmu_config = {
	.num_regions = ARRAY_SIZE(mmu_regions),
	.mmu_regions = mmu_regions,
};
