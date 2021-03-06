/*
 * Copyright (c) 2016-2019, NVIDIA CORPORATION.
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <stdlib.h>
#include <common.h>
#include <linux/ctype.h>
#include <fdt_support.h>
#include <fdtdec.h>
#include <asm/arch/tegra.h>
#include <asm/arch-tegra/cboot.h>
#include <asm/armv8/mmu.h>

DECLARE_GLOBAL_DATA_PTR;

char *strstrip(char *s)
{
    size_t size;
    char *end;

    size = strlen(s);

    if (!size)
        return s;

    end = s + size - 1;
    while (end >= s && isblank(*end))
        end--;
    *(end + 1) = '\0';

    while (*s && isblank(*s))
        s++;

    return s;
}

/*
 * The following few functions run late during the boot process and dynamically
 * calculate the load address of various binaries. To keep track of multiple
 * allocations, some writable list of RAM banks must be used. tegra_mem_map[]
 * is used for this purpose to avoid making yet another copy of the list of RAM
 * banks. This is safe because tegra_mem_map[] is only used once during very
 * early boot to create U-Boot's page tables, long before this code runs. If
 * this assumption becomes invalid later, we can just fix the code to copy the
 * list of RAM banks into some private data structure before running.
 */

extern struct mm_region tegra_mem_map[];

static char *gen_varname(const char *var, const char *ext)
{
	size_t len_var = strlen(var);
	size_t len_ext = strlen(ext);
	size_t len = len_var + len_ext + 1;
	char *varext = malloc(len);

	if (!varext)
		return 0;
	strcpy(varext, var);
	strcpy(varext + len_var, ext);
	return varext;
}

static void mark_ram_allocated(int bank, u64 allocated_start, u64 allocated_end)
{
	u64 bank_start = tegra_mem_map[bank].base;
	u64 bank_size = tegra_mem_map[bank].size;
	u64 bank_end = bank_start + bank_size;
	bool keep_front = allocated_start != bank_start;
	bool keep_tail = allocated_end != bank_end;

	if (keep_front && keep_tail) {
		/*
		 * There are CONFIG_NR_DRAM_BANKS DRAM entries in the array,
		 * starting at index 1 (index 0 is MMIO). So, we are at DRAM
		 * entry "bank" not "bank - 1" as for a typical 0-base array.
		 * The number of remaining DRAM entries is therefore
		 * "CONFIG_NR_DRAM_BANKS - bank". We want to duplicate the
		 * current entry and shift up the remaining entries, dropping
		 * the last one. Thus, we must copy one fewer entry than the
		 * number remaining.
		 */
		memmove(&tegra_mem_map[bank + 1], &tegra_mem_map[bank],
			CONFIG_NR_DRAM_BANKS - bank - 1);
		tegra_mem_map[bank].size = allocated_start - bank_start;
		bank++;
		tegra_mem_map[bank].base = allocated_end;
		tegra_mem_map[bank].size = bank_end - allocated_end;
	} else if (keep_front) {
		tegra_mem_map[bank].size = allocated_start - bank_start;
	} else if (keep_tail) {
		tegra_mem_map[bank].base = allocated_end;
		tegra_mem_map[bank].size = bank_end - allocated_end;
	} else {
		/*
		 * We could move all subsequent banks down in the array but
		 * that's not necessary for subsequent allocations to work, so
		 * we skip doing so.
		 */
		tegra_mem_map[bank].size = 0;
	}
}

static void reserve_ram(u64 start, u64 size)
{
	int bank;
	u64 end = start + size;

	for (bank = 1; bank <= CONFIG_NR_DRAM_BANKS; bank++) {
		u64 bank_start = tegra_mem_map[bank].base;
		u64 bank_size = tegra_mem_map[bank].size;
		u64 bank_end = bank_start + bank_size;

		if (end <= bank_start || start > bank_end)
			continue;
		mark_ram_allocated(bank, start, end);
		break;
	}
}

static u64 alloc_ram(u64 size, u64 align, u64 offset)
{
	int bank;

	for (bank = 1; bank <= CONFIG_NR_DRAM_BANKS; bank++) {
		u64 bank_start = tegra_mem_map[bank].base;
		u64 bank_size = tegra_mem_map[bank].size;
		u64 bank_end = bank_start + bank_size;
		u64 allocated = ROUND(bank_start, align) + offset;
		u64 allocated_end = allocated + size;

		if (allocated_end > bank_end)
			continue;
		mark_ram_allocated(bank, allocated, allocated_end);
		return allocated;
	}
	return 0;
}

static void set_calculated_aliases(char *aliases, u64 address)
{
	char *tmp, *alias;
	int err;

	aliases = strdup(aliases);
	if (!aliases) {
		error("strdup(aliases) failed");
		return;
	}

	tmp = aliases;
	while (true) {
		alias = strsep(&tmp, " ");
		if (!alias)
			break;
		debug("%s: alias: %s\n", __func__, alias);
		err = setenv_hex(alias, address);
		if (err)
			error("Could not set %s\n", alias);
	}

	free(aliases);
}

