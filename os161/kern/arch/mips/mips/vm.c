#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <curthread.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/spl.h>
#include <machine/tlb.h>
#include <vfs.h>
#include <kern/unistd.h>
#include <bitmap.h>
#include <elf.h>
#include <uio.h>
#include <vnode.h>
#include <kern/stat.h>
/*****************************************************************************************/
#define PTE_PRESENT 0x00000800
#define PTE_SWAPPED 0x00000400
#define PTE_UNSET_PRESENT 0xfffff7ff
#define PTE_UNSET_SWAPPED 0xfffffbff
/********************************** Extern Declarations***********************************/
extern struct thread* curthread;

/*********************************** The Coremap *****************************************/
frame* coremap;
size_t num_frames;
size_t num_fixed_page;
/*********************************** Swap file *******************************************/
struct vnode * swap_file;
struct bitmap* swapfile_map;
/**************************** Convenience Function ***************************************/
void swap_out(int frame_id, off_t pos);

void load_page(struct addrspace* addrspace, vaddr_t vaddr, int frame_id);

u_int32_t* get_PTE (struct thread*, vaddr_t va);
u_int32_t* get_PTE_from_addrspace (struct addrspace*, vaddr_t va);
int get_free_frame_kernel();
/********************************* Some bookkeepping data*********************************/
int vm_bootstraped = 0;
int total_disk_slots;
/*****************************************************************************************/
paddr_t load_swapped_page(struct addrspace* as, vaddr_t va);
int get_free_frame();

void swapping_init(){
	// for swapping subsystem
	int spl = splhigh();
	int err = vfs_open("lhd1raw:", O_RDWR, &swap_file);
	if (err) {
		panic("vfs_open on lhad1 failed");
	}
	struct stat stat;
	VOP_STAT(swap_file, &stat);
	total_disk_slots = stat.st_size / PAGE_SIZE;
	swapfile_map = bitmap_create(total_disk_slots);
	splx(spl);
}

paddr_t
getppages(unsigned long npages)
{
	int spl;
	paddr_t addr;

	spl = splhigh();

	addr = ram_stealmem(npages);
	
	splx(spl);
	return addr;
}


void
vm_bootstrap(void)
{
	u_int32_t start, end, start_adjusted;

	size_t num_pages = 0;
	// get the amount of physical memory
	ram_getsize(&start, &end);
	// align start
	start = ROUNDUP(start, PAGE_SIZE);

	num_pages = ((end - start)/ PAGE_SIZE);
	// kernel always looks at virtual address only
	coremap = (frame *)PADDR_TO_KVADDR(start);
	/************************************** ALLOCATE SPACE *****************************************/
	// first the coremap
	start_adjusted = start + num_pages * sizeof(frame); 
	/************************************** initialize coremap *************************************/
	// these fixed pages are for the coremap
	int fixed_pages = (start_adjusted - start) / PAGE_SIZE + 1;
	int free_pages = num_pages - fixed_pages;
	int i = 0;
	for (; i < num_pages; ++i) {
		if(i < fixed_pages) {
			coremap[i].addrspace = NULL;
			coremap[i].frame_id = i;
			coremap[i].frame_start = start + PAGE_SIZE * i;
			// the fixed pages are mapped to kernel addr space, i.e. kernel pages have fixed mapping
			coremap[i].mapped_vaddr = PADDR_TO_KVADDR(coremap[i].frame_start);
			coremap[i].state = FIXED;
			coremap[i].num_pages_allocated = 1;
		} else {
			// free pages 
			coremap[i].addrspace = NULL;
			coremap[i].frame_id = i;
			coremap[i].frame_start = start + PAGE_SIZE * i;
			coremap[i].mapped_vaddr = 0xDEADBEEF; // LOL
			coremap[i].state = FREE;
			coremap[i].num_pages_allocated = 0;			
		}
	}
	// globals
	num_frames = num_pages;
	num_fixed_page = fixed_pages;
	// sanity check
	for (i = 0; i < num_frames; i++) {
		if ((coremap[i].state != FREE && coremap[i].state != FIXED) || ((coremap[i].frame_start % PAGE_SIZE) != 0)) 
			panic("error initializing the coremap"); 
	}
	/**************************************** END of init ******************************************/
	// TODO: we may want to set some flags to indicate that vm has already bootstrapped, 
	vm_bootstraped = 1;
	// TODO: start the paging thread below
}


