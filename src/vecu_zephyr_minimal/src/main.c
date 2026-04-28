#include <zephyr/sys/printk.h>

static void spin_delay(void)
{
	for (volatile unsigned long i = 0; i < 200000000UL; i++) {
		__asm__ volatile("" ::: "memory");
	}
}

int main(void)
{
	printk("\n");
	printk("========================================\n");
	printk(" Zephyr Realm Minimal Smoke Test\n");
	printk("========================================\n");
	printk("[MIN] entered main()\n");

	while (1) {
		printk("[MIN] heartbeat\n");
		spin_delay();
	}

	return 0;
}
