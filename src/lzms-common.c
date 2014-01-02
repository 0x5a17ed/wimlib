/*
 * lzms-common.c
 *
 * Code shared between the compressor and decompressor for the LZMS compression
 * format.
 */

/*
 * Copyright (C) 2013 Eric Biggers
 *
 * This file is part of wimlib, a library for working with WIM files.
 *
 * wimlib is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.
 *
 * wimlib is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with wimlib; if not, see http://www.gnu.org/licenses/.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "wimlib/endianness.h"
#include "wimlib/error.h"
#include "wimlib/lzms.h"
#include "wimlib/util.h"

#include <pthread.h>

/* A table that maps position slots to their base values.  These are constants
 * computed at runtime by lzms_compute_slot_bases().  */
u32 lzms_position_slot_base[LZMS_MAX_NUM_OFFSET_SYMS + 1];

/* A table that maps length slots to their base values.  These are constants
 * computed at runtime by lzms_compute_slot_bases().  */
u32 lzms_length_slot_base[LZMS_NUM_LEN_SYMS + 1];

/* Return the slot for the specified value.  */
unsigned
lzms_get_slot(u32 value, const u32 slot_base_tab[], unsigned num_slots)
{
	/* TODO:  Speed this up.  */
	unsigned slot = 0;

	while (slot_base_tab[slot + 1] <= value)
		slot++;

	return slot;
}


static void
lzms_decode_delta_rle_slot_bases(u32 slot_bases[],
				 const u8 delta_run_lens[], size_t num_run_lens)
{
	u32 delta = 1;
	u32 base = 0;
	size_t slot = 0;
	for (size_t i = 0; i < num_run_lens; i++) {
		u8 run_len = delta_run_lens[i];
		while (run_len--) {
			base += delta;
			slot_bases[slot++] = base;
		}
		delta <<= 1;
	}
}

/* Initialize the global position and length slot tables.  */
static void
lzms_compute_slot_bases(void)
{
	/* If an explicit formula that maps LZMS position and length slots to
	 * slot bases exists, then it could be used here.  But until one is
	 * found, the following code fills in the slots using the observation
	 * that the increase from one slot base to the next is an increasing
	 * power of 2.  Therefore, run-length encoding of the delta of adjacent
	 * entries can be used.  */
	static const u8 position_slot_delta_run_lens[] = {
		9,   0,   9,   7,   10,  15,  15,  20,
		20,  30,  33,  40,  42,  45,  60,  73,
		80,  85,  95,  105, 6,
	};

	static const u8 length_slot_delta_run_lens[] = {
		27,  4,   6,   4,   5,   2,   1,   1,
		1,   1,   1,   0,   0,   0,   0,   0,
		1,
	};

	lzms_decode_delta_rle_slot_bases(lzms_position_slot_base,
					 position_slot_delta_run_lens,
					 ARRAY_LEN(position_slot_delta_run_lens));

	lzms_position_slot_base[LZMS_MAX_NUM_OFFSET_SYMS] = 0x7fffffff;

	lzms_decode_delta_rle_slot_bases(lzms_length_slot_base,
					 length_slot_delta_run_lens,
					 ARRAY_LEN(length_slot_delta_run_lens));

	lzms_length_slot_base[LZMS_NUM_LEN_SYMS] = 0x400108ab;
}

/* Initialize the global position length slot tables if not done so already.  */
void
lzms_init_slot_bases(void)
{
	static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
	static bool already_computed = false;

	if (unlikely(!already_computed)) {
		pthread_mutex_lock(&mutex);
		if (!already_computed) {
			lzms_compute_slot_bases();
			already_computed = true;
		}
		pthread_mutex_unlock(&mutex);
	}
}

static s32
lzms_maybe_do_x86_translation(u8 data[restrict], s32 i, s32 num_op_bytes,
			      s32 * restrict closest_target_usage_p,
			      s32 last_target_usages[restrict],
			      s32 max_trans_offset, bool undo)
{
	u16 pos;

	if (undo) {
		if (i - *closest_target_usage_p <= max_trans_offset) {
			LZMS_DEBUG("Undid x86 translation at position %d "
				   "(opcode 0x%02x)", i, data[i]);
			le32 *p32 = (le32*)&data[i + num_op_bytes];
			u32 n = le32_to_cpu(*p32);
			*p32 = cpu_to_le32(n - i);
		}
		pos = i + le16_to_cpu(*(const le16*)&data[i + num_op_bytes]);
	} else {
		pos = i + le16_to_cpu(*(const le16*)&data[i + num_op_bytes]);

		if (i - *closest_target_usage_p <= max_trans_offset) {
			LZMS_DEBUG("Did x86 translation at position %d "
				   "(opcode 0x%02x)", i, data[i]);
			le32 *p32 = (le32*)&data[i + num_op_bytes];
			u32 n = le32_to_cpu(*p32);
			*p32 = cpu_to_le32(n + i);
		}
	}

	i += num_op_bytes + sizeof(le32) - 1;

	if (i - last_target_usages[pos] <= LZMS_X86_MAX_GOOD_TARGET_OFFSET)
		*closest_target_usage_p = i;

	last_target_usages[pos] = i;

	return i + 1;
}

