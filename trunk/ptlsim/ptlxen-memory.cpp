//
// PTLsim: Cycle Accurate x86-64 Simulator
// Memory functions for Xen subsystem
//
// Copyright 2005-2006 Matt T. Yourst <yourst@yourst.com>
//

#include <globals.h>
#include <superstl.h>
#include <ptlxen.h>
#include <mm.h>
#include <ptlsim.h>
#include <stats.h>

//
// PTLsim internal page table management
//
mmu_update_t mmuqueue[1024];
int mmuqueue_count = 0;

int do_commit_page_table_updates() {
  static const bool DEBUG = 0;

  if (DEBUG) logfile << "Page table update commit of ", mmuqueue_count, " entries:", endl, flush;

  foreach (i, mmuqueue_count) {
    mmu_update_t& mmu = mmuqueue[i];
    W64 virt = mmu.ptr;

    if likely (virt_is_inside_ptlsim(mmu.ptr)) {
      mmu.ptr = ptl_virt_to_phys((void*)mmu.ptr);
    } else if likely (virt_is_inside_physmap(mmu.ptr)) {
      mmu.ptr = mapped_virt_to_phys((void*)mmu.ptr);
    } else {
      // invalid update
      mmu.ptr = 0;
    }

    if (DEBUG) logfile << "  virt 0x", hexstring(virt, 64), ", phys 0x", hexstring(mmu.ptr, 64), " (mfn ", intstring(mmu.ptr >> 12, 8), 
                 " offset ", intstring(lowbits(mmu.ptr, 12) / 8, 8), ") <= ", Level1PTE(mmu.val), endl, flush;
  }

  int update_count = 0;
  int rc = HYPERVISOR_mmu_update(mmuqueue, mmuqueue_count, &update_count, DOMID_SELF);

  if unlikely (rc) {
    logfile << "Page table update commit failed for ", mmuqueue_count, " entries (completed ", update_count, " entries):", endl, flush;
    foreach (i, mmuqueue_count) {
      logfile << "  phys 0x", hexstring(mmuqueue[i].ptr, 64), " (mfn ", intstring(mmuqueue[i].ptr >> 12, 8), 
        " offset ", intstring(lowbits(mmuqueue[i].ptr, 12) / 8, 8), ") <= ", Level1PTE(mmuqueue[i].val), endl, flush;
    }
  }

  mmuqueue_count = 0;

  return rc;
}

// Update a PTE by its physical address
template <typename T>
int update_phys_pte(Waddr dest, const T& src) {
	mmu_update_t u;
	u.ptr = dest;
	u.val = (W64)src;
  return HYPERVISOR_mmu_update(&u, 1, NULL, DOMID_SELF);
}

int pin_page_table_page(void* virt, int level) {
  return 0;
  assert(inrange(level, 0, 4));

  // Was it in PTLsim space?
  mfn_t mfn = ptl_virt_to_mfn(virt);
  if unlikely (mfn == INVALID_MFN) return -1;
  
  int level_to_function[5] = {MMUEXT_UNPIN_TABLE, MMUEXT_PIN_L1_TABLE, MMUEXT_PIN_L2_TABLE, MMUEXT_PIN_L3_TABLE, MMUEXT_PIN_L4_TABLE};
  int func = level_to_function[level];
  
  int rc = 0;
  mmuext_op op;
  op.cmd = func;
  op.arg1.mfn = mfn;

  int success_count = 0;
  return HYPERVISOR_mmuext_op(&op, 1, &success_count, DOMID_SELF);
}

int make_ptl_page_writable(void* virt, bool writable) {
  pfn_t pfn = ptl_virt_to_pfn(virt);
  if unlikely (pfn == INVALID_MFN) return -1;

  Level1PTE& pte = bootinfo.ptl_pagedir[pfn];
  Level1PTE temppte = pte;
  temppte.rw = writable;
  return update_ptl_pte(pte, temppte);
}

int query_pages(page_type_t* pt, int count) {
  mmuext_op op;
  op.cmd = MMUEXT_QUERY_PAGES;
  op.arg1.linear_addr = (Waddr)pt;
  op.arg2.nr_ents = count;

  int success_count = 0;
  return HYPERVISOR_mmuext_op(&op, 1, &success_count, DOMID_SELF);
}

page_type_t query_page(mfn_t mfn) {
  unmap_phys_page(mfn);

  page_type_t pt;
  pt.in.mfn = mfn;

  mmuext_op op;
  op.cmd = MMUEXT_QUERY_PAGES;
  op.arg1.linear_addr = (Waddr)&pt;
  op.arg2.nr_ents = 1;

  int success_count = 0;
  assert(HYPERVISOR_mmuext_op(&op, 1, &success_count, DOMID_SELF) == 0);

  return pt;
}


//
// Physical memory map (physmap)
//

//
// Dummy page for speculative faults: this page of all zeros
// is mapped in whenever we try to access physical memory
// that doesn't exist, isn't ours, or is part of Xen itself.
//
void* zeropage;

Level4PTE ptlsim_pml4_entry;
Level4PTE physmap_pml4_entry;

struct Level1PTE* phys_pagedir;
struct Level2PTE* phys_level2_pagedir;
struct Level3PTE* phys_level3_pagedir;
mfn_t phys_level3_pagedir_mfn;
W64 phys_pagedir_mfn_count;

//
// Build page tables for the 1:1 mapping of physical memory.
//
// Since we don't know which pages a domain can access until later,
// and the accessibility may change at any time, we only build levels
// L2 and L3, but leave L1 to be constructed on demand (we still do
// allocate L1, we just don't fill it).
//
// On return, PML4 slot 508 (0xfffffe0000000000) should be set to
// ptl_virt_to_mfn(phys_level3_pagedir).
//
void build_physmap_page_tables() {
  static const bool DEBUG = 0;

  if (DEBUG) cerr << "Building physical page map for ", bootinfo.total_machine_pages, " pages (",
    (pages_to_kb(bootinfo.total_machine_pages) / 1024), " MB)", " of memory:", endl, flush;

  zeropage = ptl_alloc_private_page();
  ptl_zero_private_page(zeropage);

  Waddr physmap_level1_page_count = ceil(bootinfo.total_machine_pages, PTES_PER_PAGE) / PTES_PER_PAGE;

  phys_pagedir = (Level1PTE*)ptl_alloc_private_pages(physmap_level1_page_count * PAGE_SIZE);
  memset(phys_pagedir, 0, physmap_level1_page_count * PAGE_SIZE);

  if (DEBUG) cerr << "  L1 page table at virt ", (void*)phys_pagedir, " (", bootinfo.total_machine_pages, " entries, ",
    physmap_level1_page_count, " pages, ", (bootinfo.total_machine_pages * sizeof(Level1PTE)), " bytes)", endl, flush;

  //
  // Construct L2 page tables, pointing to fill-on-demand L1 tables:
  //
  Waddr physmap_level2_page_count = ceil(physmap_level1_page_count, PTES_PER_PAGE) / PTES_PER_PAGE;
  phys_level2_pagedir = (Level2PTE*)ptl_alloc_private_pages(physmap_level2_page_count * PAGE_SIZE);

  if (DEBUG) cerr << "  L2 page table at virt ", (void*)phys_level2_pagedir, " (", physmap_level1_page_count, " entries, ",
    physmap_level2_page_count, " pages, ", (physmap_level1_page_count * sizeof(Level1PTE)), " bytes)", endl, flush;

  foreach (i, physmap_level1_page_count) {
    Level2PTE& pte = phys_level2_pagedir[i];
    pte = 0;
    pte.p = 1;  // let PTLsim fill it in on demand
    pte.rw = 1; // sub-pages are writable unless overridden
    pte.us = 1; // both user and supervisor (PTLsim itself will check protections)
    pte.a = 1;  // accessed
    pte.mfn = ptl_virt_to_mfn(phys_pagedir + (i * PTES_PER_PAGE));

    // cerr << "    Slot ", intstring(i, 6), " = ", pte, endl, flush;
    pte.p = 0;
  }

  // Clear out leftover slots: we may not care, but Xen will complain:
  if ((physmap_level1_page_count & (PTES_PER_PAGE-1)) > 0) {
    foreach (i, PTES_PER_PAGE - (physmap_level1_page_count & (PTES_PER_PAGE-1))) {
      // cerr << "    Slot ", intstring(physmap_level1_page_count + i, 6), " is left over", endl, flush;
      phys_level2_pagedir[physmap_level1_page_count + i] = 0;
    }
  }

  //
  // Construct L3 page table (just one page covers 2^39 bit phys addr space):
  //
  // PTLmon has pre-allocated an mfn for this page and it is already mapped
  // into every address space. Therefore, we need to do a batched update
  // on the page.
  //
  assert(physmap_level2_page_count < PTES_PER_PAGE);
  phys_level3_pagedir = (Level3PTE*)ptl_alloc_private_page();
  ptl_zero_private_page(phys_level3_pagedir);

  if (DEBUG) cerr << "  L3 page table at mfn ", phys_level3_pagedir_mfn, ", virt ", (void*)phys_level3_pagedir, " (", physmap_level2_page_count, " entries, ",
    1, " pages, ", (physmap_level2_page_count * sizeof(Level1PTE)), " bytes)", endl, flush;

  foreach (i, physmap_level2_page_count) {
    Level3PTE& pte = phys_level3_pagedir[i];
    Level3PTE newpte = 0;

    newpte.p = 1;  // pre-filled
    newpte.rw = 1; // sub-pages are writable unless overridden
    newpte.us = 1; // both user and supervisor (PTLsim itself will check protections)
    newpte.a = 1;  // accessed

    // Link back to L2 tables:
    Level2PTE* ptvirt = phys_level2_pagedir + (i * PTES_PER_PAGE);
    newpte.mfn = ptl_virt_to_mfn(ptvirt);
    // cerr << "    Slot ", intstring(i, 6), " = ", pte, endl, flush;
    assert(make_ptl_page_writable(ptvirt, false) == 0);

    pte = newpte;
  }

  //
  // Remap and pin L3 page
  //
  if (DEBUG) cerr << "  Final L3 page table page at virt ", phys_level3_pagedir,
    " (mfn ", ptl_virt_to_mfn(phys_level3_pagedir), ")", endl, flush;
  assert(make_ptl_page_writable(phys_level3_pagedir, false) == 0);

  //
  // Build PTLsim PML4 entry 510:
  //
  ptlsim_pml4_entry = 0;
  ptlsim_pml4_entry.p = 1;
  ptlsim_pml4_entry.rw = 1;
  ptlsim_pml4_entry.us = 1;
  ptlsim_pml4_entry.a = 1;
  ptlsim_pml4_entry.mfn = ptl_virt_to_mfn(bootinfo.ptl_level3_map);

  //
  // Build physmap PML4 entry 508:
  //
  physmap_pml4_entry = 0;
  physmap_pml4_entry.p = 1;
  physmap_pml4_entry.rw = 1;
  physmap_pml4_entry.us = 1;
  physmap_pml4_entry.a = 1;
  physmap_pml4_entry.mfn = ptl_virt_to_mfn(phys_level3_pagedir);

  //
  // Inject the physmap entry into the current page table,
  // which must be PTLsim's initial page table:
  //
  mfn_t top_mfn = bootinfo.toplevel_page_table_mfn;
  assert(top_mfn == get_cr3_mfn());
  int physmap_slot = VirtAddr(PHYS_VIRT_BASE).lm.level4;
  assert(update_phys_pte_mfn_and_slot(top_mfn, physmap_slot, physmap_pml4_entry) == 0);
}

