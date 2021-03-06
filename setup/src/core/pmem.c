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
#include <pmem.h>
#include <elf.h>
#include <smap.h>
#include <string.h>
#include <pagemem.h>
#include <pool.h>
#include <debug.h>
#include <info_data.h>

extern info_data_t *info;

/*
** cf vmm.h for paging structures
**
** pdp(s) and pd(s) (if any) are pre-allocated
*/
static inline size_t pmem_vmm_pg_size(pg_cnt_t *pg)
{
   return (PML4_SZ + pg->pdp*PDP_SZ + pg->pd*PD64_SZ);
}

static inline offset_t pmem_vmm_pg_alloc(vmm_t *vmm, offset_t area, pg_cnt_t *pg)
{
   vmm->cpu.pg.pml4 = (pml4e_t*)area;
   area += PML4_SZ;

   vmm->cpu.pg.pdp = (pdp_t*)area;
   area += pg->pdp*PDP_SZ;

   if(pg->pd)
   {
      vmm->cpu.pg.pd = (pd64_t*)area;
      area += pg->pd*PD64_SZ;
   }

   return area;
}

/*
** cf vm.h for paging structures
**
** pdp(s) are pre-allocated
** pd(s) and pt(s) will be allocated from the pool
*/
static inline size_t pmem_vm_pg_size(pg_cnt_t *pg)
{
   return (sizeof(npg_pml4_t) + pg->pdp*sizeof(npg_pdp_t));
}

static inline size_t pmem_vm_pg_pool_size(pg_cnt_t *pg)
{
   return (pg->pd*sizeof(npg_pd64_t) + pg->pt*sizeof(npg_pt64_t));
}

static inline offset_t pmem_vm_pg_alloc(vm_t *vm, offset_t area, pg_cnt_t *pg)
{
   vm->cpu.pg.pml4 = (npg_pml4e_t*)area;
   area += sizeof(npg_pml4_t);

   vm->cpu.pg.pdp = (npg_pdp_t*)area;
   area += pg->pdp*sizeof(npg_pdp_t);

   return area;
}

/*
** vmm uses 1GB/2MB pages if possible
** vm uses fine-grain paging
*/
static inline void pmem_pg_predict(pg_cnt_t *vmm, pg_cnt_t *vm)
{
   vmm->pdp = pdp_nr(info->hrd.mem.top - 1) + 1;
   vm->pdp  = vmm->pdp;

   if(info->vmm.cpu.skillz.pg_1G)
      vmm->pd = 0;
   else
      vmm->pd = pd64_nr(info->hrd.mem.top - 1) + 1;

   if(info->vm.cpu.skillz.pg_1G)
      vm->pd = 2;
   else
      vm->pd = vmm->pd;

   vmm->pt = 0;

   if(info->vm.cpu.skillz.pg_2M)
      vm->pt = 2;
   else
      vm->pt = pt64_nr(info->hrd.mem.top - 1) + 1;

   debug(PMEM,
	 "- vmm needs %d pd and %d pt\n"
	 "- vm  needs %d pd and %d pt\n"
	 , vmm->pd, vmm->pt
	 , vm->pd, vm->pt );
}

int pmem_pool_opt_hdl(char *str, void *data)
{
   return dec_to_uint64((uint8_t*)str, strlen(str), (uint64_t*)data);
}