static s32
lzms_may_x86_translate(const u8 p[restrict],
		       s32 *restrict max_offset_ret)
{
	/* Switch on first byte of the opcode, assuming it is really an x86
	 * instruction.  */
	*max_offset_ret = LZMS_X86_MAX_TRANSLATION_OFFSET;
	switch (p[0]) {
	case 0x48:
		if (p[1] == 0x8b) {
			if (p[2] == 0x5 || p[2] == 0xd) {
				/* Load relative (x86_64)  */
				return 3;
			}
		} else if (p[1] == 0x8d) {
			if ((p[2] & 0x7) == 0x5) {
				/* Load effective address relative (x86_64)  */
				return 3;
			}
		}
		break;

	case 0x4c:
		if (p[1] == 0x8d) {
			if ((p[2] & 0x7) == 0x5) {
				/* Load effective address relative (x86_64)  */
				return 3;
			}
		}
		break;

	case 0xe8:
		/* Call relative  */
		*max_offset_ret = LZMS_X86_MAX_TRANSLATION_OFFSET / 2;
		return 1;

	case 0xe9:
		/* Jump relative  */
		*max_offset_ret = 0;
		return 5;

	case 0xf0:
		if (p[1] == 0x83 && p[2] == 0x05) {
			/* Lock add relative  */
			return 3;
		}
		break;

	case 0xff:
		if (p[1] == 0x15) {
			/* Call indirect  */
			return 2;
		}
		break;
	}
	*max_offset_ret = 0;
	return 1;
}

/*
 * Translate relative addresses embedded in x86 instructions into absolute
 * addresses (@undo == %false), or undo this translation (@undo == %true).
 *
 * @last_target_usages is a temporary array of length >= 65536.
 */
void
lzms_x86_filter(u8 data[restrict],
		s32 size,
		s32 last_target_usages[restrict],
		bool undo)
{
	s32 closest_target_usage = -LZMS_X86_MAX_TRANSLATION_OFFSET - 1;

	for (s32 i = 0; i < 65536; i++)
		last_target_usages[i] = -LZMS_X86_MAX_GOOD_TARGET_OFFSET - 1;

	for (s32 i = 0; i < size - 11; ) {
		s32 max_trans_offset;
		s32 n;

		n = lzms_may_x86_translate(data + i, &max_trans_offset);
		if (max_trans_offset) {
			i = lzms_maybe_do_x86_translation(data, i, n,
							  &closest_target_usage,
							  last_target_usages,
							  max_trans_offset,
							  undo);
		} else {
			i += n;
		}
	}
}

static void
lzms_init_lz_lru_queues(struct lzms_lz_lru_queues *lz)
{
        /* Recent offsets for LZ matches  */
	for (u32 i = 0; i < LZMS_NUM_RECENT_OFFSETS + 1; i++)
		lz->recent_offsets[i] = i + 1;

	lz->prev_offset = 0;
	lz->upcoming_offset = 0;
}

static void
lzms_init_delta_lru_queues(struct lzms_delta_lru_queues *delta)
{
        /* Recent offsets and powers for LZ matches  */
	for (u32 i = 0; i < LZMS_NUM_RECENT_OFFSETS + 1; i++) {
		delta->recent_offsets[i] = i + 1;
		delta->recent_powers[i] = 0;
	}
	delta->prev_offset = 0;
	delta->prev_power = 0;
	delta->upcoming_offset = 0;
	delta->upcoming_power = 0;
}


void
lzms_init_lru_queues(struct lzms_lru_queues *lru)
{
        lzms_init_lz_lru_queues(&lru->lz);
        lzms_init_delta_lru_queues(&lru->delta);
}

void
lzms_update_lz_lru_queues(struct lzms_lz_lru_queues *lz)
{
	if (lz->prev_offset != 0) {
		for (int i = LZMS_NUM_RECENT_OFFSETS - 1; i >= 0; i--)
			lz->recent_offsets[i + 1] = lz->recent_offsets[i];
		lz->recent_offsets[0] = lz->prev_offset;
	}
	lz->prev_offset = lz->upcoming_offset;
}

void
lzms_update_delta_lru_queues(struct lzms_delta_lru_queues *delta)
{
	if (delta->prev_offset != 0) {
		for (int i = LZMS_NUM_RECENT_OFFSETS - 1; i >= 0; i--) {
			delta->recent_offsets[i + 1] = delta->recent_offsets[i];
			delta->recent_powers[i + 1] = delta->recent_powers[i];
		}
		delta->recent_offsets[0] = delta->prev_offset;
		delta->recent_powers[0] = delta->prev_power;
	}

	delta->prev_offset = delta->upcoming_offset;
	delta->prev_power = delta->upcoming_power;
}

void
lzms_update_lru_queues(struct lzms_lru_queues *lru)
{
        lzms_update_lz_lru_queues(&lru->lz);
        lzms_update_delta_lru_queues(&lru->delta);
}