//
// Page fault handling logic:
//
// By default, PTLsim maps physical pages as writable the first time
// they are referenced. Since we call unmap_address_space() before
// passing through any hypercalls that could collide with our now
// removed writable mappings, this is not a problem.
//
// If Xen refuses to update the physmap PTE with a writable mapping,
// this means some live page table is pinning it to read-only. In
// this case, for loads at least, we simply make it a read only
// mapping, which is always allowed.
//
// When we attempt to commit user stores, we check the pte.rw
// bit for the mapped page and if it's 0, we let Xen validate
// and commit the store for us.
//

//
// Map physical pages, building page tables as we go
//
static const bool force_page_fault_logging = 0;

bool force_readonly_physmap = 0;
Waddr last_virtaddr_triggering_walk = 0;
Waddr last_guest_rip_triggering_walk = 0;
Waddr last_guest_uuid_triggering_walk = 0;

int map_phys_page(mfn_t mfn, Waddr rip) {
  int level2_slot_index = mfn / PTES_PER_PAGE;
  W64 faultaddr = mfn << 12;
  Level2PTE& l2pte = phys_level2_pagedir[level2_slot_index];
  Level1PTE& l1pte = phys_pagedir[mfn];

  if unlikely (!l2pte.p) {
    //
    // Level 2 PTE was not present: either this is the first
    // access or it was fast cleared by unmap_address_space().
    // In any case, re-establish it after clearing any old
    // PTEs from the corresponding L1 page.
    //
    Level1PTE* l1page = floorptr(&l1pte, PAGE_SIZE);
    assert(make_ptl_page_writable(l1page, 1) == 0);
    ptl_zero_private_page(l1page);
    assert(make_ptl_page_writable(l1page, 0) == 0);
        
    assert(update_ptl_pte(l2pte, l2pte.P(1)) == 0);
        
    if (logable(2) | force_page_fault_logging) {
      logfile << "[PTLsim Page Fault Handler from rip ", (void*)rip, "] ",
        (void*)faultaddr, ": added L2 PTE slot ", level2_slot_index, " (L1 mfn ",
        l2pte.mfn, ") to PTLsim physmap; toplevel cr3 mfn ", get_cr3_mfn(), endl;
    }
  }
   
  //
  // Page was not present: try to map the page read-write
  //
  Level1PTE pte = 0;
  pte.p = 1;
  pte.rw = 1;
  pte.us = 1;
  pte.mfn = mfn;
      
  int rc = (force_readonly_physmap) ? -EINVAL : update_ptl_pte(l1pte, pte);
      
  if unlikely (rc) {
    //
    // It's a special page and must be marked read-only:
    //
    pte.rw = 0;
    rc = update_ptl_pte(l1pte, pte);
        
    if unlikely (rc) {
      //
      // We still can't map the page! Most likely we got here after
      // attempting to follow a virtaddr in the Xen reserved area,
      // and we can't map some Xen-internal page table page that
      // the native processor can see but the domain cannot.
      //
      // This can happen on speculative out-of-order accesses
      // that never make it to the architectural state but
      // nonetheless still must have *some* page to access
      // or PTLsim will deadlock.
      //
      // Map in a zero page (to terminate all page table walks)
      // and print a warning in the log. This is the same
      // behavior as if invalid physical memory were accessed
      // (but in that case the page is all 1's, not all 0's).
      //
      if unlikely (logable(2) | force_page_fault_logging) {
        logfile << "[PTLsim Page Fault Handler from rip ", (void*)rip, "] ",
          (void*)faultaddr, ": added dummy zero PTE for guest mfn ", mfn, 
          " (", sim_cycle, " cycles, ", total_user_insns_committed, " commits)", endl;

        logfile << "Warning: failed to map mfn ", mfn, " (for virt ", (void*)faultaddr, ", requested by rip ", (void*)rip, " at cycle ", sim_cycle, ")", endl;
        logfile << "  Either it doesn't exist, isn't ours, or is part of Xen itself.", endl;
        logfile << "  Mapping a zero page in its place and hoping the access is speculative.", endl;
        logfile << "  The last page table walk was for virtual address ", (void*)last_virtaddr_triggering_walk, endl;
        logfile << flush;
      }

      pte.mfn = ptl_virt_to_mfn(zeropage);
      rc = update_ptl_pte(l1pte, pte);
      assert(rc == 0);
      return 3;
    } else {
      if unlikely (logable(2) | force_page_fault_logging) {
        logfile << "[PTLsim Page Fault Handler from rip ", (void*)rip, "] ", 
          (void*)faultaddr, ": added read-only L1 PTE for guest mfn ", mfn,
          " (", sim_cycle, " cycles, ", total_user_insns_committed, " commits)", endl;
      }
      return 2;
    }
  } else {
    if unlikely (logable(2) | force_page_fault_logging) {
      logfile << "[PTLsim Page Fault Handler from rip ", (void*)rip,
        "] ", (void*)faultaddr, ": added L1 PTE for guest mfn ", mfn, 
        ", toplevel cr3 mfn ", get_cr3_mfn(), " (", sim_cycle, " cycles, ", total_user_insns_committed, " commits)", endl;
    }
    return 1;
  }

  return 0;
}

void unmap_phys_page(mfn_t mfn) {
  Level1PTE& pte = phys_pagedir[mfn];
  if unlikely (!pte.p) return;
  assert(update_ptl_pte(pte, Level1PTE(0)) == 0);
}

//
// Unmap an entire tree of physical pages rooted
// at the specified L4 mfn. This must be done
// before passing a pin hypercall or new_baseptr
// hypercall up to Xen. We may have read/write
// refs to some of these pages, which are currently
// normal pages (updated by the guest kernel) but
// which will become read-only page table pages
// once Xen tries to pin the entire tree. We only
// need to unmap L4/L3/L2 pages; L1 pages (i.e.
// the actual data pages) are not relevant.
// 
// Only those pages with read/write mappings are 
// unmapped. Levels 4/3/2 of the page table are
// recursively traversed and unmapped from the leaves
// on up, so we do not accidentally touch a page and
// re-map it on our way back to the root.
//
static const bool debug_unmap_phys_page_tree = 1;

inline void unmap_level1_page_tree(mfn_t mfn) {
  // No need to unmap actual leaf physical pages - those are just data pages
  Level1PTE& physpte = phys_pagedir[mfn];
  if unlikely (debug_unmap_phys_page_tree & logable(100)) logfile << "        L1: mfn ", intstring(mfn, 8), ((physpte.p & physpte.rw) ? " (unmap)" : ""), endl;
  if unlikely (physpte.p & physpte.rw) physpte <= physpte.P(0);
}