/*
	Function to get a pointer to the PTE corresponding to the virtual address va.
	Note, a PTE is uniquely identified among all processes by a tuple: <addrspace, vaddress>
*/
u_int32_t* get_PTE(struct thread* addrspace_owner, vaddr_t va) {
	return get_PTE_from_addrspace(addrspace_owner->t_vmspace, va);
}
u_int32_t* get_PTE_from_addrspace (struct addrspace* as, vaddr_t va){
	assert(curspl > 0);
	int level1_index = (va & FIRST_LEVEL_PN) >> 22; 
	int level2_index = (va & SEC_LEVEL_PN) >> 12;
	struct as_pagetable* level2_pagetable = as->as_master_pagetable[level1_index];
	// level2 pagetable shall not be null? TODO
	// assert(level2_pagetable != NULL);
	if (level2_pagetable == NULL) 
		return NULL;
	else 
		return &(level2_pagetable->PTE[level2_index]);
}


int evict_or_swap_with_avoidance(paddr_t avoid){
	assert(curspl > 0);
	int kicked_ass_page;
	do{
		kicked_ass_page = random() % num_frames;
	} while (coremap[kicked_ass_page].state == FIXED 
				|| coremap[kicked_ass_page].frame_start == avoid
				|| coremap[kicked_ass_page].state == FREE);

	int disk_slot;
	if (coremap[kicked_ass_page].state == DIRTY) {
		// page is dirty, swap out :)
		bitmap_alloc(swapfile_map, &disk_slot);
		// cannot exceed total_disk_slots pages
		assert(disk_slot < total_disk_slots);
		off_t disk_addr = disk_slot * PAGE_SIZE;
		swap_out(kicked_ass_page, disk_addr);
	} 
	/********************************* Update PTE **********************************/
	u_int32_t *pte = get_PTE_from_addrspace(coremap[kicked_ass_page].addrspace,
											coremap[kicked_ass_page].mapped_vaddr);
	assert((*pte & PTE_PRESENT) != 0);
	*pte |= PTE_SWAPPED;
	*pte &= PTE_UNSET_PRESENT;
	if (coremap[kicked_ass_page].state == DIRTY) {
		*pte &= CLEAR_PAGE_FRAME;
		*pte |= (disk_slot << 12);
	}
	/********************************* Coremap Entry *******************************/
	coremap[kicked_ass_page].state = CLEAN;
	coremap[kicked_ass_page].mapped_vaddr = 0xDEADBEEF;
	coremap[kicked_ass_page].addrspace = NULL;
	return kicked_ass_page;
	
}

int evict_or_swap_kernel(){
	assert(curspl > 0);
	int kicked_ass_page;
	int i = 0;
	for (; i < num_frames; i++){
		if(coremap[i].state != FIXED) {
			kicked_ass_page = i;
			break;
		}
	}

	int disk_slot;
	if (coremap[kicked_ass_page].state == DIRTY) {
		// page is dirty, swap out :)
		bitmap_alloc(swapfile_map, &disk_slot);
		// cannot exceed total_disk_slots pages
		assert(disk_slot < total_disk_slots);
		off_t disk_addr = disk_slot * PAGE_SIZE;
		swap_out(kicked_ass_page, disk_addr);
	} 
	/********************************* Update PTE **********************************/
	u_int32_t *pte = get_PTE_from_addrspace(coremap[kicked_ass_page].addrspace,
											coremap[kicked_ass_page].mapped_vaddr);
	assert((*pte & PTE_PRESENT) != 0);
	*pte |= PTE_SWAPPED;
	*pte &= PTE_UNSET_PRESENT;
	if (coremap[kicked_ass_page].state == DIRTY) {
		*pte &= CLEAR_PAGE_FRAME;
		*pte |= (disk_slot << 12);
	}
	/********************************* Coremap Entry *******************************/
	coremap[kicked_ass_page].state = CLEAN;
	coremap[kicked_ass_page].mapped_vaddr = 0xDEADBEEF;
	coremap[kicked_ass_page].addrspace = NULL;
	return kicked_ass_page;
	
}