/*
** init the vmm area
*/
void pmem_init(mbi_t *mbi)
{
   info_data_t  *info_r;
   size_t       vmm_elf_sz, smap_sz;
   size_t       pool_sz, pool_opt, pool_desc_sz;
   pg_cnt_t     vmm_pg, vm_pg;
   offset_t     fixed, area;
   module_t     *mod;
   smap_t       *smap;
   vmm_t        *vmm;
   vm_t         *vm;

   if(!(mbi->flags & MBI_FLAG_MMAP))
      panic("no bios smap found");

   if(mbi->mods_count != 2)
      panic("no module found");

   mod  = (module_t*)((offset_t)mbi->mods_addr + sizeof(module_t));
   smap = &info->vm.dev.mem.smap;
   vmm  = &info->vmm;
   vm   = &info->vm;

   /*
   ** 1 - compute some sizes
   */
   smap_parse(mbi, smap, &info->area.end, &info->hrd.mem.top);
   pmem_pg_predict(&vmm_pg, &vm_pg);

   smap_sz = smap->nr*sizeof(smap_e_t);
   vmm_elf_sz = elf_module_load_size(mod);
   pool_sz = pmem_vm_pg_pool_size(&vm_pg);

   if(mbi_get_opt(mod, "pool", pmem_pool_opt_hdl, (void*)&pool_opt))
   {
      debug(PMEM, "increasing pool sz by %D*PAGE_SIZE\n", pool_opt);
      pool_sz += pool_opt*PAGE_SIZE;
   }

   pool_desc_sz = sizeof(pool_pg_desc_t)*(pool_sz/PAGE_SIZE);

   if(!page_aligned(info->area.end))
      info->area.end = page_align(info->area.end);

   fixed = (VMM_MIN_STACK_SIZE
	    + pool_sz
	    + pmem_vmm_pg_size(&vmm_pg)
	    + pmem_vm_pg_size(&vm_pg)
	    + sizeof(vmc_t)
	    + sizeof(vmm_sgmem_t)
	    + vmm_elf_sz + sizeof(long)
	    + smap_sz
	    + pool_desc_sz
	    + sizeof(info_data_t));

   info->area.start = page_align(info->area.end - fixed);
   info->area.size  = info->area.end - info->area.start;
   memset((void*)info->area.start, 0, info->area.size);

   /*
   ** 2 - init vmm area
   */

   /* strictly aligned */
   area = info->area.start;

   area += VMM_MIN_STACK_SIZE;
   vmm->stack_bottom = area;
   vm->cpu.gpr = (gpr64_ctx_t*)vmm->stack_bottom - 1;

   vmm->pool.addr = area;
   vmm->pool.sz = pool_sz;
   area += pool_sz;

   area = pmem_vmm_pg_alloc(vmm, area, &vmm_pg);
   area = pmem_vm_pg_alloc(vm, area, &vm_pg);

   vm->cpu.vmc = (vmc_t*)area;
   area += sizeof(vmc_t);

   vmm->cpu.sg = (vmm_sgmem_t*)area;
   area += sizeof(vmm_sgmem_t);

   /* aligning not required */
   area = long_align_next(area);
   vmm->base = area;
   area += vmm_elf_sz;

   smap->raw = (uint8_t*)area;
   area += smap_sz;

   vmm->pool.all = (pool_pg_desc_t*)area;
   area += pool_desc_sz;

   info_r = (info_data_t*)area;
   area += sizeof(info_data_t);

   /*
   ** 3 - finish setup
   */
   vmm->size  = vmm_elf_sz;
   vmm->entry = vmm->base + elf_module_entry(mod);
   elf_module_load_relocatable(mod, vmm->base);

   /* loaded vmm starts with 'info_data' pointer */
   *(info_data_t**)vmm->base = info_r;

   smap_init(mbi, smap, info->area.start);
   pool_init();

   memcpy((void*)info_r, (void*)info, sizeof(info_data_t));
   info = info_r;

   debug(PMEM,
	 " - area start    = 0x%X\n"
	 " - area end      = 0x%X\n"
	 " - area size     = %D B (%D KB)\n"
	 " - vmm stack     = 0x%X\n"
	 " - vmm pool      = 0x%X (%D KB)\n"
	 " - vmm elf       = 0x%X - 0x%X (%D B)\n"
	 " - gdt           = 0x%X\n"
	 " - idt           = 0x%X\n"
	 " - pml4          = 0x%X\n"
	 " - vm  vmc       = 0x%X\n"
	 ,info->area.start
	 ,info->area.end
	 ,info->area.size, (info->area.size)>>10
	 ,info->vmm.stack_bottom
	 ,info->vmm.pool.addr, (info->vmm.pool.sz)>>10
	 ,info->vmm.base, info->vmm.base+info->vmm.size, info->vmm.size
	 ,info->vmm.cpu.sg->gdt
	 ,info->vmm.cpu.sg->idt
	 ,info->vmm.cpu.pg.pml4
	 ,info->vm.cpu.vmc
      );
}