inline void unmap_level2_page_tree(mfn_t mfn) {
  Level2PTE* ptes = (Level2PTE*)phys_to_mapped_virt(mfn << 12);
  foreach (i, PTES_PER_PAGE) if unlikely (ptes[i].p) unmap_level1_page_tree(ptes[i].mfn);
  Level1PTE& physpte = phys_pagedir[mfn];
  if unlikely (debug_unmap_phys_page_tree & logable(100)) logfile << "      L2: mfn ", intstring(mfn, 8), ((physpte.p & physpte.rw) ? " (unmap)" : ""), endl;
  if unlikely (physpte.p & physpte.rw) physpte <= physpte.P(0);
}

void unmap_level3_page_tree(mfn_t mfn) {
  Level3PTE* ptes = (Level3PTE*)phys_to_mapped_virt(mfn << 12);
  foreach (i, PTES_PER_PAGE) if unlikely (ptes[i].p) unmap_level2_page_tree(ptes[i].mfn);
  Level1PTE& physpte = phys_pagedir[mfn];
  if unlikely (debug_unmap_phys_page_tree & logable(100)) logfile << "    L3: mfn ", intstring(mfn, 8), ((physpte.p & physpte.rw) ? " (unmap)" : ""), endl;
  if unlikely (physpte.p & physpte.rw) physpte <= physpte.P(0);
}

void unmap_level4_page_tree(mfn_t mfn) {
  Level4PTE* ptes = (Level4PTE*)phys_to_mapped_virt(mfn << 12);
  foreach (i, PTES_PER_PAGE) if unlikely (ptes[i].p) unmap_level3_page_tree(ptes[i].mfn);
  Level1PTE& physpte = phys_pagedir[mfn];
  if unlikely (debug_unmap_phys_page_tree & logable(100)) logfile << "  L4: mfn ", intstring(mfn, 8), ((physpte.p & physpte.rw) ? " (unmap)" : ""), endl;
  if unlikely (physpte.p & physpte.rw) physpte <= physpte.P(0);
}

void unmap_phys_page_tree(mfn_t root) {
  if (logable(100)) logfile << "Unmapping page tree starting at root mfn ", root, endl;
  unmap_level4_page_tree(root);
  commit_page_table_updates();
}

//
// This is required before switching back to native mode, since we may have
// read/write maps of pages that the guest kernel thinks are read-only
// everywhere; this will cause later pin operations to fail.
//
// We scan the physmap L2 page table, looking for L1 pages that were filled
// in on demand by PTLsim's page fault handler. If the present bit was set,
// we first clear the L2 PTE's present bit, then unpin the L1 page.
//

void unmap_address_space() {
  Waddr physmap_level1_pages = ceil(bootinfo.total_machine_pages, PTES_PER_PAGE) / PTES_PER_PAGE;

  int n = 0;

  if (logable(100)) logfile << "unmap_address_space: check ", physmap_level1_pages, " PTEs:", endl, flush;

  foreach (i, physmap_level1_pages) {
    Level2PTE& l2pte = phys_level2_pagedir[i];
    if unlikely (l2pte.p) {
      l2pte <= l2pte.P(0);
      if (logable(100)) logfile << "  update ", intstring(n, 6), ": pte ", intstring(i, 6), " <= not present", endl;
      n++;
    }
  }

  commit_page_table_updates();
}

//
// Xen puts the virtualized page fault virtual address in arch.cr2
// instead of the machine's CR2 register.
//
static inline Waddr read_cr2() { return shinfo.vcpu_info[0].arch.cr2; }

static int page_fault_in_progress = 0;

asmlinkage void do_page_fault(W64* regs) {
  int rc;
  Waddr faultaddr = read_cr2();
  mfn_t toplevel = get_cr3_mfn();

  //
  // If we are already handling a page fault, and got another one
  // that means we faulted in pagetable walk. Continuing here would cause
  // a recursive fault.
  //
  PageFaultErrorCode pfec = regs[REG_ar1];

  if unlikely (page_fault_in_progress) {
    cerr << "PTLsim Internal Error: recursive page fault @ rip ", (void*)regs[REG_rip], " while accessing ", (void*)faultaddr, " (error code ", pfec, ")", endl, flush;
    cerr << "Registers:", endl;
    print_regs(cerr, regs);
    print_stack(cerr, regs[REG_rsp]);
    cerr.flush();
    logfile.flush();
    shutdown(SHUTDOWN_crash);
  }

  page_fault_in_progress = 1;

  if likely (inrange(faultaddr, PHYS_VIRT_BASE, (PHYS_VIRT_BASE + ((Waddr)bootinfo.total_machine_pages * PAGE_SIZE) - 1))) {
    mfn_t mfn = (faultaddr - (Waddr)PHYS_VIRT_BASE) >> 12;
    map_phys_page(mfn, regs[REG_rip]);
  } else {
    cerr << "PTLsim Internal Error: page fault @ rip ", (void*)regs[REG_rip], " while accessing ", (void*)faultaddr, " (error code ", pfec, "); rsp ", get_rsp(), endl;
    cerr << "Registers:", endl;
    print_regs(cerr, regs);
    print_stack(cerr, regs[REG_rsp]);
    cerr.flush();

    if (logfile) {
      logfile << "Cycle: ", sim_cycle, " PTLsim Internal Error: page fault @ rip ", (void*)regs[REG_rip], " while accessing ", (void*)faultaddr, " (error code ", pfec, "); rsp ", get_rsp(), endl;
      logfile << "Registers:", endl;
      print_regs(logfile, regs);
      print_stack(logfile, regs[REG_rsp]);
    }

    logfile.flush();
    assert(0);
    shutdown(SHUTDOWN_crash);
    asm("ud2a");
  }

  page_fault_in_progress = 0;
}

ostream& operator <<(ostream& os, const page_type_t& pagetype) {
  static const char* page_type_names[] = {"none", "L1", "L2", "L3", "L4", "GDT", "LDT", "write"};
  const char* page_type_name = 
    (pagetype.out.type == PAGE_TYPE_INVALID_MFN) ? "inv" :
    (pagetype.out.type == PAGE_TYPE_INACCESSIBLE) ? "inacc" :
    (pagetype.out.type < lengthof(page_type_names)) ? page_type_names[pagetype.out.type] :
    "???";
  
  os << padstring(page_type_name, -5), " ", (pagetype.out.pinned ? "pin" : "   "), " ",
    intstring(pagetype.out.total_count, 5), " total, ", intstring(pagetype.out.type_count, 5), " by type";
  return os;
}

//
// Debugging helper function to track down stray refs to a page
//
void find_all_mappings_of_mfn(mfn_t mfn) {
  // Start with an empty mapping
  unmap_address_space();

  int pagetype_bytes_allocated = bootinfo.total_machine_pages * sizeof(page_type_t);
  page_type_t* pagetypes = (page_type_t*)ptl_alloc_private_pages(pagetype_bytes_allocated);
  assert(pagetypes);

  foreach (i, bootinfo.total_machine_pages) {
    pagetypes[i].in.mfn = i;
  }

  logfile << "Finding all mappings of mfn ", mfn, ":", endl, flush;
  int rc = query_pages(pagetypes, bootinfo.total_machine_pages);
  logfile << "rc = ", rc, endl, flush;

  force_readonly_physmap = 1;

  foreach (i, bootinfo.total_machine_pages) {
    const page_type_t& pt = pagetypes[i];

    if (pt.out.type == PAGE_TYPE_INACCESSIBLE) continue;

    if (inrange(pt.out.type, (byte)PAGE_TYPE_L1, (byte)PAGE_TYPE_L4)) {
      const Level1PTE* pte = (const Level1PTE*)phys_to_mapped_virt(i << 12);
      foreach (j, PTES_PER_PAGE) {
        if (pte->mfn == mfn) {
          logfile << "  Page table page mfn ", intstring(i, 6), " index ", intstring(j, 3), " references target mfn ", intstring(mfn, 6), ": ", *pte, endl;
        }
        pte++;
      }
    }
  }

  ptl_free_private_pages(pagetypes, pagetype_bytes_allocated);

  force_readonly_physmap = 0;
  unmap_address_space();
}

//
// Page table control
//

//
// Set the real page table on the PTLsim primary VCPU.
//
// Inject the PTLsim toplevel page table entries (PML 510 and PML 508)
// into the specified user mfn before switching, so as to ensure a
// seamless transition. The page must already be pinned.
//

void inject_ptlsim_into_toplevel(mfn_t mfn) {
  int ptlsim_slot = VirtAddr(PTLSIM_VIRT_BASE).lm.level4;
  int physmap_slot = VirtAddr(PHYS_VIRT_BASE).lm.level4;

  assert(update_phys_pte_mfn_and_slot(mfn, ptlsim_slot, ptlsim_pml4_entry) == 0);
  assert(update_phys_pte_mfn_and_slot(mfn, physmap_slot, physmap_pml4_entry) == 0);
}