/*
	Function that makes room for a single page
	Page replacement policy (experimental) -> randomly evict a page
	Depending on the state of the page, we either
		** evict the page, should the page be clean,
		or
	 	** swap to disk, should the page be dirty
	@precondtion: no free pages in coremap
	@return the id of the evict/swapped frame
	TODO: an optimization: find clean pages if possible.
	NOTE: updates the evicted/swapped page's coremap entry and pte
*/
int evict_or_swap(){
	assert(curspl > 0);
	int kicked_ass_page;
	do{
		kicked_ass_page = random() % num_frames;
	} while (coremap[kicked_ass_page].state == FIXED 
				|| coremap[kicked_ass_page].state == FREE);

	int disk_slot;
	if (coremap[kicked_ass_page].state == DIRTY) {
		// page is dirty, swap out :)
		bitmap_alloc(swapfile_map, &disk_slot);
		// cannot exceed total_disk_slots pages
		assert(disk_slot < total_disk_slots);
		off_t disk_addr = disk_slot * PAGE_SIZE;
		swap_out(kicked_ass_page, disk_addr);
	} 
	/********************************* Update PTE **********************************/
	u_int32_t *pte = get_PTE_from_addrspace(coremap[kicked_ass_page].addrspace,
											coremap[kicked_ass_page].mapped_vaddr);
	assert((*pte & PTE_PRESENT) != 0);
	*pte |= PTE_SWAPPED;
	*pte &= PTE_UNSET_PRESENT;
	if (coremap[kicked_ass_page].state == DIRTY) {
		*pte &= CLEAR_PAGE_FRAME;
		*pte |= (disk_slot << 12);
	}
	/********************************* Coremap Entry *******************************/
	coremap[kicked_ass_page].state = CLEAN;
	coremap[kicked_ass_page].mapped_vaddr = 0xDEADBEEF;
	coremap[kicked_ass_page].addrspace = NULL;
	return kicked_ass_page;
	
}

/*
	Same as alloc_page_userspace, but it avoids touching the specified page
*/
paddr_t alloc_page_userspace_with_avoidance(struct addrspace * as, vaddr_t va, paddr_t avoid) {
	assert(curspl > 0);
	// passed in virtual address shall be page-aligned
	assert((va & PAGE_FRAME) == va);
	int i = 0;
	int kicked_ass_page = -1;
	// go through the coremap check if there's a free page
	for (; i < num_frames; i++) {
		if (coremap[i].state == FREE) {
			kicked_ass_page = i;
			break;
		}
	}
	if (kicked_ass_page == -1) {
		// no free page available right now --> we need to evict/swap a page that
		// does not have paddr of avoid
		kicked_ass_page = evict_or_swap_with_avoidance(avoid);
	}
	assert(kicked_ass_page >= 0);
	assert(coremap[kicked_ass_page].state == FREE || coremap[kicked_ass_page].state == CLEAN);
	// now update coremap entry
	coremap[kicked_ass_page].addrspace = as;
	coremap[kicked_ass_page].state = DIRTY; 
	coremap[kicked_ass_page].mapped_vaddr = va;
	coremap[kicked_ass_page].num_pages_allocated = 1;
	return coremap[kicked_ass_page].frame_start;
}

/*
	Same as alloc_one_page, but sets the coremap entries as user space
	** Only responsible for updating the coremap entries, not touching 
	the PTEs, it is the caller's responsibility to update PTEs where appropriate
*/
paddr_t alloc_page_userspace(vaddr_t va) {
	assert(curspl > 0);
	// passed in virtual address shall be page-aligned
	assert((va & PAGE_FRAME) == va);
	int kicked_ass_page = get_free_frame();

	assert(coremap[kicked_ass_page].state == FREE || coremap[kicked_ass_page].state == CLEAN);
	// now update coremap entry
	coremap[kicked_ass_page].addrspace = curthread->t_vmspace;
	coremap[kicked_ass_page].state = DIRTY; // newly allocated user page shall start DIRTY
	coremap[kicked_ass_page].mapped_vaddr = va;
	coremap[kicked_ass_page].num_pages_allocated = 1;
	return coremap[kicked_ass_page].frame_start;
}