static void set_calculated_env_var(const char *var)
{
	char *var_size;
	char *var_align;
	char *var_offset;
	char *var_aliases;
	u64 size;
	u64 align;
	u64 offset;
	char *aliases;
	u64 address;
	int err;

	var_size = gen_varname(var, "_size");
	if (!var_size)
		return;
	var_align = gen_varname(var, "_align");
	if (!var_align)
		goto out_free_var_size;
	var_offset = gen_varname(var, "_offset");
	if (!var_offset)
		goto out_free_var_align;
	var_aliases = gen_varname(var, "_aliases");
	if (!var_aliases)
		goto out_free_var_offset;

	size = getenv_hex(var_size, 0);
	if (!size) {
		error("%s not set or zero\n", var_size);
		goto out_free_var_aliases;
	}
	align = getenv_hex(var_align, 1);
	/* Handle extant variables, but with a value of 0 */
	if (!align)
		align = 1;
	offset = getenv_hex(var_offset, 0);
	aliases = getenv(var_aliases);

	debug("%s: Calc var %s; size=%llx, align=%llx, offset=%llx\n",
	      __func__, var, size, align, offset);
	if (aliases)
		debug("%s: Aliases: %s\n", __func__, aliases);

	address = alloc_ram(size, align, offset);
	if (!address) {
		error("Could not allocate %s\n", var);
		goto out_free_var_aliases;
	}
	debug("%s: Address %llx\n", __func__, address);

	err = setenv_hex(var, address);
	if (err)
		error("Could not set %s\n", var);
	if (aliases)
		set_calculated_aliases(aliases, address);

out_free_var_aliases:
	free(var_aliases);
out_free_var_offset:
	free(var_offset);
out_free_var_align:
	free(var_align);
out_free_var_size:
	free(var_size);
}

#ifdef DEBUG
static void dump_ram_banks(void)
{
	int bank;

	for (bank = 1; bank <= CONFIG_NR_DRAM_BANKS; bank++) {
		u64 bank_start = tegra_mem_map[bank].base;
		u64 bank_size = tegra_mem_map[bank].size;
		u64 bank_end = bank_start + bank_size;

		if (!bank_size)
			continue;
		printf("%d: %010llx..%010llx (+%010llx)\n", bank - 1,
		       bank_start, bank_end, bank_size);
	}
}
#endif

static void set_calculated_env_vars(void)
{
	char *vars, *tmp, *var;

#ifdef DEBUG
	printf("RAM banks before any calculated env. var.s:\n");
	dump_ram_banks();
#endif

	reserve_ram(cboot_boot_x0, fdt_totalsize(cboot_boot_x0));

#ifdef DEBUG
	printf("RAM after reserving cboot DTB:\n");
	dump_ram_banks();
#endif

	vars = getenv("calculated_vars");
	if (!vars) {
		debug("%s: No env var calculated_vars\n", __func__);
		return;
	}

	vars = strdup(vars);
	if (!vars) {
		error("strdup(calculated_vars) failed");
		return;
	}

	tmp = vars;
	while (true) {
		var = strsep(&tmp, " ");
		if (!var)
			break;
		debug("%s: var: %s\n", __func__, var);
		set_calculated_env_var(var);
#ifdef DEBUG
		printf("RAM banks affter allocating %s:\n", var);
		dump_ram_banks();
#endif
	}

	free(vars);
}

static int set_fdt_addr(void)
{
	int ret;

	ret = setenv_hex("fdt_addr", cboot_boot_x0);
	if (ret) {
		printf("Failed to set fdt_addr to point at DTB: %d\n", ret);
		goto fail;
	}

	ret = setenv_hex("fdt_copy_src_addr", cboot_boot_x0);
	if (ret) {
		printf("Failed to set fdt_copy_src_addr to point at DTB: %d\n",
			ret);
		goto fail;
	}
	ret = 0;
fail:
	return ret;
}

__weak int set_ethaddr_from_cboot(const void *fdt)
{
	return 0;
}

static int set_cbootargs(void)
{
	const void *cboot_blob = (void *)cboot_boot_x0;
	const void *prop;
	char *bargs, *s;
	int node, len, ret = 0;

	/*
	 * Save the bootargs passed in the DTB by the previous bootloader
	 * (CBoot) to the env. (pointer in reg x0)
	 */

	debug("%s: cboot_blob = %p\n", __func__, cboot_blob);

	node = fdt_path_offset(cboot_blob, "/chosen");
	if (node < 0) {
		error("Can't find /chosen node in cboot DTB");
		return node;
	}
	debug("%s: found 'chosen' node: %d\n", __func__, node);

	prop = fdt_getprop(cboot_blob, node, "bootargs", &len);
	if (!prop) {
		error("Can't find /chosen/bootargs property in cboot DTB");
		return -ENOENT;
	}
	debug("%s: found 'bootargs' property, len =%d\n",  __func__, len);

	/* CBoot seems to add trailing whitespace - strip it here */
	s = strdup((char *)prop);
	bargs = strstrip(s);
	debug("%s: bootargs = %s!\n", __func__, bargs);

        /* Set cbootargs to env for later use by extlinux files */
	ret = setenv("cbootargs", bargs);
	if (ret)
		printf("Failed to set cbootargs from cboot DTB: %d\n", ret);

	free(s);
	return ret;
}

int cboot_init_late(void)
{
	const void *fdt = (void *)cboot_boot_x0;

	set_calculated_env_vars();
	/*
	 * Ignore errors here; the value may not be used depending on
	 * extlinux.conf or boot script content.
	 */
	set_fdt_addr();
	/* Ignore errors here; not all cases care about Ethernet addresses */
	set_ethaddr_from_cboot(fdt);
	/* Save CBoot bootargs to env */
	set_cbootargs();

	return 0;
}