void switch_page_table(mfn_t mfn) {
  unmap_phys_page(mfn);
  inject_ptlsim_into_toplevel(mfn);

  mmuext_op op;
  op.cmd = MMUEXT_NEW_BASEPTR;
  op.arg1.mfn = mfn;

  int success_count = 0;
  assert(HYPERVISOR_mmuext_op(&op, 1, &success_count, DOMID_SELF) == 0);
}

ostream& print_page_table_with_types(ostream& os, Level1PTE* ptes) {
  page_type_t pagetypes[512];
  foreach (i, 512) {
    pagetypes[i].in.mfn = ptes[i].mfn;
  }

  assert(query_pages(pagetypes, lengthof(pagetypes)) == 0);

  foreach (i, 512) {
    os << "        ", intstring(i, 3), ": ", ptes[i], " type ", pagetypes[i], endl;
  }

  return os;
}

//
// Page Table Walks
//

//
// Walk the page table tree, accumulating the relevant permissions
// as we go, according to x86 rules (specifically, p, rw, us, nx).
//
// The A (accessed) and D (dirty) bits in the returned PTE have
// special meaning. We do not actually update these bits unless
// the instruction causing the PT walk successfully commits.
// Therefore, if the returned A is *not* set, this means one or
// more PT levels need to have their A bits refreshed. If D is
// *not* set, AND the intended access is for a store, the D bits
// also need to be refreshed at the final PT level (level 2 or 1).
// This is done at commit time by page_table_acc_dirty_update().
//

Waddr xen_m2p_map_end;

Level1PTE page_table_walk(W64 rawvirt, W64 toplevel_mfn) {
  VirtAddr virt(rawvirt);
  last_virtaddr_triggering_walk = rawvirt;

  bool acc_bit_up_to_date = 0;

  if unlikely ((rawvirt >= HYPERVISOR_VIRT_START) & (rawvirt < xen_m2p_map_end)) {
    //
    // The access is inside Xen's address space. Xen will not let us even access the
    // page table entries it injects into every top-level page table page, and we
    // cannot map M2P pages like we do other physical pages. Because Xen does not
    // allow its internal page tables to be mapped by guests at all, we have to
    // special-case these virtual addresses.
    //
    // We cheat by biasing the returned physical address such that we have
    // (HYPERVISOR_VIRT_START - PHYS_VIRT_BASE) + PHYS_VIRT_BASE == HYPERVISOR_VIRT_START
    // when other parts of PTLsim use ptl_phys_to_virt to access the memory.
    //
    const Waddr hypervisor_space_mask = (HYPERVISOR_VIRT_END - HYPERVISOR_VIRT_START)-1;
    Waddr pseudo_phys = (HYPERVISOR_VIRT_START - PHYS_VIRT_BASE) + (rawvirt & hypervisor_space_mask);

    Level1PTE pte = 0;
    pte.mfn = pseudo_phys >> 12;
    pte.p = 1;
    pte.rw = 0;
    pte.us = 1;
    pte.a = 1; // don't try to update accessed bits again
    pte.d = 0;

    return pte;
  }

  if unlikely ((rawvirt >= PTLSIM_RESERVED_VIRT_BASE) & (rawvirt <= PTLSIM_RESERVED_VIRT_END)) {
    // PTLsim space is inaccessible to the guest
    Level1PTE pte = 0;
    return pte;
  }

  Level4PTE& level4 = ((Level4PTE*)phys_to_mapped_virt(toplevel_mfn << 12))[virt.lm.level4];
  Level1PTE final = (W64)level4;

  if unlikely (!level4.p) return final;
  acc_bit_up_to_date = level4.a;

  Level3PTE& level3 = ((Level3PTE*)phys_to_mapped_virt(level4.mfn << 12))[virt.lm.level3];
  final.accum(level3);
  if unlikely (!level3.p) return final;
  acc_bit_up_to_date &= level3.a;

  Level2PTE& level2 = ((Level2PTE*)phys_to_mapped_virt(level3.mfn << 12))[virt.lm.level2];
  final.accum(level2);
  if (unlikely(!level2.p)) return final;
  acc_bit_up_to_date &= level2.a;

  if unlikely (level2.psz) {
    final.mfn = level2.mfn;
    final.pwt = level2.pwt;
    final.pcd = level2.pcd;
    acc_bit_up_to_date &= level2.a;

    final.a = acc_bit_up_to_date;
    final.d = level2.d;

    return final;
  }

  Level1PTE& level1 = ((Level1PTE*)phys_to_mapped_virt(level2.mfn << 12))[virt.lm.level1];
  final.accum(level1);
  if unlikely (!level1.p) return final;
  acc_bit_up_to_date &= level1.a;

  final.mfn = level1.mfn;
  final.g = level1.g;
  final.pat = level1.pat;
  final.pwt = level1.pwt;
  final.pcd = level1.pcd;
  final.a = acc_bit_up_to_date;
  final.d = level1.d;

  if unlikely (final.mfn == bootinfo.shared_info_mfn) {
    final.mfn = (Waddr)ptl_virt_to_phys(&sshinfo) >> 12;
  }

  return final;
}

//
// Page table walk with debugging info:
//
Level1PTE page_table_walk_debug(W64 rawvirt, W64 toplevel_mfn, bool DEBUG) {
  ostream& os = logfile;

  VirtAddr virt(rawvirt);

  bool acc_bit_up_to_date = 0;

  if (DEBUG) os << "page_table_walk: rawvirt ", (void*)rawvirt, ", toplevel ", (void*)toplevel_mfn, endl, flush;

  if unlikely ((rawvirt >= HYPERVISOR_VIRT_START) & (rawvirt < xen_m2p_map_end)) {
    //
    // The access is inside Xen's address space. Xen will not let us even access the
    // page table entries it injects into every top-level page table page, and we
    // cannot map M2P pages like we do other physical pages. Because Xen does not
    // allow its internal page tables to be mapped by guests at all, we have to
    // special-case these virtual addresses.
    //
    // We cheat by biasing the returned physical address such that we have
    // (HYPERVISOR_VIRT_START - PHYS_VIRT_BASE) + PHYS_VIRT_BASE == HYPERVISOR_VIRT_START
    // when other parts of PTLsim use ptl_phys_to_virt to access the memory.
    //
    const Waddr hypervisor_space_mask = (HYPERVISOR_VIRT_END - HYPERVISOR_VIRT_START)-1;
    Waddr pseudo_phys = (HYPERVISOR_VIRT_START - PHYS_VIRT_BASE) + (rawvirt & hypervisor_space_mask);

    if (DEBUG) os << "page_table_walk: special case (inside M2P map): pseudo_phys ", (void*)pseudo_phys, endl, flush;

    Level1PTE pte = 0;
    pte.mfn = pseudo_phys >> 12;
    pte.p = 1;
    pte.rw = 0;
    pte.us = 1;
    pte.a = 1; // don't try to update accessed bits again
    pte.d = 0;

    return pte;
  }

  Level4PTE& level4 = ((Level4PTE*)phys_to_mapped_virt(toplevel_mfn << 12))[virt.lm.level4];
  if (DEBUG) os << "  level4 @ ", &level4, " (mfn ", ((((Waddr)&level4) & 0xffffffff) >> 12), ", entry ", virt.lm.level4, ")", endl, flush;
  Level1PTE final = (W64)level4;

  if unlikely (!level4.p) return final;
  acc_bit_up_to_date = level4.a;

  Level3PTE& level3 = ((Level3PTE*)phys_to_mapped_virt(level4.mfn << 12))[virt.lm.level3];
  if (DEBUG) os << "  level3 @ ", &level3, " (mfn ", ((((Waddr)&level3) & 0xffffffff) >> 12), ", entry ", virt.lm.level3, ")", endl, flush;
  final.accum(level3);
  if unlikely (!level3.p) return final;
  acc_bit_up_to_date &= level3.a;

  Level2PTE& level2 = ((Level2PTE*)phys_to_mapped_virt(level3.mfn << 12))[virt.lm.level2];
  if (DEBUG) os << "  level2 @ ", &level2, " (mfn ", ((((Waddr)&level2) & 0xffffffff) >> 12), ", entry ", virt.lm.level2, ")", endl, flush;
  final.accum(level2);
  if unlikely (!level2.p) return final;
  acc_bit_up_to_date &= level2.a;

  if unlikely (level2.psz) {
    final.mfn = level2.mfn;
    final.pwt = level2.pwt;
    final.pcd = level2.pcd;
    acc_bit_up_to_date &= level2.a;

    final.a = acc_bit_up_to_date;
    final.d = level2.d;

    return final;
  }

  Level1PTE& level1 = ((Level1PTE*)phys_to_mapped_virt(level2.mfn << 12))[virt.lm.level1];
  if (DEBUG) os << "  level1 @ ", &level1, " (mfn ", ((((Waddr)&level1) & 0xffffffff) >> 12), ", entry ", virt.lm.level1, ")", endl, flush;
  final.accum(level1);
  if unlikely (!level1.p) return final;
  acc_bit_up_to_date &= level1.a;

  final.mfn = level1.mfn;
  final.g = level1.g;
  final.pat = level1.pat;
  final.pwt = level1.pwt;
  final.pcd = level1.pcd;
  final.a = acc_bit_up_to_date;
  final.d = level1.d;

  if unlikely (final.mfn == bootinfo.shared_info_mfn) {
    final.mfn = (Waddr)ptl_virt_to_phys(&sshinfo) >> 12;
    if (DEBUG) os << "  Remap shinfo access from real mfn ", bootinfo.shared_info_mfn,
                 " to PTLsim virtual shinfo page mfn ", final.mfn, " (virt ", &sshinfo, ")", endl, flush;
  }

  if (DEBUG) os << "  Final PTE for virt ", (void*)(Waddr)rawvirt, ": ", final, endl, flush;

  return final;
}