/*
	Function to evict or swap multiple pages starting from starting_frame
	** updates coremap entry && PTE
*/
void evict_or_swap_multiple(int starting_frame, size_t npages){
	assert(curspl > 0);
	int i;
	for (i = starting_frame; i < npages + starting_frame; i++) {
		// come on, don't let me down...
		assert(coremap[i].state != FIXED);
		int disk_slot;
		if (coremap[i].state == DIRTY) {
			// page is dirty, swap out :)
			bitmap_alloc(swapfile_map, &disk_slot);
			// cannot exceed total_disk_slots pages
			assert(disk_slot < total_disk_slots);
			off_t disk_addr = disk_slot * PAGE_SIZE;
			swap_out(i, disk_addr);
		} 
		/********************************* Update PTE **********************************/
		u_int32_t *pte = get_PTE_from_addrspace(coremap[i].addrspace,
												coremap[i].mapped_vaddr);
		assert((*pte & PTE_PRESENT) != 0);
		*pte |= PTE_SWAPPED;
		*pte &= PTE_UNSET_PRESENT;
		if (coremap[i].state == DIRTY) {
			*pte &= CLEAR_PAGE_FRAME;
			*pte |= (disk_slot << 12);
		}
		/********************************* Coremap Entry *******************************/
		coremap[i].state = CLEAN;
		coremap[i].mapped_vaddr = 0xDEADBEEF;
		coremap[i].addrspace = NULL;
	}	
}

/*
	For allocating pages in kernel space
*/

vaddr_t alloc_one_page() {
	assert(curspl > 0);
	int kicked_ass_page = get_free_frame_kernel();	
	// now do the allocation
	coremap[kicked_ass_page].addrspace = curthread->t_vmspace;
	coremap[kicked_ass_page].state = FIXED; // keep kernel pages in memory
	coremap[kicked_ass_page].mapped_vaddr = PADDR_TO_KVADDR(coremap[kicked_ass_page].frame_start);
	coremap[kicked_ass_page].num_pages_allocated = 1;
	return (coremap[kicked_ass_page].mapped_vaddr);
}
/*
	Allocate npages.
*/
vaddr_t alloc_npages(int npages) {
	assert(curspl > 0);
	int num_continous = 1;
	int i = 0; 
	// find if there're npages in succession
	for (; i < num_frames - 1; i++){
		// as long as we've got enough, we break:)
		if (num_continous >= npages) break;
		if ((coremap[i].state == FREE) && (coremap[i + 1].state == FREE)) {
			num_continous++;
		} else {
			num_continous = 1;
		}
	}
	// start is the first page allocated
	int start = i - npages + 1;
	if (num_continous >= npages) {
		// found n continous free pages, just do the allocation
		int j = start;
		for (; j < npages + start; j++){
			coremap[j].addrspace = curthread->t_vmspace;
			coremap[j].state = FIXED;
			coremap[j].mapped_vaddr = PADDR_TO_KVADDR(coremap[j].frame_start);
			// redundancy not a problem ;)
			coremap[j].num_pages_allocated = npages; 
		}
		assert(coremap[start].mapped_vaddr == PADDR_TO_KVADDR(coremap[start].frame_start));
		return PADDR_TO_KVADDR(coremap[start].frame_start);

	} else {
		int found = 0;
		int continous = 0;
		for (i = 0; i < num_frames; i++) {
			if (continous >= npages) {
				found = 1;
				break;
			}
			if(coremap[i].state == FIXED) {
				continous = 0;
				continue;
			} else {
				continous ++;
			}
		}
		if(!found) {
			return NULL;
		}
		int starting_frame = i - npages + 1;
		// sanity check
		int j = starting_frame;
		for (; j < starting_frame + npages; j++) {
			if (coremap[j].state == FIXED) 
				panic("alloc_npages contains a fixed page"); 
		}
		// evict/swap all pages to disk
		evict_or_swap_multiple(starting_frame, npages);
		// sanity check: these npages shall now be free or clean
		for (i = starting_frame; i < npages + starting_frame; i++) {
			if (coremap[i].state != CLEAN || coremap[i].state != FREE) 
				panic("alloc_npages after evict/swap contains a non-free page"); 
		}
		// allocation
		for (i = starting_frame; i < npages + starting_frame; i++) {
			coremap[i].addrspace = curthread->t_vmspace;
			coremap[i].state = FIXED;
			coremap[i].mapped_vaddr = PADDR_TO_KVADDR(coremap[i].frame_start);
			coremap[i].num_pages_allocated = npages; 
		}
		return PADDR_TO_KVADDR(coremap[starting_frame].frame_start);
	}
}


