/*
 * Copyright (C) 2011 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <common.h>
#include <malloc.h>
#include <fastboot.h>
#include <mmc.h>
#include <asm/io.h>

static struct ptable the_ptable;

/* in a board specific file */
struct fbt_partition {
	const char *name;
	unsigned size_kb;
};
extern struct fbt_partition fbt_partitions[];

#include <part.h>

/* a bunch of this is duplicate of part_efi.c and part_efi.h
 * we should consider putting this code in there and layer
 * the partition info.
 */
#define EFI_VERSION 0x00010000
#define EFI_ENTRIES 128
#define EFI_NAMELEN 36

static const u8 partition_type[16] = {
	0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
	0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7,
};

static const u8 random_uuid[16] = {
	0xff, 0x1f, 0xf2, 0xf9, 0xd4, 0xa8, 0x0e, 0x5f,
	0x97, 0x46, 0x59, 0x48, 0x69, 0xae, 0xc3, 0x4e,
};

struct efi_entry {
	u8 type_uuid[16];
	u8 uniq_uuid[16];
	u64 first_lba;
	u64 last_lba;
	u64 attr;
	u16 name[EFI_NAMELEN];
};

struct efi_header {
	u8 magic[8];

	u32 version;
	u32 header_sz;

	u32 crc32;
	u32 reserved;

	u64 header_lba;
	u64 backup_lba;
	u64 first_lba;
	u64 last_lba;

	u8 volume_uuid[16];

	u64 entries_lba;

	u32 entries_count;
	u32 entries_size;
	u32 entries_crc32;
} __attribute__((packed));

struct ptable {
	u8 mbr[512];
	union {
		struct efi_header header;
		u8 block[512];
	};
	struct efi_entry entry[EFI_ENTRIES];
};

static void init_mbr(u8 *mbr, u32 blocks)
{
	mbr[0x1be] = 0x00; // nonbootable
	mbr[0x1bf] = 0xFF; // bogus CHS
	mbr[0x1c0] = 0xFF;
	mbr[0x1c1] = 0xFF;

	mbr[0x1c2] = 0xEE; // GPT partition
	mbr[0x1c3] = 0xFF; // bogus CHS
	mbr[0x1c4] = 0xFF;
	mbr[0x1c5] = 0xFF;

	mbr[0x1c6] = 0x01; // start
	mbr[0x1c7] = 0x00;
	mbr[0x1c8] = 0x00;
	mbr[0x1c9] = 0x00;

	memcpy(mbr + 0x1ca, &blocks, sizeof(u32));

	mbr[0x1fe] = 0x55;
	mbr[0x1ff] = 0xaa;
}

static void start_ptbl(struct ptable *ptbl, unsigned blocks)
{
	struct efi_header *hdr = &ptbl->header;

	memset(ptbl, 0, sizeof(*ptbl));

	init_mbr(ptbl->mbr, blocks - 1);

	memcpy(hdr->magic, "EFI PART", 8);
	hdr->version = EFI_VERSION;
	hdr->header_sz = sizeof(struct efi_header);
	hdr->header_lba = 1;
	hdr->backup_lba = blocks - 1;
	hdr->first_lba = 34;
	hdr->last_lba = blocks - 1;
	memcpy(hdr->volume_uuid, random_uuid, 16);
	hdr->entries_lba = 2;
	hdr->entries_count = EFI_ENTRIES;
	hdr->entries_size = sizeof(struct efi_entry);
}

static void end_ptbl(struct ptable *ptbl)
{
	struct efi_header *hdr = &ptbl->header;
	u32 n;

	n = crc32(0, 0, 0);
	n = crc32(n, (void*) ptbl->entry, sizeof(ptbl->entry));
	hdr->entries_crc32 = n;

	n = crc32(0, 0, 0);
	n = crc32(0, (void*) &ptbl->header, sizeof(ptbl->header));
	hdr->crc32 = n;
}

static int add_ptn(struct ptable *ptbl, u64 first, u64 last, const char *name)
{
	struct efi_header *hdr = &ptbl->header;
	struct efi_entry *entry = ptbl->entry;
	unsigned n;

	if (first < 34) {
		printf("partition '%s' overlaps partition table\n", name);
		return -1;
	}

	if (last > hdr->last_lba) {
		printf("partition '%s' does not fit\n", name);
		return -1;
	}
	for (n = 0; n < EFI_ENTRIES; n++, entry++) {
		if (entry->last_lba)
			continue;
		memcpy(entry->type_uuid, partition_type, 16);
		memcpy(entry->uniq_uuid, random_uuid, 16);
		entry->uniq_uuid[0] = n;
		entry->first_lba = first;
		entry->last_lba = last;
		for (n = 0; (n < EFI_NAMELEN) && *name; n++)
			entry->name[n] = *name++;
		return 0;
	}
	printf("out of partition table entries\n");
	return -1;
}