//
// Walk the page table, but return the physical address of the PTE itself
// that maps the specified virtual address
//
Waddr virt_to_pte_phys_addr(W64 rawvirt, W64 toplevel_mfn) {
  static const bool DEBUG = 0;
  VirtAddr virt(rawvirt);

  if (unlikely((rawvirt >= HYPERVISOR_VIRT_START) & (rawvirt < xen_m2p_map_end))) return 0;

  Level4PTE& level4 = ((Level4PTE*)phys_to_mapped_virt(toplevel_mfn << 12))[virt.lm.level4];
  if (DEBUG) logfile << "  level4 @ ", &level4, " (mfn ", ((((Waddr)&level4) & 0xffffffff) >> 12), ", entry ", virt.lm.level4, ")", endl, flush;
  if (unlikely(!level4.p)) return 0;

  Level3PTE& level3 = ((Level3PTE*)phys_to_mapped_virt(level4.mfn << 12))[virt.lm.level3];
  if (DEBUG) logfile << "  level3 @ ", &level3, " (mfn ", ((((Waddr)&level3) & 0xffffffff) >> 12), ", entry ", virt.lm.level3, ")", endl, flush;
  if (unlikely(!level3.p)) return 0;

  Level2PTE& level2 = ((Level2PTE*)phys_to_mapped_virt(level3.mfn << 12))[virt.lm.level2];
  if (DEBUG) logfile << "  level2 @ ", &level2, " (mfn ", ((((Waddr)&level2) & 0xffffffff) >> 12), ", entry ", virt.lm.level2, ") [pte ", level2, "]", endl, flush;
  if (unlikely(!level2.p)) return 0;

  if (unlikely(level2.psz)) return ((Waddr)&level2) - PHYS_VIRT_BASE;

  Level1PTE& level1 = ((Level1PTE*)phys_to_mapped_virt(level2.mfn << 12))[virt.lm.level1];
  if (DEBUG) logfile << "  level1 @ ", &level1, " (mfn ", ((((Waddr)&level1) & 0xffffffff) >> 12), ", entry ", virt.lm.level1, ")", endl, flush;

  return ((Waddr)&level1) - PHYS_VIRT_BASE;
}

//
// Walk the specified page table tree and update the accessed
// (and optionally dirty) bits as we go.
//
// Technically this could be done transparently by just accessing
// the specified virtual address, however we still explicitly
// submit this as an update queue to the hypervisor since we need
// to keep our simulated TLBs in sync.
//
void page_table_acc_dirty_update(W64 rawvirt, W64 toplevel_mfn, const PTEUpdate& update) {
  static const bool DEBUG = 0;

  VirtAddr virt(rawvirt);

  if (unlikely((rawvirt >= HYPERVISOR_VIRT_START) & (rawvirt < xen_m2p_map_end))) return;

  if (logable(5)) logfile << "Update acc/dirty bits: ", update.a, " ", update.d, " for virt ", (void*)rawvirt, endl;

  Level4PTE& level4 = ((Level4PTE*)phys_to_mapped_virt(toplevel_mfn << 12))[virt.lm.level4];
  if unlikely (!level4.p) return;
  if unlikely (!level4.a) { if (DEBUG) logfile << "level4 @ ", &level4, " <= ", level4.A(1), endl; level4 <= level4.A(1); }

  Level3PTE& level3 = ((Level3PTE*)phys_to_mapped_virt(level4.mfn << 12))[virt.lm.level3];
  if unlikely (!level3.p) return;
  if unlikely (!level3.a) { if (DEBUG) logfile << "level3 @ ", &level3, " <= ", level3.A(1), endl; level3 <= level3.A(1); }

  Level2PTE& level2 = ((Level2PTE*)phys_to_mapped_virt(level3.mfn << 12))[virt.lm.level2];
  if unlikely (!level2.p) return;
  if unlikely (!level2.a) { if (DEBUG) logfile << "level2 @ ", &level2, " <= ", level2.A(1), endl; level2 <= level2.A(1); }

  if unlikely (level2.psz) {
    if unlikely (update.d & (!level2.d)) { if (DEBUG) logfile << "level2 @ ", &level2, " <= ", level2.D(1), endl; level2 <= level2.D(1); }
    return;
  }

  Level1PTE& level1 = ((Level1PTE*)phys_to_mapped_virt(level2.mfn << 12))[virt.lm.level1];
  if unlikely (!level1.p) return;
  if unlikely (!level1.a) { if (DEBUG) logfile << "level1 @ ", &level1, " <= ", level1.A(1), endl; level1 <= level1.A(1); }
  if unlikely (update.d & (!level1.d)) { if (DEBUG) logfile << "level1 @ ", &level1, " <= ", level1.D(1), endl; level1 <= level1.D(1); }

  commit_page_table_updates();
}

//
// Loads and Stores
//

//
// Find out of the specified mfn is a valid page
// in main memory (and hence can be a page table)
// or if false, it's either invalid or is part
// of the Xen reserved space (equivalent to being
// mapped as a ROM or memory mapped device)
//
bool is_mfn_mainmem(mfn_t mfn) {
  return (mfn < bootinfo.total_machine_pages);
}

//
// Find out if the specified mfn needs to commit stores
// through Xen (to do a checked page table update)
// rather than direct memory stores. 
//
bool is_mfn_ptpage(mfn_t mfn) {
  if unlikely (!is_mfn_mainmem(mfn)) {
    // logfile << "Invalid MFN ", mfn, " (", sim_cycle, " cycles, ", total_user_insns_committed, " commits)", endl, flush;
    return false;
  }

  Level1PTE& pte = phys_pagedir[mfn];

  if unlikely (!pte.p) {
    //
    // The page has never been accessed before.
    // Pretend we're reading from it so PTLsim's page fault handler
    // will fault it in for us.
    //
    map_phys_page(mfn);
    if unlikely (!pte.p) {
      // This should never occur: errors are caught while mapping
      logfile << "PTE for mfn ", mfn, " is still not present (around sim_cycle ", sim_cycle, ")!", endl, flush;
      abort();
    }
  } else if unlikely (!pte.rw) {
    //
    // Try to promote to writable:
    //

    if likely (update_ptl_pte(pte, pte.W(1)) == 0) {
      if (logable(2)) {
        logfile << "[PTLsim Writeback Handler: promoted read-only L1 PTE for guest mfn ",
          mfn, " to writable (", sim_cycle, " cycles, ", total_user_insns_committed, " commits)", endl;
      }
    } else {
      // Could not promote: really is a pinned page table page (need mmu_update hypercall to update it)
    }
  }

  return (!pte.rw);
}

W64 storemask(Waddr physaddr, W64 data, byte bytemask) {
  W64& mem = *(W64*)phys_to_mapped_virt(physaddr);
  W64 merged = mux64(expand_8bit_to_64bit_lut[bytemask], mem, data);
  mfn_t mfn = physaddr >> 12;

  if unlikely (!is_mfn_mainmem(mfn)) {
    //
    // Physical address is inside of PTLsim: apply directly.
    //
    // Technically the address could also be inside of Xen,
    // however the guest itself is literally unable to generate
    // such an address through page tables, so we don't need
    // to worry about that here.
    //
    mem = merged;
  } else if unlikely (is_mfn_ptpage(mfn)) {
    //
    // MFN is read-only and could not be promoted to
    // writable: force Xen to do the store for us
    //
    mmu_update_t u;
    u.ptr = physaddr;
    u.val = merged;
    int rc = HYPERVISOR_mmu_update(&u, 1, NULL, DOMID_SELF);
    if unlikely (rc) {
      logfile << "storemask: WARNING: store to physaddr ", (void*)physaddr, " <= ", Level1PTE(data), " failed with rc ", rc, endl, flush;
    }
  } else {
    mem = merged;
  }

  return data;
}