/* Allocate/free some kernel-space virtual pages */
vaddr_t 
alloc_kpages(int npages)
{	
	int spl = splhigh();
	if(vm_bootstraped == 0){
		vaddr_t vaddr = getppages(npages);
		splx(spl);
		return PADDR_TO_KVADDR(vaddr);
	} else {
		vaddr_t	ret = (npages == 1) ? alloc_one_page() : alloc_npages(npages);
		splx(spl);
		return ret; 
	}
}

/*
	Function to free a certain number of pages given *ONLY* the starting address of the page.
	*** The given address shall be page aligned ***
	To know the number of pages to free here, we need to store information in the page structure
	when we do the page allocation accordingly.
*/

void 
free_kpages(vaddr_t addr)
{	
	// the addr must be page aligned
	assert(addr % PAGE_SIZE == 0);

	int spl = splhigh();
	int i = 0;
	for (i = 0; i < num_frames; i++) {
		if (PADDR_TO_KVADDR(coremap[i].frame_start) == addr) {
			// found the starting page
			int numpage_to_free = coremap[i].num_pages_allocated;
			int j;
			for (j = 0; j < numpage_to_free; j++) {
				coremap[j + i].mapped_vaddr = 0xDEADBEEF;
				coremap[j + i].state = FREE;
				coremap[j + i].num_pages_allocated = 0;
			}
			splx(spl);
			return;
		}
	}
	// not found 
	splx(spl);
	panic("invalid addr to free_kpages");
}

/*
 * When TLB miss happening, a page fault will be trigged.
 * The way to handle it is as follow:
 * 1. check what page fault it is, if it is READONLY fault, 
 *    then do nothing just pop up an exception and kill the process
 * 2. if it is a read fault or write fault
 *    1. first check whether this virtual address is within any of the regions
 *       or stack of the current addrspace. if it is not, pop up a exception and
 *       kill the process, if it is there, goes on. 
 *    2. then try to find the mapping in the page table, 
 *       if a page table entry exists for this virtual address insert it into TLB 
 *    3. if this virtual address is not mapped yet, mapping this address,
 *     update the pagetable, then insert it into TLB
 */

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	struct addrspace *as;
	int spl;

	spl = splhigh();

	faultaddress &= PAGE_FRAME; 

	DEBUG(DB_VM, "vm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:
			return EFAULT;
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		splx(spl);
		return EINVAL;
	}

	as = curthread->t_vmspace;

	if (as == NULL) {
		/*
		 * No address space set up. This is probably a kernel
		 * fault early in boot. Return EFAULT so as to panic
		 * instead of getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	unsigned int permissions = 0;
	vaddr_t vbase, vtop;

	/*********************************** Check the validity of the faulting address ******************************/
	int i = 0;
	int found = 0;
	for (; i < array_getnum(as->as_regions); i++) {

		struct as_region * cur = array_getguy(as->as_regions, i);
		vbase = cur->vbase;
		vtop = vbase + cur->npages * PAGE_SIZE;
		// base addr should be page aligned
		assert((cur->vbase & PAGE_FRAME) == cur->vbase);  

		// find which region the faulting address btlb_lowngs to
		if(faultaddress >= vbase && faultaddress < vtop){
			found = 1;
			// get the permission of the region
			permissions = (cur->region_permis);
			int err = handle_vaddr_fault(faultaddress, cur->region_permis); 
			splx(spl);
			return err;
		}
	}

	//if we didn't fall into any region, we check if it falls into the user stack.
	if(!found){
		vtop = USERSTACK;
		// hardcoded stack region
		vbase = vtop - MAX_STACK_PAGES * PAGE_SIZE; 
		if(faultaddress >= vbase && faultaddress < vtop){
			found = 1;
			permissions |= (PF_W | PF_R); 
			splx(spl);
			int err = handle_vaddr_fault(faultaddress, permissions);
			return err;
		}
	}
	// the last place we check is the heap
	if(!found){
		vbase = as->heap_start;
		vtop = as->heap_end;
		if(faultaddress >= vbase && faultaddress < vtop){
			found = 1;
			// heap region is read/write of course
			permissions |= (PF_W | PF_R); 
			int err = handle_vaddr_fault(faultaddress, permissions);
			splx(spl);
			return err;
		}
	}
	// cannot find the faulting address, this is a segfault

	splx(spl);
	return EFAULT;
}

	#define FIRST_LEVEL_PN 0xffc00000 /* mask to get the 1st-level pagetable index from vaddr (first 10 bits) */
	#define SEC_LEVEL_PN 0x003ff000	/* mask to get the 2nd-level pagetable index from vaddr (mid 10 bits) */
	#define PAGE_FRAME 0xfffff000	/* mask to get the page number from vaddr (first 20 bits) */
	#define PHY_PAGENUM 0xfffff000  /* Redundancy here :) */
	#define INVALIDATE_PTE 0xfffff3ff  /* invalidate PTE by setting PRESENT and SWAPPED bits to zero */
	#define KVADDR_TO_PADDR(vaddr) ((vaddr)-MIPS_KSEG0) /* Convert a kernel space vaddr to paddr */
	#define CLEAR_PAGE_FRAME 0x00000fff /* Zero out the first 20 bits */

