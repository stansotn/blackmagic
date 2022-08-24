/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2022 1BitSquared <info@1bitsquared.com>
 * Written by Rafael Silva <perigoso@riseup.net>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* This file implements target flash interaction routines.
 * Provides functionality for buffered flash operations
 * It depends on target flash implementations
 */

#include "general.h"
#include "target_internal.h"

static bool flash_buffered_write(target_flash_s *f, target_addr_t dest, const void *src, size_t len);
static bool flash_buffered_flush(target_flash_s *f);

target_flash_s *target_flash_for_addr(target *t, uint32_t addr)
{
	for (target_flash_s *f = t->flash; f; f = f->next)
		if ((f->start <= addr) && (addr < (f->start + f->length)))
			return f;
	return NULL;
}

static bool target_enter_flash_mode(target *t)
{
	if (t->flash_mode)
		return true;

	bool ret = true;
	if (t->enter_flash_mode)
		ret = t->enter_flash_mode(t);
	else
		/* Reset target on flash command */
		/* This saves us if we're interrupted in IRQ context */
		target_reset(t);

	if (ret == true)
		t->flash_mode = true;

	return ret;
}

static bool target_exit_flash_mode(target *t)
{
	if (!t->flash_mode)
		return true;

	bool ret = true;
	if (t->exit_flash_mode)
		ret = t->exit_flash_mode(t);
	else
		/* Reset target to known state when done flashing */
		target_reset(t);

	t->flash_mode = false;

	return ret;
}

static bool flash_prepare(target_flash_s *f)
{
	if (f->ready)
		return true;

	bool ret = true;
	if (f->prepare)
		ret = f->prepare(f);

	if (ret == true)
		f->ready = true;

	return ret;
}

static bool flash_done(target_flash_s *f)
{
	if (!f->ready)
		return true;

	bool ret = true;
	if (f->done)
		ret = f->done(f);

	if (f->buf) {
		free(f->buf);
		f->buf = NULL;
	}

	f->ready = false;

	return ret;
}

bool target_flash_erase(target *t, target_addr_t addr, size_t len)
{
	if (!target_enter_flash_mode(t))
		return false;

	bool ret = true; /* catch false returns with &= */
	while (len) {
		target_flash_s *f = target_flash_for_addr(t, addr);
		if (!f) {
			DEBUG_WARN("Requested address is outside the valid range 0x%06" PRIx32 "\n", addr);
			return false;
		}

		/* terminate flash operations if we're not in the same target flash */
		for (target_flash_s *target_f = t->flash; target_f; target_f = target_f->next)
			if (target_f != f)
				ret &= flash_done(target_f);

		const target_addr_t local_start_addr = addr & ~(f->blocksize - 1U);
		const target_addr_t local_end_addr = local_start_addr + f->blocksize;

		if (!flash_prepare(f))
			return false;

		ret &= f->erase(f, local_start_addr, f->blocksize) == 0;

		len -= MIN(local_end_addr - addr, len);
		addr = local_end_addr;

		/* Issue flash done on last operation */
		if (len == 0)
			ret &= flash_done(f);
	}
	return ret;
}

bool target_flash_write(target *t, target_addr_t dest, const void *src, size_t len)
{
	if (!target_enter_flash_mode(t))
		return false;

	bool ret = true; /* catch false returns with &= */
	while (len) {
		target_flash_s *f = target_flash_for_addr(t, dest);
		if (!f)
			return false;

		/* terminate flash operations if we're not in the same target flash */
		for (target_flash_s *target_f = t->flash; target_f; target_f = target_f->next) {
			if (target_f != f) {
				ret &= flash_buffered_flush(target_f);
				ret &= flash_done(target_f);
			}
		}

		const target_addr_t local_end_addr = MIN(dest + len, f->start + f->length);
		const target_addr_t local_length = local_end_addr - dest;

		ret &= flash_buffered_write(f, dest, src, local_length);

		dest = local_end_addr;
		src += local_length;
		len -= local_length;

		/* Flush operations if we reached the end of Flash */
		if (dest == f->start + f->length) {
			ret &= flash_buffered_flush(f);
			ret &= flash_done(f);
		}
	}
	return ret;
}

bool target_flash_complete(target *t)
{
	if (!t->flash_mode)
		return false;

	bool ret = true; /* catch false returns with &= */
	for (target_flash_s *f = t->flash; f; f = f->next) {
		ret &= flash_buffered_flush(f);
		ret &= flash_done(f);
	}

	target_exit_flash_mode(t);
	return ret;
}

static bool flash_buffered_write(target_flash_s *f, target_addr_t dest, const void *src, size_t len)
{
	if (f->buf == NULL) {
		/* Allocate buffer */
		f->buf = malloc(f->writebufsize);
		if (!f->buf) { /* malloc failed: heap exhaustion */
			DEBUG_WARN("malloc: failed in %s\n", __func__);
			return false;
		}
		f->buf_addr_base = UINT32_MAX;
		f->buf_addr_low = UINT32_MAX;
		f->buf_addr_high = 0;
	}

	bool ret = true; /* catch false returns with &= */
	while (len) {
		const target_addr_t base_addr = dest & ~(f->writebufsize - 1U);

		/* check for base address change */
		if (base_addr != f->buf_addr_base) {
			ret &= flash_buffered_flush(f);

			/* Setup buffer */
			f->buf_addr_base = base_addr;
			memset(f->buf, f->erased, f->writebufsize);
		}

		const size_t offset = dest % f->writebufsize;
		const size_t local_len = MIN(f->writebufsize - offset, len);

		/* Copy chunk into sector buffer */
		memcpy(f->buf + offset, src, local_len);

		/* this allows for writes smaller than blocksize when flushing in the future */
		f->buf_addr_low = MIN(f->buf_addr_low, dest);
		f->buf_addr_high = MAX(f->buf_addr_high, dest + local_len);

		dest += local_len;
		src += local_len;
		len -= local_len;
	}
	return ret;
}

static bool flash_buffered_flush(target_flash_s *f)
{
	bool ret = true; /* catch false returns with &= */
	if (f->buf && f->buf_addr_base != UINT32_MAX && f->buf_addr_low != UINT32_MAX &&
		f->buf_addr_low < f->buf_addr_high) {
		/* Write buffer to flash */

		if (!flash_prepare(f))
			return false;

		target_addr_t aligned_addr = f->buf_addr_low & ~(f->writesize - 1U);

		const uint8_t *src = f->buf + (aligned_addr - f->buf_addr_base);
		uint32_t len = f->buf_addr_high - aligned_addr;

		while (len) {
			ret &= f->write(f, aligned_addr, src, f->writesize) == 0;

			aligned_addr += f->writesize;
			src += f->writesize;
			len -= MIN(len, f->writesize);
		}

		f->buf_addr_base = UINT32_MAX;
		f->buf_addr_low = UINT32_MAX;
		f->buf_addr_high = 0;
	}

	return ret;
}