void* Context::check_and_translate(Waddr virtaddr, int sizeshift, bool store, bool internal, int& exception, PageFaultErrorCode& pfec, PTEUpdate& pteupdate) {
  exception = 0;
  pteupdate = 0;

  pfec = 0;

  if unlikely (lowbits(virtaddr, sizeshift)) {
    exception = EXCEPTION_UnalignedAccess;
    return null;
  }

  if unlikely (internal) {
    //
    // Directly mapped to PTL space (microcode load/store)
    // We need to patch in PTLSIM_VIRT_BASE since in 32-bit
    // mode, ctx.virt_addr_mask will chop off these bits.
    //
    return (void*)(lowbits(virtaddr, 32) | PTLSIM_VIRT_BASE);
  }

  Level1PTE pte = virt_to_pte(virtaddr);

  bool page_not_present = (!pte.p);
  bool page_read_only = (store & (!pte.rw));
  bool page_kernel_only = ((!kernel_mode) & (!pte.us));

  if unlikely (page_not_present | page_read_only | page_kernel_only) {
    if unlikely (store && (!page_not_present) && (!page_kernel_only) &&
                 page_read_only && is_mfn_mainmem(pte.mfn) && is_mfn_ptpage(pte.mfn)) {
      if (logable(5)) {
        logfile << "Page is a page table page: special semantics", endl;
      }
      //
      // This is a page table page and is technically mapped read only,
      // but the user code has attempted to store to it anyway under the
      // assumption that the hypervisor will trap the store, validate the
      // written PTE value and emulate the store as if it was to a normal
      // read-write page.
      //
      // For PTLsim use, we set the pteupdate.ptwrite bit to indicate that
      // special handling is needed. However, no exception is signalled.
      //
      pteupdate.ptwrite = 1;
    } else {
      exception = (store) ? EXCEPTION_PageFaultOnWrite : EXCEPTION_PageFaultOnRead;
      pfec.p = pte.p;
      pfec.rw = store;
      pfec.us = (!kernel_mode);
    }

    if (exception) return null;
  }

  pteupdate.a = (!pte.a);
  pteupdate.d = (store & (!pte.d));

  return pte_to_mapped_virt(virtaddr, pte);
}

int Context::copy_from_user(void* target, Waddr source, int bytes, PageFaultErrorCode& pfec, Waddr& faultaddr, bool forexec) {
  Level1PTE pte;

  int n = 0;

  pfec = 0;
  pte = virt_to_pte(source);

  if unlikely ((!pte.p) | (forexec & pte.nx) | ((!kernel_mode) & (!pte.us))) {
    faultaddr = source;
    pfec.p = pte.p;
    pfec.nx = forexec;
    pfec.us = (!kernel_mode);
    return 0;
  }

  n = min(4096 - lowbits(source, 12), (Waddr)bytes);
  memcpy(target, pte_to_mapped_virt(source, pte), n);

  PTEUpdate pteupdate = 0;
  pteupdate.a = 1;

  if unlikely (!pte.a) update_pte_acc_dirty(source, pteupdate);

  // All the bytes were on the first page
  if likely (n == bytes) return n;

  // Go on to second page, if present
  pte = virt_to_pte(source + n);
  if unlikely ((!pte.p) | (forexec & pte.nx) | ((!kernel_mode) & (!pte.us))) {
    faultaddr = source + n;
    pfec.p = pte.p;
    pfec.nx = forexec;
    pfec.us = (!kernel_mode);
    return n;
  }

  if (!pte.a) update_pte_acc_dirty(source + n, pteupdate);

  memcpy((byte*)target + n, pte_to_mapped_virt(source + n, pte), bytes - n);
  n = bytes;
  return n;
}

int Context::copy_to_user(Waddr target, void* source, int bytes, PageFaultErrorCode& pfec, Waddr& faultaddr) {
  Level1PTE pte;

  pfec = 0;
  pte = virt_to_pte(target);
  if unlikely ((!pte.p) | (!pte.rw) | ((!kernel_mode) & (!pte.us))) {
    faultaddr = target;
    pfec.p = pte.p;
    pfec.rw = 1;
    pfec.us = (!kernel_mode);
    return 0;
  }

  byte* targetlo = (byte*)pte_to_mapped_virt(target, pte);
  int nlo = min(4096 - lowbits(target, 12), (Waddr)bytes);

  PTEUpdate pteupdate = 0;
  pteupdate.a = 0;
  pteupdate.d = 1;
  if unlikely ((!pte.a) | (!pte.d)) update_pte_acc_dirty(target, pteupdate);

  // All the bytes were on the first page
  if likely (nlo == bytes) {
    memcpy(targetlo, source, nlo);
    return bytes;
  }

  // Go on to second page, if present
  pte = virt_to_pte(target + nlo);
  if unlikely ((!pte.p) | (!pte.rw) | ((!kernel_mode) & (!pte.us))) {
    faultaddr = target + nlo;
    pfec.p = pte.p;
    pfec.rw = 1;
    pfec.us = (!kernel_mode);
    return nlo;
  }

  if unlikely ((!pte.a) | (!pte.d)) update_pte_acc_dirty(target + nlo, pteupdate);

  memcpy(pte_to_mapped_virt(target + nlo, pte), (byte*)source + nlo, bytes - nlo);
  memcpy(targetlo, source, nlo);

  return bytes;
}

//
// Why we need to always track both MFNs:
// Example of ambiguity:
//
// - Pair of proceses (A and B)
// - Page 1 is mapped to mfn X in both A and B
// - Page 2 is mapped to mfn Y in A and mfn Z in B
// - BB crosses 1-to-2 page boundary at same virt addr in both A and B
// - Meaning of instruction is different depending only on those
//   bytes in page 2 (mfn Y or Z)
//

RIPVirtPhys& RIPVirtPhys::update(Context& ctx, int bytes) {
  Level1PTE pte;
  bool invalid;

  use64 = ctx.use64;
  kernel = ctx.kernel_mode;
  df = ((ctx.internal_eflags & FLAG_DF) != 0);
  padlo = 0;
  padhi = 0;

  pte = ctx.virt_to_pte(rip);
  invalid = ((!pte.p) | pte.nx | ((!ctx.kernel_mode) & (!pte.us)));
  mfnlo = (invalid) ? INVALID : pte.mfn;
  mfnhi = mfnlo;

  int page_crossing = ((lowbits(rip, 12) + (bytes-1)) >> 12);

  //
  // Since table lookups only know the RIP of the target and not
  // its size, we don't know if there is a page crossing. Hence,
  // we always assume there is. BB translation (case above) may
  // be more optimized, only doing this if the pages are truly
  // different.
  //
  //++MTY TODO:
  // If BBs are terminated at the first insn to cross a page,
  // technically we could get away with only checking if the
  // byte at rip + (15-1) would hit the next page.
  //

  if unlikely (page_crossing) {
    pte = ctx.virt_to_pte(rip + (bytes-1));
    invalid = ((!pte.p) | pte.nx | ((!ctx.kernel_mode) & (!pte.us)));
    mfnhi = (invalid) ? INVALID : pte.mfn;
  }

  return *this;
}

//
// Self modifying code support
//

void smc_setdirty_internal(Level1PTE& pte, bool dirty) {
  if (logable(5)) logfile << "smc_setdirty_internal(", &pte, " [", pte, "], dirty ", dirty, ")", endl, flush;
  assert(update_ptl_pte(pte, pte.D(dirty)) == 0);
}

//
// Memory hypercalls
//
#define getreq(type) type req; if (ctx.copy_from_user(&req, (Waddr)arg, sizeof(type)) != sizeof(type)) { return -EFAULT; }
#define putreq(type) ctx.copy_to_user((Waddr)arg, &req, sizeof(type))

W64 handle_mmu_update_hypercall(Context& ctx, mmu_update_t* reqp, W64 count, int* total_updates_ptr, domid_t domain, bool debug) {
  mmu_update_t req;
  
  int rc;
  int total_updates = 0;
  foreach (i, count) {
    int n = ctx.copy_from_user(&req, (Waddr)&reqp[i], sizeof(mmu_update_t));
    if (n < sizeof(mmu_update_t)) break;
    mfn_t mfn = req.ptr >> 12;
    if (mfn >= bootinfo.total_machine_pages) {
      if (debug) logfile << "  mfn out of range (", bootinfo.total_machine_pages, ")", endl, flush;
      continue;
    }
    
    //
    // If we're updating an L4/L3/L2 page and the new PTE data specifies
    // a page we currently have mapped read/write, we must unmap it first
    // since Xen will not let the page table page reference it otherwise.
    //
    // The actual mfn we're modifying must already be a page table page;
    // hence we would only have a read only mapping of it anyway.
    //
    
    Level1PTE newpte(req.val);
    
    if (debug) logfile << "mmu_update: mfn ", mfn, " + ", (void*)(Waddr)lowbits(req.ptr, 12), " (entry ", (lowbits(req.ptr, 12) >> 3), ") <= ", newpte, endl, flush;
    
    if (newpte.p) {
      unmap_phys_page(newpte.mfn);
      // (See notes about DMA and self-modifying code for update_va_mapping)
      //++MTY FIXME We need a more intelligent mechanism to avoid constant flushing
      // as new processes are started.
      if unlikely ((!smc_isdirty(newpte.mfn)) && (bbcache.get_page_bb_count(newpte.mfn) > 0)) {
        if (debug) logfile << "  Target mfn ", newpte.mfn, " was clean and had ", bbcache.get_page_bb_count(newpte.mfn), " cached translations; making dirty", endl;
        smc_setdirty(newpte.mfn);
      }
    }
    
    int update_count;
    rc = HYPERVISOR_mmu_update(&req, 1, &update_count, domain);
    total_updates += update_count;
    
    if (rc) break;
  }
  
  ctx.flush_tlb();
  ctx.copy_to_user((Waddr)total_updates_ptr, &total_updates, sizeof(int));
  return rc;
}