/*
	Do the right thing, since the faulting address has been validated
*/
int handle_vaddr_fault(vaddr_t faultaddress, unsigned int permissions) {

	int spl = splhigh();
	vaddr_t vaddr;
	paddr_t paddr;
 
	int level1_index = (faultaddress & FIRST_LEVEL_PN) >> 22; 
	int level2_index = (faultaddress & SEC_LEVEL_PN) >> 12;
	// check if the 2nd level page table exists
	struct as_pagetable *level2_pagetable = curthread->t_vmspace->as_master_pagetable[level1_index];
	/************************************* 2nd Level Pagetable exists***************************************/
	if(level2_pagetable != NULL) {
	
		u_int32_t *pte = &(level2_pagetable->PTE[level2_index]);

		if (*pte & PTE_PRESENT) {
			// page is present in physical memory, meaning this is merely a TLB miss,
			// so we just load the mapping into TLB
			assert((*pte & PTE_SWAPPED) == 0); // a page that is present cannot at the same time be swapped
			paddr = *pte & PHY_PAGENUM; 
		} else {
			// if page is not present, one case is that the page was swapped out...
			if (*pte & PTE_SWAPPED) { 

				paddr = load_swapped_page(curthread->t_vmspace, faultaddress);

			} else {
				// ... the other case is that the page does not exist
				paddr = alloc_page_userspace(faultaddress);
			}
			// now udpate the PTE with the physical frame number and PRESENT bit
			assert((paddr & PAGE_FRAME) == paddr);
			*pte &= CLEAR_PAGE_FRAME;
			*pte |= paddr;
	    	*pte |= PTE_PRESENT;
	    	*pte &= PTE_UNSET_SWAPPED;
		}
	} else {
		/************************************* 2nd Level Pagetable DNE ***************************************/
		// If second page table doesn't exist, create one --> demand paging part
		curthread->t_vmspace->as_master_pagetable[level1_index] = kmalloc(sizeof(struct as_pagetable));
		level2_pagetable = curthread->t_vmspace->as_master_pagetable[level1_index];
		assert(level2_pagetable != NULL);
		// initialize all PTE to 0, in order to unset both the PRESENT and SWAPPED bits 
		int i = 0;
		for (; i < SECOND_LEVEL_PT_SIZE; i++) {
			level2_pagetable->PTE[i] = 0;
		}
	    // allocate a page and do the mapping
	    paddr = alloc_page_userspace(faultaddress);
	    assert(paddr % PAGE_SIZE == 0);
		
	    // update pte: PRESENT = 1
	    u_int32_t* pte = get_PTE(curthread, faultaddress); 

	    assert((*pte & PTE_PRESENT) == 0);
	    assert((*pte & PTE_SWAPPED) == 0);
	    assert((*pte & PAGE_FRAME) == 0); // it can't have a frame number now
		
		*pte &= CLEAR_PAGE_FRAME;
		*pte &= PTE_UNSET_SWAPPED;
	    *pte |= PTE_PRESENT;
	    *pte |= paddr;
	    // consistent?
	    assert(*pte == level2_pagetable->PTE[level2_index]);
	}

	/* once we are here, it means that we can guarantee that there exists PTE in page table for faultaddress.
	Now we need to load the mapping to TLB */
	if (permissions & PF_W) {
		paddr |= TLBLO_DIRTY;  
	}
	
	u_int32_t tlb_hi, tlb_low;
	int k = 0;	
	for(; k< NUM_TLB; k++){
		TLB_Read(&tlb_hi, &tlb_low, k);
		// skip valid ones
		if(tlb_low & TLBLO_VALID){
			continue;
		}
		//once we find an empty entry, update it with tlb_hi and tlb_loww.
		tlb_hi = faultaddress;
		tlb_low = paddr | TLBLO_VALID; 
		TLB_Write(tlb_hi, tlb_low, k);
		splx(spl);
		return 0;
	}
	// no invalid ones, so we randomly kicked out an entry
	tlb_hi = faultaddress;
	tlb_low = paddr | TLBLO_VALID;
	TLB_Random(tlb_hi, tlb_low);
	splx(spl);
	return 0;
}



