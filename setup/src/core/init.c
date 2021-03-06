/*
** Copyright (C) 2011 EADS France, stephane duverger <stephane.duverger@eads.net>
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License along
** with this program; if not, write to the Free Software Foundation, Inc.,
** 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#include <init.h>
#include <dev.h>
#include <cpu.h>
#include <pmem.h>
#include <vmem.h>
#include <intr.h>
#include <vmm.h>
#include <info_data.h>

static info_data_t __info;
       info_data_t *info = &__info;

void __regparm__(1) init(mbi_t *mbi)
{
   dev_init();
   cpu_init();
   pmem_init(mbi);
   vmem_init();
   intr_init();
   vmm_init();
}