static int do_format(void)
{
	struct ptable *ptbl = &the_ptable;
	unsigned next;
	int n;
	block_dev_desc_t *dev_desc;
	unsigned long blocks_to_write, result;

	/*
	 * Effectively do an "mmc rescan" to handle the case of a card
	 * being inserted after the system booted.
	 */
	struct mmc *mmc = find_mmc_device(FASTBOOT_MMC_DEVICE_ID);
	if (mmc == NULL) {
		printf("error finding mmc device %d\n", FASTBOOT_MMC_DEVICE_ID);
		return -1;
	}
	if (mmc_init(mmc)) {
		printf("error initializing mmc device %d\n", FASTBOOT_MMC_DEVICE_ID);
		return -1;
	}

	dev_desc = mmc_get_dev(FASTBOOT_MMC_DEVICE_ID);
	if (!dev_desc) {
		printf("error getting mmc device %d\n", FASTBOOT_MMC_DEVICE_ID);
		return -1;
	}
	if (!dev_desc->lba) {
		printf("mmc device %d has no space\n", FASTBOOT_MMC_DEVICE_ID);
		return -1;
	}

	printf("blocks %lu\n", dev_desc->lba);

	start_ptbl(ptbl, dev_desc->lba);
	for (n = 0, next = 0; fbt_partitions[n].name; n++) {
		unsigned sz = fbt_partitions[n].size_kb * 2;
		if (!strcmp(fbt_partitions[n].name,"-")) {
			next += sz;
			continue;
		}
		if (sz == 0)
			sz = dev_desc->lba - next;
		if (add_ptn(ptbl, next, next + sz - 1, fbt_partitions[n].name))
			return -1;
		next += sz;
	}
	end_ptbl(ptbl);

	fbt_reset_ptn();
	blocks_to_write = DIV_ROUND_UP(sizeof(struct ptable), dev_desc->blksz);
	result = dev_desc->block_write(dev_desc->dev, 0, blocks_to_write, ptbl);
	if (result != blocks_to_write) {
		printf("\nFormat failed, block_write() returned %lu instead of %lu\n", result, blocks_to_write);
		return -1;
	}

	printf("\nnew partition table:\n");
	board_fbt_load_ptbl();

	return 0;
}

int board_fbt_oem(const char *cmdbuf)
{
	if (!strcmp(cmdbuf,"format"))
		return do_format();
	return -1;
}

void board_fbt_set_reboot_type(enum fbt_reboot_type frt)
{
	switch(frt) {
	case FASTBOOT_REBOOT_NORMAL:
	  writel(0, (void*)FASTBOOT_REBOOT_PARAMETER_ADDR);
	  break;
	case FASTBOOT_REBOOT_BOOTLOADER:
	  strcpy((char*)FASTBOOT_REBOOT_PARAMETER_ADDR, "bootloader");
	  break;
	case FASTBOOT_REBOOT_RECOVERY:
	  strcpy((char*)FASTBOOT_REBOOT_PARAMETER_ADDR, "recovery");
	  break;
	default:
	  printf("unknown reboot type %d\n", frt);
	  break;
	}
}

enum fbt_reboot_type board_fbt_get_reboot_type(void)
{
	enum fbt_reboot_type frt = FASTBOOT_REBOOT_NORMAL;
	if (!strcmp((const char*)FASTBOOT_REBOOT_PARAMETER_ADDR, "recovery")) {
		frt = FASTBOOT_REBOOT_RECOVERY;
	} else if (!strcmp((const char*)FASTBOOT_REBOOT_PARAMETER_ADDR, "bootloader")) {
		frt = FASTBOOT_REBOOT_BOOTLOADER;
	}
	/* clear before next boot */
	writel(0, (void*)FASTBOOT_REBOOT_PARAMETER_ADDR);
	return frt;
}



static void import_efi_partition(struct efi_entry *entry)
{
	struct fastboot_ptentry e;
	int n;
	if (memcmp(entry->type_uuid, partition_type, sizeof(entry->type_uuid)))
		return;
	for (n = 0; n < (sizeof(e.name)-1); n++)
		e.name[n] = entry->name[n];
	e.name[n] = 0;
	e.start = entry->first_lba;
	e.length = (entry->last_lba - entry->first_lba + 1) * 512;
	e.flags = 0;

	if (!strcmp(e.name,"environment"))
		e.flags |= FASTBOOT_PTENTRY_FLAGS_WRITE_ENV;
	fbt_add_ptn(&e);

	if (e.length > 0x100000)
		printf(" %8d  %12u(%7dM)  %s\n", e.start, e.length, e.length/0x100000, e.name);
	else
		printf(" %8d  %12u(%7dK)  %s\n", e.start, e.length, e.length/0x400, e.name);
}

int board_fbt_load_ptbl(void)
{
	static struct efi_entry *entry;
	void *data;
	int n,m;
	int res = -1;
	struct mmc *mmc = find_mmc_device(FASTBOOT_MMC_DEVICE_ID);
	if (mmc == NULL) {
		printf("error finding mmc device %d\n", FASTBOOT_MMC_DEVICE_ID);
		return -1;
	}
	mmc_init(mmc);
	data = malloc(mmc->block_dev.blksz);
	if (data == NULL) {
		printf("error allocating blksz(%lu) buffer\n", mmc->block_dev.blksz);
		return -1;
	}
	res = mmc->block_dev.block_read(FASTBOOT_MMC_DEVICE_ID, 1, 1, data);
	if (res != 1) {
		printf("error reading partition table\n");
		res = -1;
		goto out;
	}
	if (memcmp(data, "EFI PART", 8)) {
		printf("efi partition table not found\n");
		goto out;
	}
	entry = data;
	printf("lba_start      partition_size          name\n");
	printf("=========  ======================  ==============\n");
	for (n = 0; n < (128/4); n++) {
		res = mmc->block_dev.block_read(FASTBOOT_MMC_DEVICE_ID, n+1, 1, data);
		if (res != 1) {
			printf("partition read failed\n");
			res = -1;
			goto out;
		}
		for (m = 0; m < 4; m ++)
			import_efi_partition(entry + m);
	}
	printf("=========  ======================  ==============\n");
	res = 0;
 out:
	free(data);
	return res;
}