/*
	Function to clear the content of a certain number of pages
*/
void as_zero_page(paddr_t paddr, size_t num_pages) {
	bzero((void *) PADDR_TO_KVADDR(paddr), num_pages * PAGE_SIZE);
}


/*
	Function to write a page to swap_file
	@param frame_id, pos is the starting offset for the write operation
	@precondition: the frame shall be DIRTY before calling this function
	** Note:  It only invalidates the TLB entry but does not update either 
	pte or coremap, it is up to the caller to do whatever appropriate

*/
void swap_out(int frame_id, off_t pos) {
	assert(curspl > 0);
	struct uio u;
	assert(coremap[frame_id].state == DIRTY);
	paddr_t dest = coremap[frame_id].frame_start;
	// initialize the uio
	assert((PADDR_TO_KVADDR(dest) % 512) == 0);

	assert(pos % 512 == 0);
	assert(pos / 4096 < total_disk_slots);

	mk_kuio(&u, PADDR_TO_KVADDR(dest), PAGE_SIZE, pos, UIO_WRITE);
	// does the actual write
	if (VOP_WRITE(swap_file, &u)){
		panic("write page to disk failed");
	}
	// invalidate the TLB entry of this swapped page, if found
	// u_int32_t tlb_hi, tlb_low;
	// int k = 0;	
	// for(; k< NUM_TLB; k++){
	// 	TLB_Read(&tlb_hi, &tlb_low, k);
	// 	if(((tlb_hi & PAGE_FRAME) == coremap[frame_id].mapped_vaddr) 
	// 		&& ((tlb_low & PAGE_FRAME) == coremap[frame_id].frame_start)){
	// 		TLB_Write(TLBHI_INVALID(k), TLBLO_INVALID(), k);
	// 	}
	// }
	int i;
	for (i = 0; i < NUM_TLB; i++) {
		TLB_Write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}
	return;
}

/*
	Function to bring a page back to memory, evicting pages if necessary
*/

paddr_t fetch_page(struct addrspace* as, vaddr_t va){
	assert(curspl > 0);
	u_int32_t * pte = get_PTE_from_addrspace(as, va);
	assert(pte != NULL);
	assert((*pte & PTE_SWAPPED) != 0);
	// find a free frame and load it back
	int i = 0, found = 0;
	for (; i < num_frames; i++) {
		if (coremap[i].state == FREE){
			found = 1;
			load_page(as, va, i);
			return coremap[i].frame_start;
		}
	}
	// no free frame found, make one :)
	if(!found) {
		int free_frame = evict_or_swap();
		load_page(as, va, free_frame);
		return coremap[free_frame].frame_start;
	}
}