W64 handle_update_va_mapping_hypercall(Context& ctx, W64 va, Level1PTE newpte, W64 flags, bool debug) {
  Waddr ptephys = virt_to_pte_phys_addr(va, ctx.cr3 >> 12);
  if (!ptephys) {
    if (debug) logfile << "update_va_mapping: va ", (void*)va, " using toplevel mfn ", (ctx.cr3 >> 12), ": cannot resolve PTE address", endl, flush;
    return W64(-EINVAL);
  }
    
  if (debug) logfile << "update_va_mapping: va ", (void*)va, " using toplevel mfn ", (ctx.cr3 >> 12),
               " -> pte @ phys ", (void*)ptephys, ") <= ", newpte, ", flags ", (void*)(Waddr)flags,
               " (flushtype ", (flags & UVMF_FLUSHTYPE_MASK), ")", endl, flush;
  
  if (flags & ~UVMF_FLUSHTYPE_MASK) {
    Waddr* flush_bitmap_ptr = (Waddr*)(flags & ~UVMF_FLUSHTYPE_MASK);
    // pointer was specified: get it and thunk the address
    Waddr flush_bitmap;
    if (ctx.copy_from_user(&flush_bitmap, (Waddr)flush_bitmap_ptr, sizeof(flush_bitmap)) != sizeof(flush_bitmap)) {
      if (debug) logfile << "update_va_mapping: va ", (void*)va, "; flush bitmap ptr ", flush_bitmap_ptr, " not accessible", endl, flush;
      return W64(-EFAULT);
    }
    flags = (((Waddr)&flush_bitmap) & ~UVMF_FLUSHTYPE_MASK) | (flags & UVMF_FLUSHTYPE_MASK);
    if (debug) logfile << "Copied flush bitmap ", bitstring(flush_bitmap, 64, true), "; new flags ", hexstring(flags, 64), endl, flush;
  }
  
  int targetmfn = newpte.mfn;
  
  // if (debug) logfile << "  Old PTE: ", *(Level1PTE*)phys_to_mapped_virt(ptephys), endl, flush;
  
  int rc = update_phys_pte(ptephys, newpte);

  if likely (newpte.p) {
    //
    // External DMA handling:
    //
    // Currently we have no way to track virtual DMAs
    // from backend drivers outside the domain. However,
    // we *do* know when newly DMA'd pages are mapped
    // into some address space; we simply invalidate
    // all cached BBs on the target page. Since we never
    // load kernel space code via DMA, we always have
    // to map the page into some user process before
    // using it for the first time; hence this works.
    //
    //++MTY TODO: add hooks into dom0 drivers to pass us
    // an invalidation event or set a bit in a shared memory
    // bitmap whenever pages shared with the target domain
    // are written. This is the only reliable way.
    //
    //++MTY FIXME We need a more intelligent mechanism to avoid constant flushing
    // as new processes are started.      
    if ((!smc_isdirty(targetmfn)) && (bbcache.get_page_bb_count(targetmfn) > 0)) {
      if (debug) logfile << "  Target mfn ", targetmfn, " was clean and had ", bbcache.get_page_bb_count(targetmfn), " cached translations; making dirty", endl;
      smc_setdirty(targetmfn);
    }
  }
  
  // if (debug) logfile << "  New PTE: ", *(Level1PTE*)phys_to_mapped_virt(ptephys), " (rc ", rc, ")", endl, flush;
  
  if (flags & UVMF_FLUSHTYPE_MASK) {
    foreach (i, contextcount) {
      contextof(i).flush_tlb_virt(va);
    }
  }

  return rc;
}

ostream& print(ostream& os, const xen_memory_reservation_t& req, Context& ctx) {
  os << "{nr_extents = ", req.nr_extents, ", ", "extent_order = ", ((1 << req.extent_order) * 4096), " bytes, address_bits = ", req.address_bits, ", frames:";
  pfn_t* extents = (pfn_t*)req.extent_start.p;
  foreach (i, req.nr_extents) {
    pfn_t pfn;
    int n = ctx.copy_from_user(&pfn, (Waddr)&extents[i], sizeof(pfn));
    os << " ";
    if likely (n == sizeof(pfn)) os << pfn; else os << "???";
  }
  os << "}";
  return os;
}

W64 handle_memory_op_hypercall(Context& ctx, W64 op, void* arg, bool debug) {
  int rc = 0;
  switch (op) {
  case XENMEM_machphys_mapping: {
    xen_machphys_mapping_t req;
    rc = HYPERVISOR_memory_op(XENMEM_machphys_mapping, &req);
    if (debug) {
      logfile << "memory_op (machphys_mapping): ",
        (void*)(Waddr)arg, " <= {", (void*)(Waddr)req.v_start, ", ",
        (void*)(Waddr)req.v_end, ", ", (void*)(Waddr)req.max_mfn, "} rc ", rc , endl;
    }
    putreq(req);
    break;
  }
  case XENMEM_memory_map: {
    getreq(xen_memory_map_t);
    unsigned int orig_nr_entries = req.nr_entries;
    rc = HYPERVISOR_memory_op(XENMEM_memory_map, &req);
    //++MTY CHECKME should we fixup the memory map to exclude the PTLsim reserved area?
    if (debug) {
      logfile << "memory_op (memory_map): {nr_entries = ", orig_nr_entries,
        ", buffer = ", req.buffer.p, "} => rc ", rc, ", ", req.nr_entries, " entries filled", endl;
    }
    putreq(xen_memory_map_t);
    break;
  }
  case XENMEM_populate_physmap: {
    getreq(xen_memory_reservation_t);
    if (debug) { logfile << "memory_op (populate_physmap): in "; print(logfile, req, ctx); logfile << endl; }
    rc = HYPERVISOR_memory_op(XENMEM_populate_physmap, &req);
    if (debug) { logfile << "  populate_physmap: rc ", rc, " out "; print(logfile, req, ctx); logfile << endl; }
    putreq(xen_memory_reservation_t);
    break;
  }
  case XENMEM_increase_reservation: {
    getreq(xen_memory_reservation_t);
    if (debug) { logfile << "memory_op (increase_reservation): in "; print(logfile, req, ctx); logfile << endl; }
    rc = HYPERVISOR_memory_op(XENMEM_increase_reservation, &req);
    if (debug) { logfile << "  increase_reservation: rc ", rc, " out "; print(logfile, req, ctx); logfile << endl; }
    putreq(xen_memory_reservation_t);
    break;
  }
  case XENMEM_decrease_reservation: {
    //++MTY CHECKME we first need to unmap the specified MFNs from PTLsim if they were mapped!
    getreq(xen_memory_reservation_t);
    if (debug) { logfile << "memory_op (decrease_reservation): in "; print(logfile, req, ctx); logfile << endl; }

    //
    // We must unmap 
    //
    mfn_t* extents = (pfn_t*)req.extent_start.p;
    foreach (i, req.nr_extents) {
      unsigned int pages_in_extent = 1 << req.extent_order;
      mfn_t basemfn;
      int n = ctx.copy_from_user(&basemfn, (Waddr)&extents[i], sizeof(basemfn));
      if unlikely (!n) continue;

      foreach (j, pages_in_extent) {
        mfn_t mfn = basemfn + j;
        if unlikely (mfn >= bootinfo.total_machine_pages) break;
        Level1PTE& pte = phys_pagedir[mfn];
        if likely (pte.p) {
          pte <= pte.P(0);
          if unlikely (debug) logfile << "  Unmap mfn ", mfn, endl;
        }
      }
    }

    commit_page_table_updates();

    rc = HYPERVISOR_memory_op(XENMEM_decrease_reservation, &req);
    if (debug) { logfile << "  decrease_reservation: rc ", rc, " out "; print(logfile, req, ctx); logfile << endl; }
    putreq(xen_memory_reservation_t);
    break;
  }
  default: {
    // All others are only used by dom0
    logfile << "memory_op (", op, ") not supported!", endl, flush;
    abort();
  }
  }

  return rc;
}