/*
	Loads a page to physical memory at the specified frame_id
	@precondition: the physical page at frame_id must be free for loading
	NOTE: this only modifies the coremap entry, but does not touch the
	PTE or page table
*/
void load_page(struct addrspace* addrspace, vaddr_t vaddr, int frame_id) {
	assert(curspl > 0);
	u_int32_t *pte = get_PTE_from_addrspace(addrspace, vaddr); 
	assert(pte != NULL);
	// the first 20 bits is the slot of the page in swapfile
	off_t pos = ((*pte & SWAPFILE_OFFSET) >> 12) * PAGE_SIZE; 
	struct uio u;
	// the destination address is the frame start
	paddr_t dest = coremap[frame_id].frame_start;
	// aligned?
	assert(PADDR_TO_KVADDR(dest) % 512 == 0);
	assert(pos % 512 == 0);

	mk_kuio(&u, PADDR_TO_KVADDR(dest), PAGE_SIZE, pos, UIO_READ);

	if(VOP_READ(swap_file, &u)) {
		panic("load page from disk failed");
	}
	// update coremap entry
	coremap[frame_id].addrspace = addrspace;
	coremap[frame_id].mapped_vaddr = vaddr;
	coremap[frame_id].state = DIRTY; // not really, but safety first
	coremap[frame_id].num_pages_allocated = 1;
	return;
}


/*
	A demon thread that periodically evicts the dirty pages
	i.e. mark the physical pages as CLEAN
*/
void background_paging(void * args, unsigned int argc) {
	(void) args;
	(void) args;
	int spl = splhigh();
	int i = 0;
	for (; i < num_frames; i++) {
		if(coremap[i].state == DIRTY) {
			// if this page already has a copy in swapfile, we update it
			vaddr_t va = coremap[i].mapped_vaddr;
			assert(va != 0xDEADBEEF);
			u_int32_t *pte = get_PTE(curthread, va);
			assert(pte != NULL); // 2nd level page table does not exist? u f**king kidding me right?
			assert((*pte & PTE_PRESENT) != 0); // come on, you must be present
			int disk_slot = 0;
			off_t disk_addr = 0;
			if (*pte & PTE_SWAPPED) {
				// if this page was ever swapped to the disk... 
				// shit, need to find it. the PTE does not have the  

			} else {
				// otherwise, we find a new slot in swapfile and do the swapping
				bitmap_alloc(swapfile_map, &disk_slot);
				disk_addr = disk_slot * PAGE_SIZE;
			}
			swap_out(i, disk_addr);
		}
	}
	splx(spl);
}


/*
	Function that finds a free/clean frame, evict/swap if necessary
*/
int get_free_frame() {
		assert(curspl > 0);

	int i = 0, free_frame = -1;
	for (; i < num_frames; i++) {
		if (coremap[i].state == FREE){
			free_frame = i;
			break;
		}
	}

	if(free_frame == -1){
		free_frame = evict_or_swap();
	}
	return free_frame;
}

int get_free_frame_kernel() {
	assert(curspl > 0);

	int i = 0, free_frame = -1;
	for (; i < num_frames; i++) {
		if (coremap[i].state == FREE){
			free_frame = i;
			break;
		}
	}

	if(free_frame == -1){
		free_frame = evict_or_swap_kernel();
	}
	return free_frame;
}



/* 
	Function that loads a specified page from swapfile, evict/swap if necessary.
	@return physical address of the loaded page in mem
*/

paddr_t load_swapped_page(struct addrspace* as, vaddr_t va){
		assert(curspl > 0);

	int free_frame = get_free_frame();
	assert(coremap[free_frame].state == CLEAN || coremap[free_frame].state == FREE);
	// load the page into this frame
	load_page(as, va, free_frame);
	assert(coremap[free_frame].state == DIRTY);
	return coremap[free_frame].frame_start;
}