W64 handle_mmuext_op_hypercall(Context& ctx, mmuext_op_t* reqp, W64 count, int* total_updates_ptr, domid_t domain, bool debug) {
  mmuext_op_t req;
  int rc = 0;
  int total_updates = 0;

  foreach (i, count) {
    int n = ctx.copy_from_user(&req, (Waddr)&reqp[i], sizeof(mmuext_op_t));
    if (n < sizeof(mmuext_op_t)) break;

    switch (req.cmd) {
    case MMUEXT_PIN_L1_TABLE:
    case MMUEXT_PIN_L2_TABLE:
    case MMUEXT_PIN_L3_TABLE:
    case MMUEXT_PIN_L4_TABLE:
    case MMUEXT_UNPIN_TABLE: {
      mfn_t mfn = req.arg1.mfn;
      if (mfn >= bootinfo.total_machine_pages) continue;

      //
      // Unmap the requisite pages from our physmap since we may be making them read only.
      // It will be remapped by the PTLsim page fault handler on demand.
      //
      const Level1PTE* pinptes = (Level1PTE*)phys_to_mapped_virt(mfn << 12);
      Level1PTE pte0 = pinptes[0];

      if (debug) logfile << "mmuext_op: map/unmap mfn ", mfn, " (pin/unpin operation ", req.cmd, ")", endl, flush;

      if (req.cmd != MMUEXT_UNPIN_TABLE) {
        // Unmapping only required when pinning, not unpinning
        // It's actually more efficient to just unmap everything:
        // constant time (1 L2 page scan) for systems with only a
        // few GB of physical memory:
        // (slower) unmap_phys_page_tree(mfn);
        unmap_address_space();
      }

      int update_count = 0;
      rc = HYPERVISOR_mmuext_op(&req, 1, &update_count, domain);

      total_updates += update_count;
      break;
    }
    case MMUEXT_NEW_BASEPTR: {
      if (debug) logfile << "mmuext_op: new kernel baseptr is mfn ",
                   req.arg1.mfn, " on vcpu ", ctx.vcpuid, ")", endl, flush;
      unmap_phys_page(req.arg1.mfn);
      ctx.kernel_ptbase_mfn = req.arg1.mfn;
      ctx.cr3 = ctx.kernel_ptbase_mfn << 12;
      ctx.flush_tlb();
      // switch_page_table(ctx.cr3 >> 12);
      total_updates++;
      rc = 0;
      break;
    }
    case MMUEXT_TLB_FLUSH_LOCAL:
    case MMUEXT_INVLPG_LOCAL: {
      bool single = (req.cmd == MMUEXT_INVLPG_LOCAL);
      if (debug) logfile << "mmuext_op: ", (single ? "invlpg" : "flush"), " local (vcpu ", ctx.vcpuid, ") @ ",
                   (void*)(Waddr)req.arg1.linear_addr, endl, flush;
      if (single)
        ctx.flush_tlb_virt(req.arg1.linear_addr);
      else ctx.flush_tlb();
      total_updates++;
      rc = 0;
      break;
    }
    case MMUEXT_TLB_FLUSH_MULTI:
    case MMUEXT_INVLPG_MULTI: {
      Waddr vcpumask;
      int n = ctx.copy_from_user(&vcpumask, *(Waddr*)&req.arg2.vcpumask, sizeof(vcpumask));
      if (n != sizeof(vcpumask)) { rc = -EFAULT; break; }
      bool single = (req.cmd == MMUEXT_INVLPG_MULTI);
      if (debug) logfile << "mmuext_op: ", (single ? "invlpg" : "flush"), " multi (mask ", 
                   bitstring(vcpumask, contextcount), " @ ", (void*)(Waddr)req.arg1.linear_addr, ")", endl, flush;
      if (single) {
        foreach (i, contextcount) {
          if (bit(vcpumask, i)) contextof(i).flush_tlb_virt(req.arg1.linear_addr);
        }
      } else {
        foreach (i, contextcount) {
          if (bit(vcpumask, i)) contextof(i).flush_tlb();
        }
      }
      total_updates++;
      rc = 0;
      break;
    }
    case MMUEXT_TLB_FLUSH_ALL:
    case MMUEXT_INVLPG_ALL: {
      bool single = (req.cmd == MMUEXT_INVLPG_ALL);
      if (debug) logfile << "mmuext_op: ", (single ? "invlpg" : "flush"), " all @ ",
                   (void*)(Waddr)req.arg1.linear_addr, endl, flush;
      if (single) {
        foreach (i, contextcount) contextof(i).flush_tlb_virt(req.arg1.linear_addr);
      } else {
        foreach (i, contextcount) contextof(i).flush_tlb();
      }
      total_updates++;
      rc = 0;
      break;
    }
    case MMUEXT_FLUSH_CACHE: {
      if (debug) logfile << "mmuext_op: flush_cache on vcpu ", ctx.vcpuid, endl, flush;
      total_updates++;
      rc = 0;
      break;
    }
    case MMUEXT_SET_LDT: {
      ctx.ldtvirt = req.arg1.linear_addr;
      ctx.ldtsize = req.arg2.nr_ents;

      if (debug) logfile << "mmuext_op: set_ldt to virt ", (void*)(Waddr)ctx.ldtvirt, " with ",
                   ctx.ldtsize, " entries on vcpu ", ctx.vcpuid, endl, flush;

      total_updates++;
      rc = 0;
      break;
    }
    case MMUEXT_NEW_USER_BASEPTR: { // (x86-64 only)
      if (debug) logfile << "mmuext_op: new user baseptr is mfn ",
                   req.arg1.mfn, " on vcpu ", ctx.vcpuid, ")", endl, flush;
      ctx.user_ptbase_mfn = req.arg1.mfn;
      //
      // Since PTLsim runs in kernel mode at all times, we can pass this request
      // through to Xen so the guest domain gets the correct base pointer on return
      // to native mode.
      //
      // In simulation, we do not switch ctx.cr3 = ctx.user_cr3 until we return to
      // userspace (with iret hypercall).
      //
      int update_count = 0;
      rc = HYPERVISOR_mmuext_op(&req, 1, &update_count, domain);
      total_updates++;
      break;
    }
    default:
      if (debug) logfile << "mmuext_op: unknown op ", req.cmd, endl, flush;
      rc = -EINVAL;
      abort();
      break;
    }

    if (rc) break;
  }

  ctx.copy_to_user((Waddr)total_updates_ptr, &total_updates, sizeof(int));

  return rc;
}

W64 handle_grant_table_op_hypercall(Context& ctx, W64 cmd, byte* arg, W64 count, bool debug) {
  int rc = 0;

  foreach (i, count) {
    switch (cmd) {
      //
      // map_grant_ref and unmap_grant_ref have a flag that says GNTMAP_contains_pte
      // which tells Xen to update the specified PTE to map the granted page.
      // However, Linux does not use this flag; instead, Xen internally generates
      // the PTE address for us based on the current page table root. Since PTLsim
      // has its own page table in effect, we need to do the virt->PTE-to-modify mapping
      // ourselves, replace the host_addr field and add in the GNTMAP_contains_pte flag.
      //
      // Since we may not cohabitate the same virtual address space as the real page
      // table base at all times, we keep it in place for SMT or multi-core use since
      // switching the page table base every time may be too expensive and time consuming.
      //
    case GNTTABOP_map_grant_ref: {
      getreq(gnttab_map_grant_ref);
      if (debug) logfile << "GNTTABOP_map_grant_ref(host_addr ", (void*)(Waddr)req.host_addr, ", flags ", req.flags,
                   ", ref ", req.ref, ", dom ", req.dom, ")", endl;
      if (debug) logfile << "map_grant_ref is not supported yet!", endl;
      abort();
    }
    case GNTTABOP_unmap_grant_ref: {
      getreq(gnttab_map_grant_ref);
      if (debug) logfile << "GNTTABOP_unmap_grant_ref(host_addr ", (void*)(Waddr)req.host_addr,
                   ", dev_bus_addr ", (void*)(Waddr)req.dev_bus_addr, ", handle ", (void*)(Waddr)req.handle, ")", endl, flush;
      if (debug) logfile << "unmap_grant_ref is not supported yet!", endl;
      abort();
    }
    case GNTTABOP_setup_table: {
      getreq(gnttab_setup_table);
      unsigned long* orig_frame_list = req.frame_list.p;
      unsigned long frames[4]; // on x86 and x86-64, NR_GRANT_FRAMES is always 1<<2 == 4
      int framecount = min(req.nr_frames, (W32)lengthof(frames));
      req.frame_list.p = frames;
      if (debug) logfile << "GNTTABOP_setup_table(dom ", req.dom, ", nr_frames ", req.nr_frames, ", frame_list ", orig_frame_list, ")", endl, flush;
      rc = HYPERVISOR_grant_table_op(GNTTABOP_setup_table, &req, 1);
      req.frame_list.p = orig_frame_list;
      if (debug) { logfile << "  Frames:"; foreach (i, framecount) { logfile << " ", frames[i]; }; logfile << ", status ", req.status, endl, flush; }
      assert(ctx.copy_to_user((Waddr)orig_frame_list, &frames, framecount * sizeof(unsigned long)) == (framecount * sizeof(unsigned long)));
      putreq(gnttab_setup_table);
      arg += sizeof(req);
      break;
    }
    case GNTTABOP_transfer: {
      getreq(gnttab_transfer);
      ctx.flush_tlb();
      if (debug) logfile << "GNTTABOP_transfer(mfn ", req.mfn, ", domid ", req.domid, ", ref ", req.ref, ")", endl, flush;
      unmap_phys_page(req.mfn);
      rc = HYPERVISOR_grant_table_op(GNTTABOP_transfer, &req, 1);
      putreq(gnttab_transfer);
      arg += sizeof(req);
      break;
    }
    default: {
      if (debug) logfile << "grant_table_op: unknown op ", cmd, endl, flush;
      rc = -EINVAL;
      abort();
      break;
    }
    }
  }

  return rc;
}
