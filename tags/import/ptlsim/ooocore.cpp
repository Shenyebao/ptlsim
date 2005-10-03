//
// PTLsim: Cycle Accurate x86-64 Simulator
// Out-of-Order Core Simulator
//
// Copyright 2003-2005 Matt T. Yourst <yourst@yourst.com>
//

#include <globals.h>
#include <stdio.h>
#include <elf.h>
#include <asm/unistd.h>
#include <asm/prctl.h>
#include <sys/prctl.h>
#include <ptlsim.h>
#include <branchpred.h>
#include <datastore.h>
#include <logic.h>

// With these disabled, simulation is ~10-20% faster
//#define ENABLE_CHECKS
//#define ENABLE_LOGGING

#ifndef ENABLE_CHECKS
#undef assert
#define assert(x) (x)
#endif

#undef logable
#ifdef ENABLE_LOGGING
#define logable(level) (loglevel >= level)
#else
#define logable(level) (0)
#endif

#define ENABLE_SIM_TIMING
#ifdef ENABLE_SIM_TIMING
#define start_timer(ct) ct.start()
#define stop_timer(ct) ct.stop()
#else
#define start_timer(ct)
#define stop_timer(ct)
#endif

#define MAX_OPERANDS 4
#define RA 0
#define RB 1
#define RC 2
#define RS 3

void print_rob(ostream& os);
void print_lsq(ostream& os);
void check_rob();
void dump_ooo_state();
void check_refcounts();

struct ReorderBufferEntry;
void log_forwarding(const ReorderBufferEntry* source, const ReorderBufferEntry* target, int operand);

//
// Issue queue based scheduler with broadcast
//

template <int size, int operandcount = MAX_OPERANDS>
struct IssueQueue {
  typedef FullyAssociativeTags8bit<size, size> assoc_t;
  static const int SIZE = size;

  assoc_t uopids;
  assoc_t tags[operandcount];

  // States:
  //             V I
  // free        0 0
  // dispatched  1 0
  // issued      1 1
  // complete    0 1

  bitvec<size> valid;
  bitvec<size> issued;
  bitvec<size> allready;
  int count;

  bool remaining() const { return (size - count); }
  bool empty() const { return (!count); }
  bool full() const { return (!remaining()); }

  void reset() {
    count = 0;
    valid = 0;
    issued = 0;
    allready = 0;
    foreach (i, operandcount) {
      tags[i].reset();
    }
    uopids.reset();
  }

  void clock() {
    allready = (valid & (~issued));
    foreach (operand, operandcount) {
      allready &= ~tags[operand].valid;
    }
  }

  bool insert(byte uopid, const byte* operands, const byte* preready) {
    if (count == size)
      return false;

    assert(uopids.search(uopid) < 0);

    int slot = count++;
    assert(!bit(valid, slot));

    uopids.insertslot(slot, uopid);

    valid[slot] = 1;
    issued[slot] = 0;

    foreach (operand, operandcount) {
      if (preready[operand])
        tags[operand].invalidateslot(slot);
      else tags[operand].insertslot(slot, operands[operand]);
    }

    return true;
  }

  void tally_broadcast_matches(byte sourceid, const bitvec<size>& mask, int operand) const;

  bool broadcast(byte uopid) {
    vec16b tagvec = assoc_t::prep(uopid);

    if (logable(1)) {
      foreach (operand, operandcount) {
        bitvec<size> mask = tags[operand].invalidate(tagvec);
        tally_broadcast_matches(uopid, mask, operand);
      }
    } else {
      foreach (operand, operandcount) tags[operand].invalidate(tagvec);
    }
    return true;
  }

  int uopof(int slot) const {
    return uopids[slot];
  }

  int slotof(int uopid) const {
    return uopids.search(uopid);
  }

  //
  // Select one ready slot and move it to the issued state.
  // This function returns the slot id. The returned slot
  // id becomes invalid after the next call to remove()
  // before the next uop can be processed in any way.
  //
  int issue() {
    if (!allready) return -1;
    int slot = allready.lsb();
    issued[slot] = 1;
    return slot;
  }

  //
  // Replay a uop that has already issued once.
  // The caller may add or reset dependencies here as needed.
  //
  bool replay(int slot, const byte* operands, const byte* preready) {
    assert(valid[slot]);
    assert(issued[slot]);

    issued[slot] = 0;

    foreach (operand, operandcount) {
      if (preready[operand])
        tags[operand].invalidateslot(slot);
      else tags[operand].insertslot(slot, operands[operand]);
    }

    return true;
  }

  //
  // Replay a uop that has already issued once.
  // The caller may add or reset dependencies here as needed.
  //
  bool replay(int slot) {
    issued[slot] = 0;
    return true;
  }

  //
  // Remove an entry from the issue queue after it has completed,
  // or in the process of annulment.
  //
  bool release(int slot) {
    remove(slot);
    return true;
  }

  bool annul(int slot) {
    remove(slot);
    return true;
  }

  bool annuluop(int uopid) {
    int slot = slotof(uopid);
    if (slot < 0) return false;
    remove(slot);
    return true;
  }

  // NOTE: This is a fairly expensive operation:
  bool remove(int slot) {
    uopids.collapse(slot);

    foreach (i, operandcount) {
      tags[i].collapse(slot);
    }

    valid = valid.remove(slot, 1);
    issued = issued.remove(slot, 1);
    allready = allready.remove(slot, 1);

    count--;
    assert(count >= 0);
    return true;
  }

  ostream& print(ostream& os) const {
    os << "IssueQueue: count = ", count, ":", endl;
    foreach (i, size) {
      os << "  uop ";
      uopids.printid(os, i);
      os << ": ",
        ((valid[i]) ? 'V' : '-'), ' ',
        ((issued[i]) ? 'I' : '-'), ' ',
        ((allready[i]) ? 'R' : '-'), ' ';
      foreach (j, operandcount) {
        if (j) os << ' ';
        tags[j].printid(os, i);
      }
      os << endl;
    }
    return os;
  }
};

template <int size, int operandcount>
ostream& operator <<(ostream& os, const IssueQueue<size, operandcount>& issueq) {
  return issueq.print(os);
}

//
// Iterate through a linked list of objects where each object directly inherits
// only from the selfqueuelink class or otherwise has a selfqueuelink object
// as the first member.
//
// This iterator supports mutable lists, meaning the current entry (obj) may
// be safely removed from the list and/or moved to some other list without
// affecting the next object processed.
//
// This does NOT mean you can remove any object from the list other than the
// current object obj - to do this, copy the list of pointers to an array and
// then process that instead.
//
#define foreach_list_mutable_linktype(L, obj, entry, nextentry, linktype) \
  linktype* entry; \
  linktype* nextentry; \
  for (entry = (L).next, nextentry = entry->next, prefetch(entry->next), obj = (typeof obj)entry; \
    entry != &(L); entry = nextentry, nextentry = entry->next, prefetch(nextentry), obj = (typeof obj)entry)

#define foreach_list_mutable(L, obj, entry, nextentry) foreach_list_mutable_linktype(L, obj, entry, nextentry, selfqueuelink)

//
// Each ROB's state_link member can be linked into at most one of the
// following rob_xxx_list lists at any given time; the ROB's current_state_list
// points back to the list it belongs to.
//
struct StateList;

struct ListOfStateLists: public array<StateList*, 64> {
  int count;

  int add(StateList* list);
  void reset();
};

ListOfStateLists rob_states;
ListOfStateLists physreg_states;
ListOfStateLists lsq_states;

struct StateList: public selfqueuelink {
  const char* name;
  int count;
  int listid;
  W64 dispatch_source_counter;
  W64 issue_source_counter;

  StateList() { }

  StateList(const char* name, ListOfStateLists& lol) {
    reset();
    this->name = name;
    count = 0;
    listid = lol.add(this);
    dispatch_source_counter = 0;
    issue_source_counter = 0;
  }

  void reset() {
    selfqueuelink::reset();
    count = 0;
  }

  selfqueuelink* dequeue() {
    if (empty())
      return null;
    count--;
    selfqueuelink* obj = removehead();
    return obj;
  }

  selfqueuelink* enqueue(selfqueuelink* entry) {
    entry->addtail(this);
    count++;
    return entry;
  }

  selfqueuelink* remove(selfqueuelink* entry) {
    assert(entry->linked());
    entry->unlink();
    count--;
    return entry;
  }

  selfqueuelink* peek() {
    return (empty()) ? null : head();
  }

  void checkvalid();
};

int ListOfStateLists::add(StateList* list) {
  assert(count < lengthof(data));
  data[count] = list;
  return count++;
}

void ListOfStateLists::reset() {
  foreach (i, count) {
    data[i]->reset();
  }
}

template <typename T> 
void print_list_of_state_lists(ostream& os, const ListOfStateLists& lol, const char* title) {
  os << title, ":", endl;
  foreach (i, lol.count) {
    StateList& list = *lol[i];
    if (!list.count) continue;
    os << list.name, " (", list.count, " entries):", endl;
    int n = 0;
    T* obj;
    foreach_list_mutable(list, obj, entry, nextentry) {
      if ((n % 16) == 0) os << " ";
      os << " ", intstring(obj->index(), -3);
      if (((n % 16) == 15) || (n == list.count-1)) os << endl;
      n++;
    }
    os << endl;
    // list.validate();
  }
}

void StateList::checkvalid() {
#if 0
  int realcount = 0;
  selfqueuelink* obj;
  foreach_list_mutable(*this, obj, entry, nextentry) {
    realcount++;
  }
  assert(count == realcount);
#endif
}

#define DeclareROBList(name, description) StateList name("" description "", rob_states);

//
// Reorder Buffer (ROB) structure, used for tracking all instructions in flight.
// This same structure is used to represent both dispatched but not yet issued 
// instructions (traditionally held in an instruction dispatch buffer, IDB) 
// as well as issued instructions. The descriptions below have much more
// detail on this.
//

struct Cluster {
  char* name;
  W16 issue_width;
  W32 fu_mask;
};

//
// Include the simulator configuration
//
#include <ooohwdef.h>

#if (MAX_CLUSTERS == 1)
#define DeclareClusteredROBList(name, description) StateList name[MAX_CLUSTERS] = { \
  StateList("" description "-cl0", rob_states), \
};
#elif (MAX_CLUSTERS == 2)
#define DeclareClusteredROBList(name, description) StateList name[MAX_CLUSTERS] = { \
  StateList("" description "-cl0", rob_states), \
  StateList("" description "-cl1", rob_states), \
};
#elif (MAX_CLUSTERS == 3)
#define DeclareClusteredROBList(name, description) StateList name[MAX_CLUSTERS] = { \
  StateList("" description "-cl0", rob_states), \
  StateList("" description "-cl1", rob_states), \
  StateList("" description "-cl2", rob_states), \
};
#elif (MAX_CLUSTERS == 4)
#define DeclareClusteredROBList(name, description) StateList name[MAX_CLUSTERS] = { \
  StateList("" description "-cl0", rob_states), \
  StateList("" description "-cl1", rob_states), \
  StateList("" description "-cl2", rob_states), \
  StateList("" description "-cl3", rob_states), \
};
#elif (MAX_CLUSTERS == 5)
#define DeclareClusteredROBList(name, description) StateList name[MAX_CLUSTERS] = { \
  StateList("" description "-cl0", rob_states), \
  StateList("" description "-cl1", rob_states), \
  StateList("" description "-cl2", rob_states), \
  StateList("" description "-cl3", rob_states), \
  StateList("" description "-cl4", rob_states), \
};
#elif (MAX_CLUSTERS == 6)
#define DeclareClusteredROBList(name, description) StateList name[MAX_CLUSTERS] = { \
  StateList("" description "-cl0", rob_states), \
  StateList("" description "-cl1", rob_states), \
  StateList("" description "-cl2", rob_states), \
  StateList("" description "-cl3", rob_states), \
  StateList("" description "-cl4", rob_states), \
  StateList("" description "-cl5", rob_states), \
};
#elif (MAX_CLUSTERS == 7)
#define DeclareClusteredROBList(name, description) StateList name[MAX_CLUSTERS] = { \
  StateList("" description "-cl0", rob_states), \
  StateList("" description "-cl1", rob_states), \
  StateList("" description "-cl2", rob_states), \
  StateList("" description "-cl3", rob_states), \
  StateList("" description "-cl4", rob_states), \
  StateList("" description "-cl5", rob_states), \
  StateList("" description "-cl6", rob_states), \
};
#elif (MAX_CLUSTERS == 8)
#define DeclareClusteredROBList(name, description) StateList name[MAX_CLUSTERS] = { \
  StateList("" description "-cl0", rob_states), \
  StateList("" description "-cl1", rob_states), \
  StateList("" description "-cl2", rob_states), \
  StateList("" description "-cl3", rob_states), \
  StateList("" description "-cl4", rob_states), \
  StateList("" description "-cl5", rob_states), \
  StateList("" description "-cl6", rob_states), \
  StateList("" description "-cl7", rob_states), \
};
#else
#error Maximum number of supported clusters is 8!
#endif

#define for_each_cluster(iter) for (int iter = 0; iter < MAX_CLUSTERS; iter++)
#define for_each_operand(iter) for (int iter = 0; iter < MAX_OPERANDS; iter++)

// Frontend states
DeclareROBList(rob_free_list, "free");                                         // Free entry
DeclareROBList(rob_frontend_list, "frontend");                                 // Frontend in progress (artificial delay)
DeclareROBList(rob_ready_to_dispatch_list, "ready-to-dispatch");               // Ready to dispatch
DeclareClusteredROBList(rob_dispatched_list, "dispatched");                    // Dispatched but waiting for operands
DeclareClusteredROBList(rob_ready_to_issue_list, "ready-to-issue");            // Ready to issue (all operands ready)
DeclareClusteredROBList(rob_ready_to_store_list, "ready-to-store");            // Ready to store (all operands ready except possibly rc)
DeclareClusteredROBList(rob_ready_to_load_list, "ready-to-load");              // Ready to load (all operands ready)

// Out of order core states
DeclareClusteredROBList(rob_issued_list, "issued");                            // Issued and in progress (or for loads, returned here after address is generated)
DeclareClusteredROBList(rob_completed_list, "completed");                      // Completed and result in transit for local and global forwarding
DeclareClusteredROBList(rob_ready_to_writeback_list, "ready-to-write");        // Completed; result ready to writeback in parallel across all cluster register files

DeclareROBList(rob_cache_miss_list, "cache-miss");                             // Loads only: wait for cache miss to be serviced

// In-order commit and retirement state
DeclareROBList(rob_ready_to_commit_queue, "ready-to-commit");                  // Ready to commit

#define issueq_operation_on_cluster(cluster, expr) { int dummyrc; issueq_operation_on_cluster_with_result(cluster, dummyrc, expr); }

static W32 forward_at_cycle_lut[MAX_CLUSTERS][MAX_FORWARDING_LATENCY+1];

struct ReorderBufferEntry;
struct LoadStoreQueueEntry;
struct PhysicalRegister;

struct BranchPredictorUpdateInfo: public PredictorUpdate {
  int stack_recover_idx;
  int bptype;
  W64 ripafter;
};

struct FetchBufferEntry: public TransOp {
  W64 rip;
  W64 uuid;
  const byte* synthop;
  BranchPredictorUpdateInfo predinfo;

  int index() const;

  int init() { return 0; }
  void validate() { }
};

struct ReorderBufferEntry: public selfqueuelink {
  struct StateList* current_state_list;
  FetchBufferEntry uop;
  W16s cycles_left; // execution latency counter, decremented every cycle when executing
  W16s forward_cycle; // forwarding cycle after completion
  W8s cluster;
  byte fu;
  W16s lfrqslot;
  byte entry_valid:1, load_store_second_phase:1;
#ifdef ENABLE_TRANSIENT_VALUE_TRACKING
  byte dest_renamed_before_writeback:1, no_branches_between_renamings:1, transient:1;
  byte consumer_count;
#endif
  W8s  iqslot;

  indexref<PhysicalRegister> physreg;
  indexref<PhysicalRegister> operands[MAX_OPERANDS];

  // Loads and stores only:
  indexref<LoadStoreQueueEntry> lsq;

  int index() const;

  void init() {
    entry_valid = 0;

    foreach_issueq(reset());
    selfqueuelink::reset();
    current_state_list = NULL;

    reset();
  }

  //
  // Clean out various fields from the ROB entry that are 
  // expected to be zero when allocating a new ROB entry.
  //
  void reset() {
    int latency, operand;
    // Deallocate ROB entry
    entry_valid = false;
    cycles_left = 0;
    physreg = (const PhysicalRegister*)null;
    lfrqslot = -1;
    lsq = 0;
    load_store_second_phase = 0;
#ifdef ENABLE_TRANSIENT_VALUE_TRACKING
    dest_renamed_before_writeback = 0;
    no_branches_between_renamings = 0;
    consumer_count = 0;
#endif
  }

  void validate() {
    entry_valid = true;
  }

  void changestate(StateList& newqueue) {
    if (current_state_list)
      current_state_list->remove(this);
    current_state_list = &newqueue;
    newqueue.enqueue(this);
  }

  bool operand_ready(int operand) const;

  bool ready_to_issue() const {
    bool raready = operand_ready(0);
    bool rbready = operand_ready(1);
    bool rcready = operand_ready(2);
    bool rsready = operand_ready(3);

    if (isstore(uop.opcode)) {
      return (load_store_second_phase) ? (raready & rbready & rcready & rsready) : (raready & rbready);
    } else if (isload(uop.opcode)) {
      return (load_store_second_phase) ? (raready & rbready & rcready & rsready) : (raready & rbready & rcready);
    } else {
      return (raready & rbready & rcready & rsready);
    }
  }

  bool ready_to_commit() const {
    return (current_state_list == &rob_ready_to_commit_queue);
  }

  StateList& get_ready_to_issue_list() const {
    return 
      isload(uop.opcode) ? rob_ready_to_load_list[cluster] :
      isstore(uop.opcode) ? rob_ready_to_store_list[cluster] :
      rob_ready_to_issue_list[cluster];
  }

  bool has_exception() const;

  bool find_sources();
  int forward();

  int select_cluster();

  int issue();
  int issuestore(LoadStoreQueueEntry& state, W64 ra, W64 rb, W64 rc, bool rcready);
  int issueload(LoadStoreQueueEntry& state, W64 ra, W64 rb, W64 rc);
  void release();

  W64 annul(bool keep_misspec_uop);

  W64 annul_after() { return annul(true); }
  W64 annul_after_and_including() { return annul(false); }

  int commit();
  void replay();
  int pseudocommit();

  void loadwakeup();

  ostream& print(ostream& os) const;
  stringbuf& get_operand_info(stringbuf& sb, int operand) const;
  ostream& print_operand_info(ostream& os, int operand) const;
};

ostream& operator <<(ostream& os, const ReorderBufferEntry& rob) {
  return rob.print(os);
}

static Queue<ReorderBufferEntry, ROB_SIZE> ROB;

void check_rob() {
  foreach (i, ROB_SIZE) {
    ReorderBufferEntry& rob = ROB[i];
    if (!rob.entry_valid) continue;
    assert(inrange(rob.forward_cycle, 0, (MAX_FORWARDING_LATENCY+1)-1));
  }

  foreach (i, rob_states.count) {
    StateList& list = *rob_states[i];
    ReorderBufferEntry* rob;
    foreach_list_mutable(list, rob, entry, nextentry) {
      assert(inrange(rob->index(), 0, ROB_SIZE-1));
      assert(rob->current_state_list == &list);
      if (!((rob->current_state_list != &rob_free_list) ? rob->entry_valid : (!rob->entry_valid))) {
        logfile << "ROB ", rob->index(), " list = ", rob->current_state_list->name, " entry_valid ", rob->entry_valid, endl, flush;
        dump_ooo_state();
        assert(false);
      }
    }
  }
}

int ReorderBufferEntry::index() const {
  int i = (this - ROB.data);
  //assert(inrange(i, 0, ROB_SIZE-1));
  return i;
}

ReorderBufferEntry& indexref<ReorderBufferEntry>::get(int i) const {
  return ROB[i];
}

void Queue<ReorderBufferEntry, ROB_SIZE>::prepfree(ReorderBufferEntry& rob) {
  rob.reset();
}

#define LSQ_SIZE (LDQ_SIZE + STQ_SIZE)

struct LoadStoreQueueEntry: public SFR {
  ReorderBufferEntry* rob;
  W16s mbtag;
  W16 store:1, entry_valid:1;
  W32 padding;

  LoadStoreQueueEntry() { }

  int index() const;

  void reset() {
    rob = null;
    entry_valid = 0;
    physaddr = 0;
    invalid = 1;
    bytemask = 0;
    addrvalid = 0;
    datavalid = 0;
    mbtag = -1;
    invalid = 0;
    data = 0;
    physaddr = 0;
  }

  void init() {
    reset();
  }

  void validate() {
    entry_valid = 1;
  }
  
  ostream& print(ostream& os) const;

  LoadStoreQueueEntry& operator =(const SFR& sfr) {
    data = sfr.data;
    addrvalid = sfr.addrvalid;
    datavalid = sfr.datavalid;
    invalid = sfr.invalid;
    physaddr = sfr.physaddr;
    bytemask = sfr.bytemask;
    return *this;
  }
};

ostream& operator <<(ostream& os, const LoadStoreQueueEntry& lsq) {
  return lsq.print(os);
}

static Queue<LoadStoreQueueEntry, LSQ_SIZE> LSQ;

LoadStoreQueueEntry& indexref<LoadStoreQueueEntry>::get(int i) const {
  return LSQ[i];
}

int loads_in_flight = 0;
int stores_in_flight = 0;

void Queue<LoadStoreQueueEntry, LSQ_SIZE>::prepfree(LoadStoreQueueEntry& lsq) {
  loads_in_flight -= (lsq.store == 0);
  stores_in_flight -= (lsq.store == 1);
  lsq.reset();
}

int LoadStoreQueueEntry::index() const {
  return (this - LSQ.data);
}

//
// Physical Registers
//

struct PhysicalRegisterFile;

// NOTE: the counter fields of the following total 100%:

// Free to be reallocated
StateList physreg_free_list("free", physreg_states);

// Allocated by rename stage but value not ready
StateList physreg_used_list("used", physreg_states);

// Value ready, but not yet written to physical register file
StateList physreg_ready_list("ready", physreg_states);

// Written and ready in physical register file
StateList physreg_written_list("written", physreg_states);

// Mapped to architectural register
StateList physreg_arch_list("arch", physreg_states);

// Old architectural register mapping overwritten, but consumers are
// still in the pipeline so it cannot be freed yet
StateList physreg_pendingfree_list("pendingfree", physreg_states);

//
// Physical Register Recycling Complications
//
// Consider the following scenario:
//
// - uop U3 is renamed and found to depend on physical register R from an earlier uop U1.
// - U1 commits to architectural register A and moves R to the arch state
// - U2, which updates the same architectural register A as U1, also commits. Since the
//   mapping of A is being logically overwritten by U2, U1's physical register R is freed.
// - U3 finally issues, but finds that operand physical register R for U1 no longer exists.
//
// Additionally, in x86 processors the flags attached to a given physical register may 
// be referenced by three additional rename table entries (for ZAPS, CF, OF) so simply
// freeing the old physical register mapping when the RRT is updated doesn't work.
//
// For these reasons, we need to prevent U2's register from being freed if it is still
// referenced by anything still in the pipeline; the normal reorder buffer mechanism
// cannot always handle this situation in a very long pipeline.
//
// The solution is to give each physical register a reference counter. As each uop operand
// is renamed, the counter for the corresponding physical register is incremented. As each
// uop commits, the counter for each of its operands is decremented, but the counter for
// the target physical register itself is incremented before that register is moved to
// the arch state during commitment (since the committed state now owns that register).
//
// As we update the committed RRT during the commit stage, the old register R mapped
// to the destination architectural register A of the uop being committed is examined.
// The register R is only moved to the free state iff its reference counter is zero.
// Otherwise, it is moved to the pendingfree state. The hardware examines all counters
// every cycle and moves physical registers to the free state only when their counters
// become zero and they are in the pendingfree state.
//
// An additional complication arises for x86 since we maintain three separate rename 
// table entries for the ZAPS, CF, OF flags in addition to the register rename table
// entry. Therefore, each speculative RRT and commit RRT entry adds to the refcount.
//
// Hardware Implementation
//
// The hardware implementation of this scheme is straightforward and low complexity.
// The counters can have a very small number of bits since it is very unlikely a given
// physical register would be referenced by all 100+ uops in the ROB; 3 bits should be
// enough to handle the typical maximum of < 8 uops sharing a given operand. Counter
// overflows can simply stall renaming or flush the pipeline since they are so rare.
//
// The counter table can be updated in bulk each cycle by adding/subtracting the
// appropriate sum or just adding zero if the corresponding register wasn't used.
// Since there are several stages between renaming and commit, the same counter is never
// both incremented and decremented in the same cycle, so race conditions are not an 
// issue. 
//
// In real processors, the Pentium 4 uses a scheme similar to this one but uses bit
// vectors instead. For smaller physical register files, this may be a better solution.
// Each physical register has a bit vector with one bit per ROB entry. If a given
// physical register P is still used by ROB entry E in the pipeline, P's bit vector
// bit R is set. Register P cannot be freed until all bits in its vector are zero.
//

struct PhysicalRegister: public selfqueuelink {
public:
  StateList* current_state_list;
  W64 data;
  W16 flags;
  indexref<ReorderBufferEntry> rob;
  W8 archreg;
  W8 all_consumers_sourced_from_bypass:1;
  W16s refcount;

  void changestate(StateList& newqueue) {
    if (current_state_list)
      current_state_list->remove(this);
    current_state_list = &newqueue;
    newqueue.enqueue(this);
  }

  void addref() { if (nonnull()) refcount++; }
  void unref() { if (nonnull()) refcount--; assert(refcount >= 0); }

  void addref(const ReorderBufferEntry& rob) { addref(); }
  void unref(const ReorderBufferEntry& rob) { unref(); }
  void addspecref(int archreg) { addref(); }
  void unspecref(int archreg) { unref(); }
  void addcommitref(int archreg) { addref(); }
  void uncommitref(int archreg) { unref(); }
  bool referenced() const { return (refcount > 0); }

  bool nonnull() const { return (index() != PHYS_REG_NULL); }
  bool allocated() const { return (current_state_list != &physreg_free_list); }

  void commit() { changestate(physreg_arch_list); }
  void complete() { changestate(physreg_ready_list); }
  void writeback() { changestate(physreg_written_list); }

  void free() {
    changestate(physreg_free_list);
    rob = 0;
    refcount = 0;
    all_consumers_sourced_from_bypass = 1;
  }

  //
  // Notice that this does NOT change the physical register
  // values, since these are needed in fixed positions across
  // exceptions.
  //
  void reset() {
    selfqueuelink::reset();
    current_state_list = null;
    free();
  }

  int index() const;

  bool valid() const {
    return ((flags & FLAG_INV) == 0);
  }

  bool ready() const {
    return ((flags & FLAG_WAIT) == 0);
  }
};

bool ReorderBufferEntry::has_exception() const {
  return ((physreg->flags & FLAG_INV) != 0);
}

ostream& operator <<(ostream& os, const PhysicalRegister& physreg) {
  stringbuf sb;
  print_value_and_flags(sb, physreg.data, physreg.flags);

  os << "  r", intstring(physreg.index(), -3), " state ", padstring(physreg.current_state_list->name, -12), " ", sb;
  if (physreg.rob) os << " rob ", physreg.rob->index(), " (uuid ", physreg.rob->uop.uuid, ")";
  os << " refcount ", physreg.refcount;

  return os;
}

struct PhysicalRegisterFile: public array<PhysicalRegister, PHYS_REG_FILE_SIZE> {
  PhysicalRegisterFile() {
    reset();
  }

  //
  // Reset to initial state, for instance after major exceptions.
  // The physical register values themselves are preserved, since
  // they may have been updated by external_to_core_state().
  //
  void reset() {
    physreg_free_list.reset();
    physreg_used_list.reset();
    physreg_ready_list.reset();
    physreg_written_list.reset();
    physreg_arch_list.reset();
    physreg_pendingfree_list.reset();

    foreach (i, PHYS_REG_FILE_SIZE) {
      PhysicalRegister& physreg = (*this)[i];
      physreg.reset();
    }

    for (int i = PHYS_REG_ARCH_BASE; i < PHYS_REG_FILE_SIZE; i++) {
      PhysicalRegister& physreg = (*this)[i];
      physreg.commit();
    }

    PhysicalRegister& zeroreg = (*this)[PHYS_REG_NULL];
    zeroreg.changestate(physreg_arch_list);
    zeroreg.data = 0;
  }

  bool remaining() const {
    return (!physreg_free_list.empty());
  }

  PhysicalRegister* alloc() {
    PhysicalRegister* physreg = (PhysicalRegister*)physreg_free_list.dequeue();
    if (!physreg)
      return null;
    physreg->current_state_list = null;
    physreg->changestate(physreg_used_list);
    physreg->flags = FLAG_WAIT;
    return physreg;
  }
};

PhysicalRegisterFile physregs;

int PhysicalRegister::index() const {
  return (this - physregs.data);
}

PhysicalRegister& indexref<PhysicalRegister>::get(int i) const {
  return physregs[i];
}

bool ReorderBufferEntry::operand_ready(int operand) const {
  return ((operands[operand]->flags & FLAG_WAIT) == 0);
}

ostream& operator <<(ostream& os, const PhysicalRegisterFile& physregs) {
  foreach (i, PHYS_REG_FILE_SIZE) {
    os << physregs[i], endl;
  }
  return os;
}

struct RegisterRenameTable: public array<indexref<PhysicalRegister>, TRANSREG_COUNT> {
#ifdef ENABLE_TRANSIENT_VALUE_TRACKING
  bitvec<TRANSREG_COUNT> renamed_in_this_basic_block;
#endif

  RegisterRenameTable() {
    reset();
  }

  void reset() {
    RegisterRenameTable& rrt = *this;

    for (int i = 0; i < ARCHREG_COUNT; i++) {
      rrt[i] = PHYS_REG_ARCH_BASE + i;
    }

    //
    // Internal translation registers are never used before
    // they are written for the first time:
    //
    for (int i = ARCHREG_COUNT; i < TRANSREG_COUNT; i++) {
      rrt[i] = PHYS_REG_NULL;
    }

    rrt[REG_zf] = PHYS_REG_ARCH_BASE + REG_flags;
    rrt[REG_cf] = PHYS_REG_ARCH_BASE + REG_flags;
    rrt[REG_of] = PHYS_REG_ARCH_BASE + REG_flags;
    rrt[REG_imm] = PHYS_REG_NULL;
    rrt[REG_mem] = PHYS_REG_NULL;
    rrt[REG_zero] = PHYS_REG_NULL;
#ifdef ENABLE_TRANSIENT_VALUE_TRACKING
    renamed_in_this_basic_block.reset();
#endif
  }

  ostream& print(ostream& os) const {
    foreach (i, TRANSREG_COUNT) {
      if ((i % 8) == 0) os << " ";
      os << " ", padstring(arch_reg_names[i], -6), " r", intstring((*this)[i]->index(), -3), " | ";
      if (((i % 8) == 7) || (i == TRANSREG_COUNT-1)) os << endl;
    }
    return os;
  }
};

ostream& operator <<(ostream& os, const RegisterRenameTable& rrt) {
  return rrt.print(os);
}

RegisterRenameTable specrrt;
RegisterRenameTable commitrrt;

void print_rename_tables(ostream& os) {
   os << "SpecRRT:", endl;
   os << specrrt;
   os << "CommitRRT:", endl;
   os << commitrrt;
}

void log_forwarding(const ReorderBufferEntry* source, const ReorderBufferEntry* target, int operand) {
  if (loglevel <= 0) return;

  PhysicalRegister* physreg = source->physreg;

  stringbuf rdstr; print_value_and_flags(rdstr, physreg->data, physreg->flags);
  logfile << intstring(source->uop.uuid, 20), " forwd", source->forward_cycle, " rob ", intstring(source->index(), -3), 
    " (", clusters[source->cluster].name, ") r", intstring(physreg->index(), -3), 
    " => ", "uuid ", target->uop.uuid, " rob ", target->index(), " (", clusters[target->cluster].name, ") r", target->physreg->index(), " operand ", operand;
  if (isstore(target->uop.opcode)) logfile << " => st", target->lsq->index();
  logfile << " [still waiting?";
  foreach (i, MAX_OPERANDS) { if (!target->operand_ready(i)) logfile << " r", (char)('a' + i); }
  if (target->ready_to_issue()) logfile << " READY";
  logfile << "]";
  logfile << endl;
}

template <int size, int operandcount>
void IssueQueue<size, operandcount>::tally_broadcast_matches(byte sourceid, const bitvec<size>& mask, int operand) const {
  if (loglevel <= 0) return;

  const ReorderBufferEntry* source = &ROB[sourceid];

  bitvec<size> temp = mask;

  while (*temp) {
    int slot = temp.lsb();
    int robid = uopof(slot);
    assert(inrange(robid, 0, ROB_SIZE-1));
    const ReorderBufferEntry* target = &ROB[robid];

    log_forwarding(source, target, operand);
    temp[slot] = 0;
  }
}

//
// Fetch Stage
//

BasicBlock* current_basic_block = null;
const byte* current_basic_block_transop = null;
int current_basic_block_transop_index = 0;
int bytes_in_current_insn = 0;

W64 fetchrip;
int uop_in_basic_block;

//
// Fetch a stream of x86 instructions from the L1 i-cache along predicted
// branch paths.
//
// Internally, up to N uops per clock corresponding to instructions in
// the current basic block are fetched per cycle and placed in the uopq
// as TransOps. When we run out of uops in one basic block, we proceed
// to lookup or translate the next basic block.
//

bool stall_frontend = 0;

static Queue<FetchBufferEntry, FETCH_QUEUE_SIZE> fetchq;

bool waiting_for_icache_fill = false;

// call this in response to a branch mispredict:
void reset_fetch_unit(W64 realrip) {
  fetchrip = realrip;
  stall_frontend = false;
  waiting_for_icache_fill = 0;
  fetchq.reset();
  current_basic_block = null;
  current_basic_block_transop = null;
  current_basic_block_transop_index = 0;
}

//
// Flush everything in pipeline immediately
//
void flush_pipeline(W64 realrip) {
  dcache_complete();
  reset_fetch_unit(realrip);
  rob_states.reset();
  physreg_states.reset();

  ROB.reset();
  foreach (i, ROB_SIZE) {
    ROB[i].changestate(rob_free_list);
  }
  LSQ.reset();
  loads_in_flight = 0;
  stores_in_flight = 0;
  physregs.reset();
  specrrt.reset();
  commitrrt.reset();
  foreach (i, TRANSREG_COUNT) { specrrt[i]->addspecref(i); }
  foreach (i, TRANSREG_COUNT) { commitrrt[i]->addcommitref(i); }
}

void external_to_core_state() {
  foreach (i, ARCHREG_COUNT) {
    commitrrt[i]->data = ctx.commitarf[i];
    commitrrt[i]->flags = 0;
  }
  physregs[PHYS_REG_ARCH_BASE + REG_flags].data = (W16)ctx.commitarf[REG_flags];
  physregs[PHYS_REG_ARCH_BASE + REG_flags].flags = (W16)ctx.commitarf[REG_flags];
}

void core_to_external_state() {
  W64 rip = ctx.commitarf[REG_rip];
  W64 flags = ctx.commitarf[REG_flags];

  foreach (i, ARCHREG_COUNT) {
    ctx.commitarf[i] = physregs[commitrrt[i]->index()].data;
  }

  ctx.commitarf[REG_rip] = rip;
  ctx.commitarf[REG_flags] = flags;

  /*
    // Same thing:

  ctx.commitarf[REG_flags] = 
    (commitrrt[REG_zf]->flags & (FLAG_ZAPS)) | 
    (commitrrt[REG_cf]->flags & (FLAG_CF)) | 
    (commitrrt[REG_of] & (FLAG_OF));
  */
}

int FetchBufferEntry::index() const {
  return (this - fetchq.data);
}

W64 fetch_uuid = 0;

CycleTimer ctfetch;
CycleTimer cttrans;

W64 fetch_width_histogram[FETCH_WIDTH+1];
W64 branchpred_predictions;
W64 branchpred_updates;

W64 branchpred_cond_correct;
W64 branchpred_cond_mispred;
W64 branchpred_indir_correct;
W64 branchpred_indir_mispred;
W64 branchpred_return_correct;
W64 branchpred_return_mispred;
W64 branchpred_total_correct;
W64 branchpred_total_mispred;

// Cause of fetch stops:
// totals 100%
W64 fetch_stop_icache_miss;
W64 fetch_stop_fetchq_full;
W64 fetch_stop_bogus_rip;
W64 fetch_stop_branch_taken;
W64 fetch_stop_full_width;

// (n/a):
W64 fetch_blocks_fetched;
W64 fetch_uops_fetched;
W64 fetch_user_insns_fetched;

W64 bbcache_inserts;
W64 bbcache_removes;

W64 fetch_opclass_histogram[OPCLASS_COUNT];

W64 icache_filled_callback(LoadStoreInfo lsi, W64 addr) {
  if (logable(1)) 
    logfile << "L1 i-cache wakeup on line ", (void*)addr, endl;

  waiting_for_icache_fill = 0;
  return 0;
}

// How many bytes of x86 code to fetch into decode buffer at once
#define ICACHE_FETCH_GRANULARITY 16

// Last block in icache we fetched into our buffer
W64 current_icache_block = 0;

void fetch() {
  int fetchcount = 0;
  int taken_branch_count = 0;

  start_timer(ctfetch);

  if (stall_frontend) {
    if (logable(1)) logfile << padstring("", 20), " fetch  frontend stalled", endl;
    return;
  }

  if (waiting_for_icache_fill) {
    if (logable(1)) logfile << padstring("", 20), " fetch  rip 0x", (void*)fetchrip, ": wait for icache fill", endl;
    fetch_stop_icache_miss++;
    return;
  }

  while ((fetchcount < FETCH_WIDTH) && (taken_branch_count == 0)) {
    if (!fetchq.remaining()) {
      if (!fetchcount)
        if (logable(1)) logfile << padstring("", 20), " fetch  rip 0x", (void*)fetchrip, ": fetchq full", endl;
      fetch_stop_fetchq_full++;
      break;
    }

    if (!asp.check((byte*)fetchrip, PROT_EXEC)) {
      if (logable(1)) logfile << padstring("", 20), " fetch  rip 0x", (void*)fetchrip, ": bogus RIP", endl;
      fetch_stop_bogus_rip++;
      break;
    }

    W64 req_icache_block = floor(fetchrip, ICACHE_FETCH_GRANULARITY);
    if (req_icache_block != current_icache_block) {
      bool hit = probe_icache(fetchrip);
      if (!hit) {
        int missbuf = initiate_icache_miss(fetchrip);
        if (logable(1)) logfile << padstring("", 20), " fetch  rip 0x", (void*)fetchrip, ": wait for icache fill on missbuf ", missbuf, endl;
        if (missbuf < 0) {
          // Try to re-allocate a miss buffer on the next cycle
          if (logable(1)) logfile << padstring("", 20), " fetch  rip 0x", (void*)fetchrip, ": icache fill missbuf full", endl;
          break;
        }
        waiting_for_icache_fill = 1;
        fetch_stop_icache_miss++;
        break;
      }

      fetch_blocks_fetched++;
      current_icache_block = req_icache_block;
      fetch_hit_L1++;
    }

    if ((!current_basic_block) || (current_basic_block_transop_index >= current_basic_block->count)) {
      BasicBlock** bb = bbcache(fetchrip);

      if (bb) {
        current_basic_block = *bb;
      } else {
        start_timer(cttrans);
        current_basic_block = translate_basic_block((byte*)fetchrip);
        assert(current_basic_block);
        synth_uops_for_bb(*current_basic_block);
        stop_timer(cttrans);

        if (logable(1)) logfile << padstring("", 20), " xlate  rip 0x", (void*)fetchrip, ": BB ", current_basic_block, " of ", current_basic_block->count, " uops", endl;
        bbcache.add(fetchrip, current_basic_block);
        bbcache_inserts++;
      }

      current_basic_block_transop = current_basic_block->data;
      current_basic_block_transop_index = 0;
    }

    FetchBufferEntry& transop = *fetchq.alloc();
    current_basic_block_transop = transop.expand(current_basic_block_transop);
    transop.synthop = current_basic_block->synthops[current_basic_block_transop_index];
    current_basic_block_transop_index++;

    if (transop.som) {
      bytes_in_current_insn = transop.bytes;
      fetch_user_insns_fetched++;
    }

    fetch_uops_fetched++;

    W64 predrip = 0;

    if (isclass(transop.opcode, OPCLASS_BRANCH)) {
      transop.predinfo.bptype = 
        (isclass(transop.opcode, OPCLASS_COND_BRANCH) << log2(BRANCH_HINT_COND)) |
        (isclass(transop.opcode, OPCLASS_INDIR_BRANCH) << log2(BRANCH_HINT_INDIRECT)) |
        (bit(transop.extshift, log2(BRANCH_HINT_PUSH_RAS)) << log2(BRANCH_HINT_CALL)) |
        (bit(transop.extshift, log2(BRANCH_HINT_POP_RAS)) << log2(BRANCH_HINT_RET));

      transop.predinfo.ripafter = fetchrip + bytes_in_current_insn;
      predrip = branchpred.predict(transop.predinfo, transop.predinfo.bptype, transop.predinfo.ripafter, transop.riptaken);
      branchpred_predictions++;
    }

    transop.rip = fetchrip;
    transop.uuid = fetch_uuid++;

    // Set up branches so mispredicts can be calculated correctly:
    if (isclass(transop.opcode, OPCLASS_COND_BRANCH)) {
      if (predrip != transop.riptaken) {
        assert(predrip == transop.ripseq);
        transop.cond = invert_cond(transop.cond);

        //
        // We need to be careful here: we already looked up the synthop for this
        // uop according to the old condition, so redo that here so we call the
        // correct code for the swapped condition.
        //
        transop.synthop = get_synthcode_for_cond_branch(transop.opcode, transop.cond, transop.size, 0);

        W64 temp = transop.riptaken;
        transop.riptaken = transop.ripseq;
        transop.ripseq = temp;
      }
    } else if (isclass(transop.opcode, OPCLASS_INDIR_BRANCH)) {
      transop.riptaken = predrip;
      transop.ripseq = predrip;
    } else if (isclass(transop.opcode, OPCLASS_UNCOND_BRANCH)) { // unconditional branches need no special handling
      assert(predrip == transop.riptaken);
    }

    fetch_opclass_histogram[opclassof(transop.opcode)]++;

    if (logable(1)) {
      logfile << intstring(transop.uuid, 20), " fetch  rip ", (void*)transop.rip, ": ", transop, 
        " (BB ", current_basic_block, " uop ", current_basic_block_transop_index, " of ", current_basic_block->count;
      if (transop.som) logfile << "; SOM";
      if (transop.eom) logfile << "; EOM ", bytes_in_current_insn, " bytes";
      logfile << ")";
      if (transop.eom && predrip) logfile << " -> pred ", (void*)predrip;
      logfile << endl;
    }

    if (transop.eom) {
      fetchrip += bytes_in_current_insn;

      if (predrip) {
        // follow to target, then end fetching for this cycle if predicted taken
        bool taken = (predrip != fetchrip);
        taken_branch_count += taken;
        fetchrip = predrip;
        if (taken) {
          fetch_stop_branch_taken++;
          break;
        }
      }
    }

    fetchcount++;
  }

  fetch_stop_full_width += (fetchcount == FETCH_WIDTH);
  fetch_width_histogram[fetchcount]++;
  stop_timer(ctfetch);
}

static const byte archdest_is_visible[TRANSREG_COUNT] = {
  // Integer registers
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  // SSE registers, low 64 bits
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  // SSE registers, high 64 bits
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  // x87 FP / MMX / special
  1, 1, 1, 1, 0, 0, 0, 0,
  0, 0, 0, 1, 0, 0, 0, 0,
  // The following are ONLY used during the translation and renaming process:
  0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,
};

static const byte archdest_can_rename[TRANSREG_COUNT] = {
  // Integer registers
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  // SSE registers, low 64 bits
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  // SSE registers, high 64 bits
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  // x87 FP / MMX / special
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 0,
  // The following are ONLY used during the translation and renaming process:
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 0, 1, 0, 0, 0,
};

#define archdest_can_commit archdest_can_rename

CycleTimer ctrename;

// totals 100%:
W64 frontend_status_complete;
W64 frontend_status_fetchq_empty;
W64 frontend_status_rob_full;
W64 frontend_status_physregs_full;
W64 frontend_status_ldq_full;
W64 frontend_status_stq_full;

// totals 100%
W64 frontend_width_histogram[FRONTEND_WIDTH+1];

// totals 100%:
W64 frontend_renamed_none;
W64 frontend_renamed_reg;
W64 frontend_renamed_flags;
W64 frontend_renamed_reg_and_flags;

// totals 100%:
W64 frontend_alloc_reg;
W64 frontend_alloc_ldreg;
W64 frontend_alloc_sfr;
W64 frontend_alloc_br;

void rename() {
  int prepcount = 0;

  start_timer(ctrename);

  while (prepcount < FRONTEND_WIDTH) {
    if (fetchq.empty()) {
      if (!prepcount) if (logable(1)) logfile << padstring("", 20), " rename fetchq empty", endl;
      frontend_status_fetchq_empty++;
      break;
    } 

    if (!ROB.remaining()) {
      if (!prepcount) if (logable(1)) logfile << padstring("", 20), " rename ROB full", endl;
      frontend_status_rob_full++;
      break;
    }

    FetchBufferEntry& fetchbuf = *fetchq.peek();

    if (!physregs.remaining()) {
      if (!prepcount) if (logable(1)) logfile << padstring("", 20), " rename physregs full", endl;
      frontend_status_physregs_full++;
      break;
    }

    bool ld = isload(fetchbuf.opcode);
    bool st = isstore(fetchbuf.opcode);
    bool br = isbranch(fetchbuf.opcode);

    if (ld && (loads_in_flight >= LDQ_SIZE)) {
      if (!prepcount) if (logable(1)) logfile << padstring("", 20), " rename ldq full", endl;
      frontend_status_ldq_full++;
      break;
    }

    if (st && (stores_in_flight >= LDQ_SIZE)) {
      if (!prepcount) if (logable(1)) logfile << padstring("", 20), " rename stq full", endl;
      frontend_status_stq_full++;
      break;
    }

    if ((ld|st) && (!LSQ.remaining())) {
      if (!prepcount) if (logable(1)) logfile << padstring("", 20), " rename memq full", endl;
      break;
    }

    frontend_status_complete++;

    FetchBufferEntry& transop = *fetchq.dequeue();
    ReorderBufferEntry& rob = *ROB.alloc();
    PhysicalRegister& physreg = *physregs.alloc();
    // For debugging purposes:
    physreg.flags = FLAG_WAIT;
    physreg.data = 0xdeadbeefdeadbeef;

    LoadStoreQueueEntry* lsqp = (ld|st) ? LSQ.alloc() : null;
    LoadStoreQueueEntry& lsq = *lsqp;

    rob.reset();
    rob.physreg = physreg;
    rob.uop = transop;
    rob.entry_valid = 1;
    rob.cycles_left = FRONTEND_STAGES;
    if (ld|st) {
      rob.lsq = lsq.index();
      lsq.rob = &rob;
      lsq.store = st;
      lsq.datavalid = 0;
      lsq.addrvalid = 0;
      lsq.invalid = 0;
    }

    frontend_alloc_reg += (!(ld|st|br));
    frontend_alloc_ldreg += ld;
    frontend_alloc_sfr += st;
    frontend_alloc_br += br;

    physreg.rob = rob;
    physreg.archreg = rob.uop.rd;

    //
    // Rename operands:
    //

    rob.operands[RA] = specrrt[transop.ra];
    rob.operands[RB] = specrrt[transop.rb];
    rob.operands[RC] = specrrt[transop.rc];
    rob.operands[RS] = PHYS_REG_NULL; // used for loads and stores only

    // See notes above on Physical Register Recycling Complications
    foreach (i, MAX_OPERANDS) {
      rob.operands[i]->addref(rob);
      assert(rob.operands[i]->current_state_list != &rob_free_list);

#ifdef ENABLE_TRANSIENT_VALUE_TRACKING
      if ((rob.operands[i]->current_state_list == &physreg_used_list) |
          (rob.operands[i]->current_state_list == &physreg_ready_list) |
          (rob.operands[i]->current_state_list == &physreg_written_list)) {
        rob.operands[i]->rob->consumer_count = min(rob.operands[i]->rob->consumer_count + 1, 255);
      }
#endif
    }

    bool renamed_reg = 0;
    bool renamed_flags = 0;

    if (logable(1)) {
      logfile << intstring(transop.uuid, 20), " rename rob ", intstring(rob.index(), -3), " r", rob.physreg->index();
      if (ld|st) logfile << ", lsq", lsq.index();

      logfile << " = ";
      foreach (i, MAX_OPERANDS) {
        int srcreg = (i == 0) ? transop.ra : (i == 1) ? transop.rb : (i == 2) ? transop.rc : REG_zero;
        logfile << arch_reg_names[srcreg], ((i < MAX_OPERANDS-1) ? "," : "");
      }
      logfile << " -> ";
      foreach (i, MAX_OPERANDS) {
        logfile << "r", rob.operands[i]->index(), ((i < MAX_OPERANDS-1) ? "," : "");
      }
      logfile << "; renamed";
    }

    if (archdest_can_rename[transop.rd]) {
      if (logable(1)) logfile << " ", arch_reg_names[transop.rd], " (old r", specrrt[transop.rd]->index(), ")";

#ifdef ENABLE_TRANSIENT_VALUE_TRACKING
      PhysicalRegister* oldmapping = specrrt[transop.rd];
      if ((oldmapping->current_state_list == &physreg_used_list) |
          (oldmapping->current_state_list == &physreg_ready_list)) {
        oldmapping->rob->dest_renamed_before_writeback = 1;
      }

      if ((oldmapping->current_state_list == &physreg_used_list) |
          (oldmapping->current_state_list == &physreg_ready_list) | 
          (oldmapping->current_state_list == &physreg_written_list)) {
        oldmapping->rob->no_branches_between_renamings = specrrt.renamed_in_this_basic_block[transop.rd];
      }

      specrrt.renamed_in_this_basic_block[transop.rd] = 1;
#endif

      specrrt[transop.rd]->unspecref(transop.rd);
      specrrt[transop.rd] = rob.physreg;
      rob.physreg->addspecref(transop.rd);
      renamed_reg = archdest_is_visible[transop.rd];
    }

    if (!transop.nouserflags) {
      if (transop.setflags & SETFLAG_ZF) {
        if (logable(1)) logfile << " zf (old r", specrrt[REG_zf]->index(), ")";
        specrrt[REG_zf]->unspecref(REG_zf);
        specrrt[REG_zf] = rob.physreg;
        rob.physreg->addspecref(REG_zf);
      }
      if (transop.setflags & SETFLAG_CF) {
        if (logable(1)) logfile << " cf (old r", specrrt[REG_cf]->index(), ")";
        specrrt[REG_cf]->unspecref(REG_cf);
        specrrt[REG_cf] = rob.physreg;
        rob.physreg->addspecref(REG_cf);
      }
      if (transop.setflags & SETFLAG_OF) {
        if (logable(1)) logfile << " of (old r", specrrt[REG_of]->index(), ")";
        specrrt[REG_of]->unspecref(REG_of);
        specrrt[REG_of] = rob.physreg;
        rob.physreg->addspecref(REG_of);
      }
      renamed_flags = (transop.setflags != 0);
    }

    if (logable(1)) {
      logfile << endl;
    }

    if (isbranch(rob.uop.opcode) && (rob.uop.predinfo.bptype & (BRANCH_HINT_CALL|BRANCH_HINT_RET)))
      branchpred.updateras(rob.uop.predinfo, rob.uop.predinfo.ripafter);

    foreach (i, MAX_OPERANDS) {
      assert(rob.operands[i]->allocated());
    }

#ifdef ENABLE_TRANSIENT_VALUE_TRACKING
    if (br) specrrt.renamed_in_this_basic_block.reset();
#endif

    frontend_renamed_none += ((!renamed_reg) && (!renamed_flags));
    frontend_renamed_reg += ((renamed_reg) && (!renamed_flags));
    frontend_renamed_flags += ((!renamed_reg) && (renamed_flags));
    frontend_renamed_reg_and_flags += ((renamed_reg) && (renamed_flags));

    rob.changestate(rob_frontend_list);

    prepcount++;
  }

  frontend_width_histogram[prepcount]++;

  stop_timer(ctrename);
}

CycleTimer ctfrontend;

int frontend() {
  ReorderBufferEntry* rob;

  start_timer(ctfrontend);

  foreach_list_mutable(rob_frontend_list, rob, entry, nextentry) {
    if (rob->cycles_left <= 0) {
      rob->cycles_left = -1;
      rob->changestate(rob_ready_to_dispatch_list);
    } else {
      if (logable(1)) logfile << intstring(rob->uop.uuid, 20), " front  rob ", intstring(rob->index(), -3), " frontend stage ", (FRONTEND_STAGES - rob->cycles_left), " of ", FRONTEND_STAGES, endl;
    }

    rob->cycles_left--;
  }

  stop_timer(ctfrontend);

  return 0;
}

//
// Dispatch and Cluster Selection
//

static byte bit_indices_set_8bits[1<<8][8] = {
  {0, 0, 0, 0, 0, 0, 0, 0},  {0, 0, 0, 0, 0, 0, 0, 0},  
  {1, 1, 1, 1, 1, 1, 1, 1},  {0, 1, 0, 1, 0, 1, 0, 1},
  {2, 2, 2, 2, 2, 2, 2, 2},  {0, 2, 0, 2, 0, 2, 0, 2},
  {1, 2, 1, 2, 1, 2, 1, 2},  {0, 1, 2, 0, 1, 2, 0, 1},
  {3, 3, 3, 3, 3, 3, 3, 3},  {0, 3, 0, 3, 0, 3, 0, 3},
  {1, 3, 1, 3, 1, 3, 1, 3},  {0, 1, 3, 0, 1, 3, 0, 1},
  {2, 3, 2, 3, 2, 3, 2, 3},  {0, 2, 3, 0, 2, 3, 0, 2},
  {1, 2, 3, 1, 2, 3, 1, 2},  {0, 1, 2, 3, 0, 1, 2, 3},
  {4, 4, 4, 4, 4, 4, 4, 4},  {0, 4, 0, 4, 0, 4, 0, 4},
  {1, 4, 1, 4, 1, 4, 1, 4},  {0, 1, 4, 0, 1, 4, 0, 1},
  {2, 4, 2, 4, 2, 4, 2, 4},  {0, 2, 4, 0, 2, 4, 0, 2},
  {1, 2, 4, 1, 2, 4, 1, 2},  {0, 1, 2, 4, 0, 1, 2, 4},
  {3, 4, 3, 4, 3, 4, 3, 4},  {0, 3, 4, 0, 3, 4, 0, 3},
  {1, 3, 4, 1, 3, 4, 1, 3},  {0, 1, 3, 4, 0, 1, 3, 4},
  {2, 3, 4, 2, 3, 4, 2, 3},  {0, 2, 3, 4, 0, 2, 3, 4},
  {1, 2, 3, 4, 1, 2, 3, 4},  {0, 1, 2, 3, 4, 0, 1, 2},
  {5, 5, 5, 5, 5, 5, 5, 5},  {0, 5, 0, 5, 0, 5, 0, 5},
  {1, 5, 1, 5, 1, 5, 1, 5},  {0, 1, 5, 0, 1, 5, 0, 1},
  {2, 5, 2, 5, 2, 5, 2, 5},  {0, 2, 5, 0, 2, 5, 0, 2},
  {1, 2, 5, 1, 2, 5, 1, 2},  {0, 1, 2, 5, 0, 1, 2, 5},
  {3, 5, 3, 5, 3, 5, 3, 5},  {0, 3, 5, 0, 3, 5, 0, 3},
  {1, 3, 5, 1, 3, 5, 1, 3},  {0, 1, 3, 5, 0, 1, 3, 5},
  {2, 3, 5, 2, 3, 5, 2, 3},  {0, 2, 3, 5, 0, 2, 3, 5},
  {1, 2, 3, 5, 1, 2, 3, 5},  {0, 1, 2, 3, 5, 0, 1, 2},
  {4, 5, 4, 5, 4, 5, 4, 5},  {0, 4, 5, 0, 4, 5, 0, 4},
  {1, 4, 5, 1, 4, 5, 1, 4},  {0, 1, 4, 5, 0, 1, 4, 5},
  {2, 4, 5, 2, 4, 5, 2, 4},  {0, 2, 4, 5, 0, 2, 4, 5},
  {1, 2, 4, 5, 1, 2, 4, 5},  {0, 1, 2, 4, 5, 0, 1, 2},
  {3, 4, 5, 3, 4, 5, 3, 4},  {0, 3, 4, 5, 0, 3, 4, 5},
  {1, 3, 4, 5, 1, 3, 4, 5},  {0, 1, 3, 4, 5, 0, 1, 3},
  {2, 3, 4, 5, 2, 3, 4, 5},  {0, 2, 3, 4, 5, 0, 2, 3},
  {1, 2, 3, 4, 5, 1, 2, 3},  {0, 1, 2, 3, 4, 5, 0, 1},
  {6, 6, 6, 6, 6, 6, 6, 6},  {0, 6, 0, 6, 0, 6, 0, 6},
  {1, 6, 1, 6, 1, 6, 1, 6},  {0, 1, 6, 0, 1, 6, 0, 1},
  {2, 6, 2, 6, 2, 6, 2, 6},  {0, 2, 6, 0, 2, 6, 0, 2},
  {1, 2, 6, 1, 2, 6, 1, 2},  {0, 1, 2, 6, 0, 1, 2, 6},
  {3, 6, 3, 6, 3, 6, 3, 6},  {0, 3, 6, 0, 3, 6, 0, 3},
  {1, 3, 6, 1, 3, 6, 1, 3},  {0, 1, 3, 6, 0, 1, 3, 6},
  {2, 3, 6, 2, 3, 6, 2, 3},  {0, 2, 3, 6, 0, 2, 3, 6},
  {1, 2, 3, 6, 1, 2, 3, 6},  {0, 1, 2, 3, 6, 0, 1, 2},
  {4, 6, 4, 6, 4, 6, 4, 6},  {0, 4, 6, 0, 4, 6, 0, 4},
  {1, 4, 6, 1, 4, 6, 1, 4},  {0, 1, 4, 6, 0, 1, 4, 6},
  {2, 4, 6, 2, 4, 6, 2, 4},  {0, 2, 4, 6, 0, 2, 4, 6},
  {1, 2, 4, 6, 1, 2, 4, 6},  {0, 1, 2, 4, 6, 0, 1, 2},
  {3, 4, 6, 3, 4, 6, 3, 4},  {0, 3, 4, 6, 0, 3, 4, 6},
  {1, 3, 4, 6, 1, 3, 4, 6},  {0, 1, 3, 4, 6, 0, 1, 3},
  {2, 3, 4, 6, 2, 3, 4, 6},  {0, 2, 3, 4, 6, 0, 2, 3},
  {1, 2, 3, 4, 6, 1, 2, 3},  {0, 1, 2, 3, 4, 6, 0, 1},
  {5, 6, 5, 6, 5, 6, 5, 6},  {0, 5, 6, 0, 5, 6, 0, 5},
  {1, 5, 6, 1, 5, 6, 1, 5},  {0, 1, 5, 6, 0, 1, 5, 6},
  {2, 5, 6, 2, 5, 6, 2, 5},  {0, 2, 5, 6, 0, 2, 5, 6},
  {1, 2, 5, 6, 1, 2, 5, 6},  {0, 1, 2, 5, 6, 0, 1, 2},
  {3, 5, 6, 3, 5, 6, 3, 5},  {0, 3, 5, 6, 0, 3, 5, 6},
  {1, 3, 5, 6, 1, 3, 5, 6},  {0, 1, 3, 5, 6, 0, 1, 3},
  {2, 3, 5, 6, 2, 3, 5, 6},  {0, 2, 3, 5, 6, 0, 2, 3},
  {1, 2, 3, 5, 6, 1, 2, 3},  {0, 1, 2, 3, 5, 6, 0, 1},
  {4, 5, 6, 4, 5, 6, 4, 5},  {0, 4, 5, 6, 0, 4, 5, 6},
  {1, 4, 5, 6, 1, 4, 5, 6},  {0, 1, 4, 5, 6, 0, 1, 4},
  {2, 4, 5, 6, 2, 4, 5, 6},  {0, 2, 4, 5, 6, 0, 2, 4},
  {1, 2, 4, 5, 6, 1, 2, 4},  {0, 1, 2, 4, 5, 6, 0, 1},
  {3, 4, 5, 6, 3, 4, 5, 6},  {0, 3, 4, 5, 6, 0, 3, 4},
  {1, 3, 4, 5, 6, 1, 3, 4},  {0, 1, 3, 4, 5, 6, 0, 1},
  {2, 3, 4, 5, 6, 2, 3, 4},  {0, 2, 3, 4, 5, 6, 0, 2},
  {1, 2, 3, 4, 5, 6, 1, 2},  {0, 1, 2, 3, 4, 5, 6, 0},
  {7, 7, 7, 7, 7, 7, 7, 7},  {0, 7, 0, 7, 0, 7, 0, 7},
  {1, 7, 1, 7, 1, 7, 1, 7},  {0, 1, 7, 0, 1, 7, 0, 1},
  {2, 7, 2, 7, 2, 7, 2, 7},  {0, 2, 7, 0, 2, 7, 0, 2},
  {1, 2, 7, 1, 2, 7, 1, 2},  {0, 1, 2, 7, 0, 1, 2, 7},
  {3, 7, 3, 7, 3, 7, 3, 7},  {0, 3, 7, 0, 3, 7, 0, 3},
  {1, 3, 7, 1, 3, 7, 1, 3},  {0, 1, 3, 7, 0, 1, 3, 7},
  {2, 3, 7, 2, 3, 7, 2, 3},  {0, 2, 3, 7, 0, 2, 3, 7},
  {1, 2, 3, 7, 1, 2, 3, 7},  {0, 1, 2, 3, 7, 0, 1, 2},
  {4, 7, 4, 7, 4, 7, 4, 7},  {0, 4, 7, 0, 4, 7, 0, 4},
  {1, 4, 7, 1, 4, 7, 1, 4},  {0, 1, 4, 7, 0, 1, 4, 7},
  {2, 4, 7, 2, 4, 7, 2, 4},  {0, 2, 4, 7, 0, 2, 4, 7},
  {1, 2, 4, 7, 1, 2, 4, 7},  {0, 1, 2, 4, 7, 0, 1, 2},
  {3, 4, 7, 3, 4, 7, 3, 4},  {0, 3, 4, 7, 0, 3, 4, 7},
  {1, 3, 4, 7, 1, 3, 4, 7},  {0, 1, 3, 4, 7, 0, 1, 3},
  {2, 3, 4, 7, 2, 3, 4, 7},  {0, 2, 3, 4, 7, 0, 2, 3},
  {1, 2, 3, 4, 7, 1, 2, 3},  {0, 1, 2, 3, 4, 7, 0, 1},
  {5, 7, 5, 7, 5, 7, 5, 7},  {0, 5, 7, 0, 5, 7, 0, 5},
  {1, 5, 7, 1, 5, 7, 1, 5},  {0, 1, 5, 7, 0, 1, 5, 7},
  {2, 5, 7, 2, 5, 7, 2, 5},  {0, 2, 5, 7, 0, 2, 5, 7},
  {1, 2, 5, 7, 1, 2, 5, 7},  {0, 1, 2, 5, 7, 0, 1, 2},
  {3, 5, 7, 3, 5, 7, 3, 5},  {0, 3, 5, 7, 0, 3, 5, 7},
  {1, 3, 5, 7, 1, 3, 5, 7},  {0, 1, 3, 5, 7, 0, 1, 3},
  {2, 3, 5, 7, 2, 3, 5, 7},  {0, 2, 3, 5, 7, 0, 2, 3},
  {1, 2, 3, 5, 7, 1, 2, 3},  {0, 1, 2, 3, 5, 7, 0, 1},
  {4, 5, 7, 4, 5, 7, 4, 5},  {0, 4, 5, 7, 0, 4, 5, 7},
  {1, 4, 5, 7, 1, 4, 5, 7},  {0, 1, 4, 5, 7, 0, 1, 4},
  {2, 4, 5, 7, 2, 4, 5, 7},  {0, 2, 4, 5, 7, 0, 2, 4},
  {1, 2, 4, 5, 7, 1, 2, 4},  {0, 1, 2, 4, 5, 7, 0, 1},
  {3, 4, 5, 7, 3, 4, 5, 7},  {0, 3, 4, 5, 7, 0, 3, 4},
  {1, 3, 4, 5, 7, 1, 3, 4},  {0, 1, 3, 4, 5, 7, 0, 1},
  {2, 3, 4, 5, 7, 2, 3, 4},  {0, 2, 3, 4, 5, 7, 0, 2},
  {1, 2, 3, 4, 5, 7, 1, 2},  {0, 1, 2, 3, 4, 5, 7, 0},
  {6, 7, 6, 7, 6, 7, 6, 7},  {0, 6, 7, 0, 6, 7, 0, 6},
  {1, 6, 7, 1, 6, 7, 1, 6},  {0, 1, 6, 7, 0, 1, 6, 7},
  {2, 6, 7, 2, 6, 7, 2, 6},  {0, 2, 6, 7, 0, 2, 6, 7},
  {1, 2, 6, 7, 1, 2, 6, 7},  {0, 1, 2, 6, 7, 0, 1, 2},
  {3, 6, 7, 3, 6, 7, 3, 6},  {0, 3, 6, 7, 0, 3, 6, 7},
  {1, 3, 6, 7, 1, 3, 6, 7},  {0, 1, 3, 6, 7, 0, 1, 3},
  {2, 3, 6, 7, 2, 3, 6, 7},  {0, 2, 3, 6, 7, 0, 2, 3},
  {1, 2, 3, 6, 7, 1, 2, 3},  {0, 1, 2, 3, 6, 7, 0, 1},
  {4, 6, 7, 4, 6, 7, 4, 6},  {0, 4, 6, 7, 0, 4, 6, 7},
  {1, 4, 6, 7, 1, 4, 6, 7},  {0, 1, 4, 6, 7, 0, 1, 4},
  {2, 4, 6, 7, 2, 4, 6, 7},  {0, 2, 4, 6, 7, 0, 2, 4},
  {1, 2, 4, 6, 7, 1, 2, 4},  {0, 1, 2, 4, 6, 7, 0, 1},
  {3, 4, 6, 7, 3, 4, 6, 7},  {0, 3, 4, 6, 7, 0, 3, 4},
  {1, 3, 4, 6, 7, 1, 3, 4},  {0, 1, 3, 4, 6, 7, 0, 1},
  {2, 3, 4, 6, 7, 2, 3, 4},  {0, 2, 3, 4, 6, 7, 0, 2},
  {1, 2, 3, 4, 6, 7, 1, 2},  {0, 1, 2, 3, 4, 6, 7, 0},
  {5, 6, 7, 5, 6, 7, 5, 6},  {0, 5, 6, 7, 0, 5, 6, 7},
  {1, 5, 6, 7, 1, 5, 6, 7},  {0, 1, 5, 6, 7, 0, 1, 5},
  {2, 5, 6, 7, 2, 5, 6, 7},  {0, 2, 5, 6, 7, 0, 2, 5},
  {1, 2, 5, 6, 7, 1, 2, 5},  {0, 1, 2, 5, 6, 7, 0, 1},
  {3, 5, 6, 7, 3, 5, 6, 7},  {0, 3, 5, 6, 7, 0, 3, 5},
  {1, 3, 5, 6, 7, 1, 3, 5},  {0, 1, 3, 5, 6, 7, 0, 1},
  {2, 3, 5, 6, 7, 2, 3, 5},  {0, 2, 3, 5, 6, 7, 0, 2},
  {1, 2, 3, 5, 6, 7, 1, 2},  {0, 1, 2, 3, 5, 6, 7, 0},
  {4, 5, 6, 7, 4, 5, 6, 7},  {0, 4, 5, 6, 7, 0, 4, 5},
  {1, 4, 5, 6, 7, 1, 4, 5},  {0, 1, 4, 5, 6, 7, 0, 1},
  {2, 4, 5, 6, 7, 2, 4, 5},  {0, 2, 4, 5, 6, 7, 0, 2},
  {1, 2, 4, 5, 6, 7, 1, 2},  {0, 1, 2, 4, 5, 6, 7, 0},
  {3, 4, 5, 6, 7, 3, 4, 5},  {0, 3, 4, 5, 6, 7, 0, 3},
  {1, 3, 4, 5, 6, 7, 1, 3},  {0, 1, 3, 4, 5, 6, 7, 0},
  {2, 3, 4, 5, 6, 7, 2, 3},  {0, 2, 3, 4, 5, 6, 7, 0},
  {1, 2, 3, 4, 5, 6, 7, 1},  {0, 1, 2, 3, 4, 5, 6, 7},
};

int find_random_set_bit(W32 v, int randsource) {
  return bit_indices_set_8bits[v & 0xff][randsource & 0x7];
}

byte uop_executable_on_cluster[OP_MAX_OPCODE];

void init_luts() {
  // Initialize opcode maps
  foreach (i, OP_MAX_OPCODE) {
    W32 allowedfu = opinfo[i].fu;
    W32 allowedcl = 0;
    foreach (cl, MAX_CLUSTERS) {
      if (clusters[cl].fu_mask & allowedfu) setbit(allowedcl, cl);
    }
    uop_executable_on_cluster[i] = allowedcl;
  }

  // Initialize forward-at-cycle LUTs
  foreach (srcc, MAX_CLUSTERS) {
    foreach (destc, MAX_CLUSTERS) {
      foreach (lat, MAX_FORWARDING_LATENCY+1) {
        if (lat == intercluster_latency_map[srcc][destc]) {
          setbit(forward_at_cycle_lut[srcc][lat], destc);
        }
      }
    }
  }
}

W64 dispatch_cluster_histogram[MAX_CLUSTERS];
W64 dispatch_cluster_none_avail;

int ReorderBufferEntry::select_cluster() {
  if (MAX_CLUSTERS == 1)
    return 0;

  W32 executable_on_cluster = uop_executable_on_cluster[uop.opcode];

  int cluster_operand_tally[MAX_CLUSTERS];
  foreach (i, MAX_CLUSTERS) { cluster_operand_tally[i] = 0; }
  foreach (i, MAX_OPERANDS) {
    PhysicalRegister& r = *operands[i];
    if ((&r) && ((r.current_state_list == &physreg_used_list) || (r.current_state_list == &physreg_ready_list))) cluster_operand_tally[r.rob->cluster]++;
  }

  assert(executable_on_cluster);

  // If a given cluster's issue queue is full, try another cluster:
  int cluster_issue_queue_avail_count[MAX_CLUSTERS];
  W32 cluster_issue_queue_avail_mask = 0;

  sched_get_all_issueq_free_slots(cluster_issue_queue_avail_count);

  foreach (i, MAX_CLUSTERS) {
    cluster_issue_queue_avail_mask |= ((cluster_issue_queue_avail_count[i] > 0) << i);
  }

  executable_on_cluster &= cluster_issue_queue_avail_mask;

  if (logable(1)) {
    logfile << intstring(uop.uuid, 20), " clustr rob ", intstring(index(), -3), " allowed FUs = ", 
      bitstring(opinfo[uop.opcode].fu, FU_COUNT, true), " -> clusters ", bitstring(executable_on_cluster, MAX_CLUSTERS, true), " avail";
    foreach (i, MAX_CLUSTERS) logfile << " ", cluster_issue_queue_avail_count[i];
    logfile << endl;
  }
  
  if (!executable_on_cluster) {
    dispatch_cluster_none_avail++;
    return -1;
  }
  
  int n = 0;
  int cluster = find_random_set_bit(executable_on_cluster, sim_cycle);
  
  foreach (i, MAX_CLUSTERS) {
    if ((cluster_operand_tally[i] > n) && (executable_on_cluster & (1 << i))) {
      n = cluster_operand_tally[i];
      cluster = i;
    }
  }

  dispatch_cluster_histogram[cluster]++;

  return cluster;
}

stringbuf& ReorderBufferEntry::get_operand_info(stringbuf& sb, int operand) const {
  PhysicalRegister& physreg = *operands[operand];
  ReorderBufferEntry& sourcerob = *physreg.rob;
  StateList* state = physreg.current_state_list;

  sb << "r", physreg.index();

  if (state == &physreg_written_list) {
    sb << " (written)";
  } else if (state == &physreg_ready_list) {
    sb << " (ready)";
  } else if (state == &physreg_used_list) {
    sb << " (wait rob ", sourcerob.index(), " uuid ", sourcerob.uop.uuid, ")";
  } else if (state == &physreg_arch_list) {
    if (physreg.index() == PHYS_REG_NULL)  sb << " (zero)"; else sb << " (arch ", arch_reg_names[physreg.archreg], ")";
  } else if (state == &physreg_pendingfree_list) {
    sb << " (pending free for ", arch_reg_names[physreg.archreg], ")";
  } else {
    // cannot be in free state!
    sb << " (FREE)";
    //assert(false);
  }

  return sb;
}

ostream& ReorderBufferEntry::print_operand_info(ostream& os, int operand) const {
  stringbuf sb;
  get_operand_info(sb, operand);
  os << sb;
  return os;
}

ostream& ReorderBufferEntry::print(ostream& os) const {
  stringbuf name, rainfo, rbinfo, rcinfo;
  nameof(name, uop);
  get_operand_info(rainfo, 0);
  get_operand_info(rbinfo, 1);
  get_operand_info(rcinfo, 2);

  os << "rob ", intstring(index(), -3), " uuid ", intstring(uop.uuid, 16), " ", padstring(current_state_list->name, -24), " @ ", padstring((cluster >= 0) ? clusters[cluster].name : "???", -4), " ", padstring(name, -12), " r", 
    intstring(physreg->index(), -3), " ", padstring(arch_reg_names[uop.rd], -6);
  if (isload(uop.opcode)) 
    os << " ld", intstring(lsq->index(), -3);
  else if (isstore(uop.opcode))
    os << " st", intstring(lsq->index(), -3);
  else os << "      ";

  os << " = ";
  os << padstring(rainfo, -30);
  os << padstring(rbinfo, -30);
  os << padstring(rcinfo, -30);

  return os;
}

ostream& LoadStoreQueueEntry::print(ostream& os) const {
  os << (store ? "st" : "ld"), intstring(index(), -3), " ";
  os << "uuid ", intstring(rob->uop.uuid, 10), " ";
  os << "rob ", intstring(rob->index(), -3), " ";
  os << "r", intstring(rob->physreg->index(), -3), " ";
  if (invalid) {
    os << "< Invalid: fault 0x", hexstring(data, 8), " > ";
  } else {
    if (datavalid)
      os << bytemaskstring((const byte*)&data, bytemask, 8);
    else os << "<    Data Invalid     >";
    os << " @ ";
    if (addrvalid)
      os << "0x", hexstring(physaddr << 3, 48);
    else os << "< Addr Inval >";
  }    
  return os;
}

void print_rob(ostream& os) {
  os << "ROB head ", ROB.head, " to tail ", ROB.tail, " (", ROB.count, " entries):", endl;
  foreach_forward(ROB, i) {
    ReorderBufferEntry& rob = ROB[i];
    os << "  ", rob, endl;
  }
}

void print_lsq(ostream& os) {
  os << "LSQ head ", LSQ.head, " to tail ", LSQ.tail, " (", LSQ.count, " entries):", endl;
  foreach_forward(LSQ, i) {
    LoadStoreQueueEntry& lsq = LSQ[i];
    os << "  ", lsq, endl;
  }
}

//
// This function locates the source operands for a uop and prepares to add the
// uop to its cluster's issue queue.
//
// If an operand is already ready at dispatch time, the issue queue associative
// array slot for that operand is marked as unused; otherwise it is marked
// as valid so the operand's ROB index can be matched when broadcast.
//
// returns: 1 iff all operands were ready at dispatch time
//

bool ReorderBufferEntry::find_sources() {
  int operands_still_needed = 0;

  byte uopids[MAX_OPERANDS];
  byte preready[MAX_OPERANDS];

  foreach (operand, MAX_OPERANDS) {
    PhysicalRegister& source_physreg = *operands[operand];

    ReorderBufferEntry& source_rob = *source_physreg.rob;
    StateList* state = source_physreg.current_state_list;

    if (state == &physreg_used_list) {
      uopids[operand] = source_rob.index();
      preready[operand] = 0;
      operands_still_needed++;
    } else {
      // No need to wait for it
      uopids[operand] = 0;
      preready[operand] = 1;
    }

    if (source_physreg.nonnull()) state->dispatch_source_counter++;
  }

  //
  // Stores are special: we can issue a store even if its rc operand (the value
  // to store) is not yet ready. In this case the store uop just checks for
  // exceptions, establishes an STQ entry and gets replayed as a second phase
  // store (this time around with the rc dependency required)
  //
  if (isstore(uop.opcode) && !load_store_second_phase) {
    preready[RC] = 1;
  }

  bool ok;
  issueq_operation_on_cluster_with_result(cluster, ok, insert(index(), uopids, preready));
  assert(ok);

  return operands_still_needed;
}

//
// Dispatch any instructions in the rob_ready_to_dispatch_list by locating
// their source operands, updating any wait queues and expanding immediates.
//

CycleTimer ctdispatch;

W64 dispatch_width_histogram[DISPATCH_WIDTH+1];

int dispatch() {
  start_timer(ctdispatch);

  int dispatchcount = 0;

  ReorderBufferEntry* rob;

  foreach_list_mutable(rob_ready_to_dispatch_list, rob, entry, nextentry) {
    if (dispatchcount >= DISPATCH_WIDTH) break;

    // All operands start out as valid, then get put on wait queues if they are not actually ready.

    rob->cluster = rob->select_cluster();

    //
    // An available cluster could not be found. This only happens 
    // when all applicable cluster issue queues are full. Since
    // we are still processing instructions in order at this point,
    // abort dispatching for this cycle.
    //
    if (rob->cluster < 0) {
      if (logable(1)) logfile << intstring(rob->uop.uuid, 20), " cannot dispatch (no cluster)", endl, flush;
      break;
    }

    int operands_still_needed = rob->find_sources();

    if (operands_still_needed) {
      rob->changestate(rob_dispatched_list[rob->cluster]);
    } else {
      rob->changestate(rob->get_ready_to_issue_list());
    }

    if (logable(1)) {
      stringbuf rainfo, rbinfo, rcinfo;
      rob->get_operand_info(rainfo, 0);
      rob->get_operand_info(rbinfo, 1);
      rob->get_operand_info(rcinfo, 2);

      logfile << intstring(rob->uop.uuid, 20), " disptc rob ", intstring(rob->index(), -3), " to cluster ", clusters[rob->cluster].name,
        ": r", rob->physreg->index(), " = ", rainfo, "  ", rbinfo, "  ", rcinfo, endl;
    }

    dispatchcount++;
  }

  dispatch_width_histogram[dispatchcount]++;

  stop_timer(ctdispatch);
  return dispatchcount;
}

W32 fu_avail = bitmask(FU_COUNT);
indexref<ReorderBufferEntry> robs_on_fu[FU_COUNT];

struct IssueInput {
  W64 ra;
  W64 rb;
  W64 rc;
  W16 raflags;
  W16 rbflags;
  W16 rcflags;
};

//
// Release the ROB from the issue queue after there is
// no possibility it will need to be pulled back for
// replay or annulment.
//
void ReorderBufferEntry::release() {
  issueq_operation_on_cluster(cluster, release(iqslot));
  iqslot = -1;
}

//
// Replay the uop by recirculating it back to the dispatched
// state so it can wait for additional dependencies not known
// when it was originally dispatched, e.g. waiting on store
// queue entries or value to store, etc.
//
// This involves re-initializing the uop's operands in its
// already assigned issue queue slot and returning that slot
// to the dispatched but not issued state.
//
// This must be done here instead of simply sending the uop
// back to the dispatch state since otherwise we could have 
// a deadlock if there is not enough room in the issue queue.
//
void ReorderBufferEntry::replay() {
  if (logable(1)) {
    logfile << intstring(uop.uuid, 20), " replay rob ", intstring(index(), -3), " r", intstring(physreg->index(), -3), 
      " on cluster ", clusters[cluster].name, ": waiting on ";

    foreach (i, MAX_OPERANDS) {
      if (!operands[i]->ready()) {
        stringbuf sb;
        get_operand_info(sb, i);
        logfile << sb;
      }
    }
    logfile << endl;
  }

  int operands_still_needed = 0;

  byte uopids[MAX_OPERANDS];
  byte preready[MAX_OPERANDS];

  foreach (operand, MAX_OPERANDS) {
    PhysicalRegister& source_physreg = *operands[operand];

    ReorderBufferEntry& source_rob = *source_physreg.rob;
    StateList* state = source_physreg.current_state_list;

    if (state == &physreg_used_list) {
      uopids[operand] = source_rob.index();
      preready[operand] = 0;
      operands_still_needed++;
    } else {
      // No need to wait for it
      uopids[operand] = 0;
      preready[operand] = 1;
    }
  }

  if (operands_still_needed) {
    changestate(rob_dispatched_list[cluster]);
  } else {
    changestate(get_ready_to_issue_list());
  }

  issueq_operation_on_cluster(cluster, replay(iqslot, uopids, preready));
}

extern "C" void call_exec_func(const IssueInput& input, IssueState& output, const byte* func);

inline int check_access_alignment(W64 addr, AddressSpace::SPATChunk** top, bool annul, int sizeshift, bool internal, int exception) {
  if (lowbits(addr, sizeshift))
    return EXCEPTION_UnalignedAccess;

  //
  // This load/store was the high part of an unaligned store but the actual user
  // address did not touch the high 64 bits. Since it is perfectly legal to do
  // an unaligned store to the very end of the page such that the next 64 bit
  // chunk is not mapped to a valid page, we must not do any further checks:
  //
  if (annul | internal)
    return 0;

  return (asp.fastcheck(addr, top)) ? 0 : exception;
}

enum {
  ISSUE_COMPLETED = 1,
  ISSUE_NEEDS_REPLAY = 0,
  ISSUE_MISSPECULATED = -1,
};

//
// Load/Store Aliasing Prevention
//
// We always issue loads as soon as possible even if some entries in the
// store queue have unresolved addresses. If a load gets erroneously
// issued before an earlier store in program order to the same address,
// this is considered load/store aliasing.
// 
// Aliasing is detected when stores issue: the load queue is scanned
// for earlier loads in program order which collide with the store's
// address. In this case all uops in program order after and including
// the store (and by extension, the colliding load) must be annulled.
//
// To keep this from happening repeatedly, whenever a collision is
// detected, the store looks up the rip of the colliding load and adds
// it to a small table called the LSAP (load/store alias predictor).
//
// Loads query the LSAP with the rip of the load; if a matching entry
// is found in the LSAP and the store address is unresolved, the load
// is not allowed to proceed.
//

struct LoadStoreAliasPredictor: public FullyAssociativeTags<W64, 16> { };
LoadStoreAliasPredictor lsap;

// This is an internal MSR required to correctly truncate ld/st pointers in 32-bit mode
extern W64 virt_addr_mask;

//
// Stores have special dependency rules: they may issue as soon as operands ra and rb are ready,
// even if rc (the value to store) or rs (the store buffer to inherit from) is not yet ready or
// even known.
//
// After both ra and rb are ready, the store is moved to [ready_to_issue] as a first phase store.
// When the store issues, it generates its physical address [ra+rb] and establishes an SFR with
// the address marked valid but the data marked invalid.
//
// The sole purpose of doing this is to allow other loads and stores to create an rs dependency
// on the SFR output of the store.
//
// The store is then marked as a second phase store, since the address has been generated.
// When the store is replayed and rescheduled, it must now have all operands ready this time.
//

CycleTimer ctstore;

int ReorderBufferEntry::issuestore(LoadStoreQueueEntry& state, W64 ra, W64 rb, W64 rc, bool rcready) {
  int sizeshift = uop.size;
  int aligntype = uop.cond;
  bool internal = uop.internal;

  //
  // Make sure the address is aligned and the page tables map it
  //
  AddressSpace::SPATChunk** top = (AddressSpace::SPATChunk**)asp.writemap;

  W64 raddr = ra + rb;
  raddr &= virt_addr_mask;
  W64 origaddr = raddr;
  bool annul = 0;

  switch (aligntype) {
  case LDST_ALIGN_NORMAL:
    break;
  case LDST_ALIGN_LO:
    raddr = floor(raddr, 8); break;
  case LDST_ALIGN_HI:
    //
    // Is the high load ever even used? If not, don't check for exceptions;
    // otherwise we may erroneously flag page boundary conditions as invalid
    //
    raddr = floor(raddr, 8);
    annul = (floor(origaddr + ((1<<sizeshift)-1), 8) == raddr);
    raddr += 8;
    break;
  }

  W64 addr = lowbits(raddr, VIRT_ADDR_BITS);
  state.physaddr = addr >> 3;
  state.invalid = 0;
  //
  // Notice that datavalid is not set until both the rc operand to
  // store is ready AND any inherited SFR data is ready to merge.
  //
  state.datavalid = 0;
  state.addrvalid = 1;

  store_type_aligned += ((!uop.internal) & (aligntype == LDST_ALIGN_NORMAL));
  store_type_unaligned += ((!uop.internal) & (aligntype != LDST_ALIGN_NORMAL));
  store_type_internal += uop.internal;
  store_size[sizeshift]++;

  //
  // Special case: if no part of the actual user load/store falls inside
  // of the high 64 bits, do not perform the access and do not signal
  // any exceptions if that page was invalid.
  //
  // However, we must be extremely careful if we're inheriting an SFR
  // from an earlier store: the earlier store may have updated some
  // bytes in the high 64-bit chunk even though we're not updating
  // any bytes. In this case we still must do the write since it
  // could very well be the final commit to that address. In any
  // case, the SFR mismatch and LSAT must still be checked.
  //
  // The store commit code checks if the bytemask is zero and does
  // not attempt the actual store if so. This will always be correct
  // for high stores as described in this scenario.
  //

  LoadStoreQueueEntry* sfra = null;
  bool ready;
  byte bytemask;

  W64 exception = check_access_alignment(addr, top, annul, uop.size, uop.internal, EXCEPTION_PageFaultOnWrite);

  if (exception) {
    state.invalid = 1;
    state.data = exception;
    state.datavalid = 1;

    if (logable(1)) 
      logfile << intstring(uop.uuid, 20), " store", (load_store_second_phase ? "2" : " "), " rob ", intstring(index(), -3), " st", lsq->index(), 
        " r", intstring(physreg->index(), -3), " on ", padstring(FU[fu].name, -4), " @ ",
        "0x", hexstring(addr, 48), ": exception ", exception_name(exception), endl;

    if (exception == EXCEPTION_UnalignedAccess) {
      //
      // If we have an unaligned access, mark all loads and stores at this 
      // macro-op's rip as being unaligned and remove the basic block from
      // the bbcache so it gets retranslated with properly split loads
      // and stores after we resume fetching.
      //
      // As noted elsewhere, the bbcache is for simulator purposes only;
      // the real hardware would detect unaligned uops in the fetch stage
      // and split them up on the fly. For simulation, it's more efficient
      // to just split them once in the bbcache; this has no performance
      // effect on the cycle accurate results.
      //
      if (logable(1)) logfile << intstring(uop.uuid, 20), " unalgn ", (void*)uop.rip, ": mark all uops in macro-op as unaligned and replay to split", endl;

      BasicBlock* bb = bbcache.remove(uop.rip);
      bbcache_removes++;
      if (bb) bb->free();
      // NOTE: bb must not be accessed after this point!

      add_unaligned_ldst_rip(uop.rip);

      W64 recoveryrip = annul_after_and_including();
      reset_fetch_unit(recoveryrip);

      store_issue_unaligned++;

      return ISSUE_MISSPECULATED;
    }

    store_issue_exception++;

    return ISSUE_COMPLETED;
  }

  //
  // The STQ is then searched for the most recent prior store S to same 64-bit block. If found, U's
  // rs dependency is set to S by setting the ROB's rs field to point to the prior store's physreg
  // and hence its ROB. If not found, U's rs dependency remains unset (i.e. to PHYS_REG_NULL).
  // If some prior stores are ambiguous (addresses not resolved yet), we assume they are a match
  // to ensure correctness yet avoid additional checks; the store is replayed and tries again 
  // when the ambiguous reference resolves.
  //
  sfra = null;

  foreach_backward_before(LSQ, lsq, i) {
    LoadStoreQueueEntry& stbuf = LSQ[i];

    if (stbuf.store && (!stbuf.addrvalid || (stbuf.addrvalid && (stbuf.physaddr == state.physaddr)))) {
      assert(stbuf.rob->uop.uuid < uop.uuid);
      sfra = &stbuf;
      break;
    }
  }

  ready = (!sfra || (sfra && sfra->addrvalid && sfra->datavalid)) && rcready;

  //
  // If any of the following are true:
  // - Prior store S with same address is found but its data is not ready
  // - Prior store S with unknown address is found
  // - Data to store (rc operand) is not yet ready
  //
  // Then the store is moved back into [ready_to_dispatch], where this time all operands are checked.
  // The replay() function will put the newly selected prior store S's ROB as the rs dependency
  // of the current store before replaying it.
  //
  // When the current store wakes up again, it will rescan the STQ to see if any intervening stores
  // slipped in, and may repeatedly go back to sleep on the new store until the entire chain of stores
  // to a given location is resolved in the correct order. This does not mean all stores must issue in
  // program order - it simply means stores to the same address (8-byte chunk) are serialized in
  // program order, but out of order w.r.t. unrelated stores. This is similar to the constraints on
  // store buffer merging in Pentium 4 and AMD K8.
  //

  if (!ready) {
    // See notes above on Physical Register Recycling Complications
    operands[RS]->unref(*this);
    operands[RS] = (sfra) ? sfra->rob->physreg : &physregs[PHYS_REG_NULL];
    operands[RS]->addref(*this);

    if (logable(1)) {
      logfile << intstring(uop.uuid, 20), " store", (load_store_second_phase ? "2" : " "), " rob ", intstring(index(), -3), " st", lsq->index(), 
        " r", intstring(physreg->index(), -3), " on ", padstring(FU[fu].name, -4), " @ ",
        "0x", hexstring(addr, 48), " wait on ";
      if (!rcready) logfile << " rc";
      if (sfra) { 
        logfile << ((rcready) ? "" : " and "), *sfra, " (uuid ", sfra->rob->uop.uuid, " rob", sfra->rob->index(), " r", sfra->rob->physreg->index(), ")";
      }
      logfile << endl;
    }

    replay();
    load_store_second_phase = 1;

    store_issue_replay_sfr_addr_and_data_and_data_to_store_not_ready += ((!rcready) & (sfra && (!sfra->addrvalid) & (!sfra->datavalid)));
    store_issue_replay_sfr_addr_and_data_to_store_not_ready += ((!rcready) & (sfra && (!sfra->addrvalid)));
    store_issue_replay_sfr_data_and_data_to_store_not_ready += ((!rcready) & (sfra && sfra->addrvalid && (!sfra->datavalid)));

    store_issue_replay_sfr_addr_and_data_not_ready += (rcready & (sfra && (!sfra->addrvalid) & (!sfra->datavalid)));
    store_issue_replay_sfr_addr_not_ready += (rcready & (sfra && ((!sfra->addrvalid) & (sfra->datavalid))));
    store_issue_replay_sfr_data_not_ready += (rcready & (sfra && (sfra->addrvalid & (!sfra->datavalid))));

    return ISSUE_NEEDS_REPLAY;
  }

  //
  // Check all later loads in LDQ to see if any have already issued
  // and have already obtained their data but really should have 
  // depended on the data generated by this store. If so, mark the
  // store as invalid (EXCEPTION_LoadStoreAliasing) so it annuls
  // itself and the load after it in program order at commit time.
  //

  foreach_forward_after (LSQ, lsq, i) {
    LoadStoreQueueEntry& ldbuf = LSQ[i];
    //
    // (see notes on Load Replay Conditions below)
    //

    if ((!ldbuf.store) & ldbuf.addrvalid & (ldbuf.physaddr == state.physaddr)) {
      state.invalid = 1;
      state.data = EXCEPTION_LoadStoreAliasing;
      state.datavalid = 1;

      if (logable(1)) {
        logfile << intstring(uop.uuid, 20), " store", (load_store_second_phase ? "2" : " "), " rob ", intstring(index(), -3), " st", lsq->index(), 
          " r", intstring(physreg->index(), -3), " on ", padstring(FU[fu].name, -4), " @ ", "0x", hexstring(addr, 48), 
          " aliased with ldbuf ", ldbuf.index(), " (uuid ", ldbuf.rob->uop.uuid, " rob", ldbuf.rob->index(), 
          " r", ldbuf.rob->physreg->index(), ")", " (add colliding load rip ", (void*)ldbuf.rob->uop.rip, "; replay from rip ", (void*)uop.rip, ")", endl, flush;
      }

      // Add the rip to the load to the load/store alias predictor:
      lsap.select(ldbuf.rob->uop.rip);

      //
      // Annul everything after and including the store: this implicitly
      // includes the aliased load following the store in program order.
      //
      W64 recoveryrip = annul_after_and_including();

      //
      // The fetch queue is reset and fetching is redirected to the
      // correct branch direction.
      //
      reset_fetch_unit(uop.rip);

      store_issue_ordering++;

      return ISSUE_MISSPECULATED;
    }
  }

  //
  // At this point all operands are valid, so merge the data and mark the store as valid.
  //

  switch (aligntype) {
  case LDST_ALIGN_NORMAL:
  case LDST_ALIGN_LO:
    bytemask = ((1 << (1 << sizeshift))-1) << (lowbits(origaddr, 3));
    rc <<= 8*lowbits(origaddr, 3);
    break;
  case LDST_ALIGN_HI:
    bytemask = ((1 << (1 << sizeshift))-1) >> (8 - lowbits(origaddr, 3));
    rc >>= 8*(8 - lowbits(origaddr, 3));
  }

  state.invalid = 0;
  state.data = (sfra) ? mux64(expand_8bit_to_64bit_lut[bytemask], sfra->data, rc) : rc;
  state.bytemask = (sfra) ? (sfra->bytemask | bytemask) : bytemask;
  state.datavalid = 1;

  store_forward_from_zero += (sfra == null);
  store_forward_from_sfr += (sfra != null);

  if (logable(1)) {
    logfile << intstring(uop.uuid, 20), " store", (load_store_second_phase ? "2" : " "), " rob ", intstring(index(), -3), " st", lsq->index(), 
      " r", intstring(physreg->index(), -3), " on ", padstring(FU[fu].name, -4), " @ ", "0x", hexstring(addr, 48);
    if (sfra) logfile << " inherit from ", (*sfra), " (uuid ", sfra->rob->uop.uuid, ")";
    logfile << " <= 0x", hexstring(rc, 8*(1<<uop.size)), " (size ", (1<<uop.size), ") = ", state, endl;
  }

  load_store_second_phase = 1;

  store_issue_complete++;

  return ISSUE_COMPLETED;
}

static inline W64 loaddata(void* target, int SIZESHIFT, bool SIGNEXT) {
  W64 data;
  switch (SIZESHIFT) {
  case 0:
    data = (SIGNEXT) ? (W64s)(*(W8s*)target) : (*(W8*)target); break;
  case 1:
    data = (SIGNEXT) ? (W64s)(*(W16s*)target) : (*(W16*)target); break;
  case 2:
    data = (SIGNEXT) ? (W64s)(*(W32s*)target) : (*(W32*)target); break;
  case 3:
    data = *(W64*)target; break;
  }
  return data;
}

CycleTimer ctload;

int ReorderBufferEntry::issueload(LoadStoreQueueEntry& state, W64 ra, W64 rb, W64 rc) {
  //bool DEBUG = analyze_in_detail();
  static const bool DEBUG = 0;

  int sizeshift = uop.size;
  int aligntype = uop.cond;
  bool internal = uop.internal;
  bool signext = (uop.opcode == OP_ldx);

  //
  // Make sure the address is aligned and the page tables map it
  //
  AddressSpace::SPATChunk** top = (AddressSpace::SPATChunk**)asp.readmap;

  W64 raddr = ra + rb;
  if (aligntype == LDST_ALIGN_NORMAL) raddr += (rc << uop.extshift);
  raddr &= virt_addr_mask;
  W64 origaddr = raddr;
  bool annul = 0;

  switch (aligntype) {
  case LDST_ALIGN_NORMAL:
    break;
  case LDST_ALIGN_LO:
    raddr = floor(raddr, 8); break;
  case LDST_ALIGN_HI:
    //
    // Is the high load ever even used? If not, don't check for exceptions;
    // otherwise we may erroneously flag page boundary conditions as invalid
    //
    raddr = floor(raddr, 8);
    annul = (floor(origaddr + ((1<<sizeshift)-1), 8) == raddr);
    raddr += 8; 
    break;
  }

  W64 addr = lowbits(raddr, VIRT_ADDR_BITS);

  state.physaddr = addr >> 3;
  state.addrvalid = 0;
  state.datavalid = 0;
  state.invalid = 0;

  load_type_aligned += ((!uop.internal) & (aligntype == LDST_ALIGN_NORMAL));
  load_type_unaligned += ((!uop.internal) & (aligntype != LDST_ALIGN_NORMAL));
  load_type_internal += uop.internal;
  load_size[sizeshift]++;

  W64 exception = check_access_alignment(addr, top, annul, uop.size, uop.internal, EXCEPTION_PageFaultOnRead);

  if (exception) {
    state.invalid = 1;
    state.data = exception;
    state.datavalid = 1;

    if (logable(1)) 
      logfile << intstring(uop.uuid, 20), " load", (load_store_second_phase ? "2" : " "), "  rob ", intstring(index(), -3), " st", lsq->index(), 
        " r", intstring(physreg->index(), -3), " on ", padstring(FU[fu].name, -4), " @ ",
        "0x", hexstring(addr, 48), ": exception ", exception_name(exception), endl;

    if (exception == EXCEPTION_UnalignedAccess) {
      // (see notes above for issuestore case)
      if (logable(1)) logfile << intstring(uop.uuid, 20), " unalgn ", (void*)uop.rip, ": mark all uops in macro-op as unaligned and replay to split", endl;

      BasicBlock* bb = bbcache.remove(uop.rip);
      bbcache_removes++;
      if (bb) bb->free();
      // NOTE: bb must not be accessed after this point!

      add_unaligned_ldst_rip(uop.rip);
      W64 recoveryrip = annul_after_and_including();
      reset_fetch_unit(recoveryrip);
      load_issue_unaligned++;
      return ISSUE_MISSPECULATED;
    }

    load_issue_exception++;
    return ISSUE_COMPLETED;
  }

  //
  // For simulation purposes only, load the data immediately
  // so it is easier to track. In the hardware this obviously
  // only arrives later, but it saves us from having to copy
  // cache lines around...
  //
  W64 data;

  LoadStoreQueueEntry* sfra = null;

  bool load_is_known_to_alias_with_store = (lsap(uop.rip) >= 0);

  foreach_backward_before(LSQ, lsq, i) {
    LoadStoreQueueEntry& stbuf = LSQ[i];

    if (!stbuf.store) continue;

    if ((load_is_known_to_alias_with_store & (!stbuf.addrvalid)) || ((stbuf.physaddr == state.physaddr) & stbuf.addrvalid)) {
      load_dependency_predicted_alias_unresolved += (load_is_known_to_alias_with_store);
      load_dependency_stq_address_match += (!load_is_known_to_alias_with_store);
      sfra = &stbuf;
      break;
    }
  }

  load_dependency_independent += (sfra == null);

  bool ready = (!sfra || (sfra && sfra->addrvalid && sfra->datavalid));

  if (!ready) {
    //
    // Load Replay Conditions:
    //
    // - Earlier store is known to alias (based on rip) yet its address is not yet resolved
    // - Earlier store to the same 8-byte chunk was found but its data has not yet arrived
    //
    // In these cases we create an rs dependency on the earlier store and replay the load uop
    // back to the dispatched state. It will be re-issued once the earlier store resolves.
    //
    // Consider the following sequence of events:
    // - Load B issues
    // - Store A issues and detects aliasing with load B; both A and B annulled
    // - Load B attempts to re-issue but aliasing is predicted, so it creates a dependency on store A
    // - Store A issues but sees that load B has already attempted to issue, so an aliasing replay is taken
    //
    // This becomes an infinite loop unless we clear both the addrvalid and datavalid fields of loads
    // when they replay; clearing both suppresses the aliasing replay the second time around.
    //

    assert(sfra);
    // See notes above on Physical Register Recycling Complications
    operands[RS]->unref(*this);
    operands[RS] = sfra->rob->physreg;
    operands[RS]->addref(*this);

    if (logable(1)) {
      logfile << intstring(uop.uuid, 20), " load", (load_store_second_phase ? "2" : " "), "  rob ", intstring(index(), -3), " st", lsq->index(), 
        " r", intstring(physreg->index(), -3), " on ", padstring(FU[fu].name, -4), " @ ", "0x", hexstring(addr, 48), " wait on ", 
        *sfra, " (uuid ", sfra->rob->uop.uuid, " rob", sfra->rob->index(), " r", sfra->rob->physreg->index(), ")";
      if (load_is_known_to_alias_with_store && sfra && (!sfra->addrvalid)) logfile << "; stalled by predicted aliasing";
      logfile << endl;
    }

    load_issue_replay_sfr_addr_and_data_not_ready += ((!sfra->addrvalid) & (!sfra->datavalid));
    load_issue_replay_sfr_addr_not_ready += ((!sfra->addrvalid) & (sfra->datavalid));
    load_issue_replay_sfr_data_not_ready += ((sfra->addrvalid) & (!sfra->datavalid));

    replay();
    load_store_second_phase = 1;
    return ISSUE_NEEDS_REPLAY;
  }

  state.addrvalid = 1;

  if (aligntype == LDST_ALIGN_HI) {
    //
    // Concatenate the aligned data from a previous ld.lo uop provided in rc
    // with the currently loaded data D as follows:
    //
    // rc | D
    //
    // Example:
    //
    // floor(a) floor(a)+8
    // ---rc--  --DD---
    // 0123456701234567
    //    XXXXXXXX
    //    ^ origaddr
    //
    if (!annul) {
      if (sfra) {
        data = mux64(expand_8bit_to_64bit_lut[sfra->bytemask], *((W64*)floor(addr, 8)), sfra->data);
      } else {
        data = *((W64*)floor(addr, 8));
      }
      
      struct {
        W64 lo;
        W64 hi;
      } aligner;
      
      aligner.lo = rc;
      aligner.hi = data;
      
      W64 offset = lowbits(origaddr - floor(origaddr, 8), 4);

      data = loaddata(((byte*)&aligner) + offset, sizeshift, signext);
    } else {
      //
      // annulled: we need no data from the high load anyway; only use the low data
      // that was already checked for exceptions and forwarding:
      //
      W64 offset = lowbits(origaddr, 3);
      state.data = loaddata(((byte*)&rc) + offset, sizeshift, signext);
      state.invalid = 0;
      state.datavalid = 1;

      if (logable(1)) {
        logfile << intstring(uop.uuid, 20), " load", (load_store_second_phase ? "2" : " "), "  rob ", intstring(index(), -3), " ld", lsq->index(), 
          " r", intstring(physreg->index(), -3), " on ", padstring(FU[fu].name, -4), " @ ", "0x", hexstring(addr, 48), " was annulled (high unaligned load)", endl;
      }

      return ISSUE_COMPLETED;
    }
  } else {
    // x86-64 requires virtual addresses to be canonical: if bit 47 is set, all upper 16 bits must be set
    W64 realaddr = (W64)signext64(addr, 48);
    if (sfra) {
      W64 predata = mux64(expand_8bit_to_64bit_lut[sfra->bytemask], *((W64*)floor(realaddr, 8)), sfra->data);
      data = loaddata(((byte*)&predata) + lowbits(addr, 3), sizeshift, signext);
    } else {
      data = loaddata((void*)realaddr, sizeshift, signext);
    }
  }

  // shift is how many bits to shift the 8-bit bytemask left by within the cache line;
  bool covered = covered_by_sfr(addr, sfra, sizeshift);
  load_forward_from_cache += (sfra == null);
  load_forward_from_sfr += ((sfra != null) & covered);
  load_forward_from_sfr_and_cache += ((sfra != null) & (!covered));

  //
  // NOTE: Technically the data is valid right now for simulation purposes
  // only; in reality it may still be arriving from the cache.
  //
  state.data = data;
  state.invalid = 0;
  state.bytemask = 0xff;

  bool L1hit = (perfect_cache) ? 1 : probe_cache_and_sfr(addr, sfra, sizeshift);

  if (L1hit) {    
    cycles_left = LOADLAT;
    
    if (logable(1)) {
      logfile << intstring(uop.uuid, 20), " load", (load_store_second_phase ? "2" : " "), "  rob ", intstring(index(), -3), " ld", lsq->index(), 
        " r", intstring(physreg->index(), -3), " on ", padstring(FU[fu].name, -4), " @ ", "0x", hexstring(addr, 48);
      if (sfra) logfile << " inherit from ", (*sfra), " (uuid ", sfra->rob->uop.uuid, ")";
      logfile << " = 0x", hexstring(state.data, 64), endl;
    }
    
    load_store_second_phase = 1;
    state.datavalid = 1;

    load_issue_complete++;
    load_hit_L1++;
    return ISSUE_COMPLETED;
  }

  load_issue_miss++;

  cycles_left = 0;
  changestate(rob_cache_miss_list);

  LoadStoreInfo lsi;
  lsi.info.tag = index();
  lsi.info.rd = physreg->index();
  lsi.info.cbslot = lsq->index();
  lsi.info.sequential = 0;
  lsi.info.commit = 0;
  lsi.info.sizeshift = sizeshift;
  lsi.info.aligntype = aligntype;
  lsi.info.sfraused = (sfra != null);
  lsi.info.internal = internal;
  lsi.info.signext = signext;

  //
  // NOTE: this state is not really used anywhere since load misses
  // will fill directly into the physical register instead.
  //
  IssueState tempstate;
  lfrqslot = issueload_slowpath(tempstate, addr, origaddr, data, *sfra, lsi);

  if (logable(1)) {
    logfile << intstring(uop.uuid, 20), " ldmis", (load_store_second_phase ? "2" : " "), " rob ", intstring(index(), -3), " ld", lsq->index(), 
      " r", intstring(physreg->index(), -3), " on ", padstring(FU[fu].name, -4), " @ ", "0x", hexstring(addr, 48);
    if (sfra) logfile << " inherit from ", (*sfra), " (uuid ", sfra->rob->uop.uuid, ")";
    logfile << " = 0x", hexstring(state.data, 64), " [L1 miss to LFRQ slot ", lfrqslot, "]", endl;
  }

  if (lfrqslot < 0) {
    logfile << intstring(uop.uuid, 20), " load  ", " rob ", intstring(index(), -3), " ld", lsq->index(), 
      " r", intstring(physreg->index(), -3), ": LFRQ or miss buffer full; replaying", endl;

    load_issue_replay_missbuf_full++;

    state.addrvalid = 0;
    replay();
    return ISSUE_NEEDS_REPLAY;
  }

  return ISSUE_COMPLETED;
}

//
// Data cache has delivered a load: wake up corresponding ROB/LSQ/physreg entries
//
void ReorderBufferEntry::loadwakeup() {
  if (logable(1)) logfile << intstring(uop.uuid, 20), " ldwake", " rob ", intstring(index(), -3), " ld", lsq->index(), 
    " r", intstring(physreg->index(), -3), ": wakeup load from lfrq slot ", lfrqslot, endl;

  physreg->flags &= ~FLAG_WAIT;
  physreg->complete();

  lsq->datavalid = 1;

  changestate(rob_completed_list[cluster]);
  cycles_left = 0;
  lfrqslot = -1;
  forward_cycle = 0;
  fu = 0;
}

W64 load_filled_callback(LoadStoreInfo lsi, W64 addr) {
  if (lsi.info.rd == PHYS_REG_NULL)
    return 0; // ignore prefetches

  assert(lsi.info.tag < ROB_SIZE);
  ReorderBufferEntry& rob = ROB[lsi.info.tag];
  assert(rob.current_state_list == &rob_cache_miss_list);
  rob.loadwakeup();
  return 0;
}

//
// Remove any and all ROBs that entered the pipeline after and
// including the misspeculated uop. Because we move all affected
// ROBs to the free state, they are instantly taken out of 
// consideration for future pipeline stages and will be dropped on 
// the next cycle.
//
// Normally this means that mispredicted branch uops are annulled 
// even though only the code after the branch itself is invalid.
// In this special case, the recovery rip is set to the actual
// target of the branch rather than refetching the branch insn.
//
// We must be extremely careful to annul all uops in an
// x86 macro-op; otherwise half the x86 instruction could
// be executed twice once refetched. Therefore, if the
// first uop to annul is not also the first uop in the x86
// macro-op, we may have to scan backwards in the ROB until
// we find the first uop of the macro-op. In this way, we
// ensure that we can annul the entire macro-op. All uops
// comprising the macro-op are guaranteed to still be in 
// the ROB since none of the uops commit until the entire
// macro-op can commit.
//
// Note that this does not apply if the final uop in the
// macro-op is a branch and that branch uop itself is
// being retained as occurs with mispredicted branches.
//

CycleTimer ctannul;

W64 ReorderBufferEntry::annul(bool keep_misspec_uop) {
  start_timer(ctannul);

  int idx;

  //
  // Pass 0: determine macro-op boundaries around uop
  //
  int somidx = index();
  while (!ROB[somidx].uop.som) somidx = add_index_modulo(somidx, -1, ROB_SIZE);
  int eomidx = index();
  while (!ROB[eomidx].uop.eom) eomidx = add_index_modulo(eomidx, +1, ROB_SIZE);

  // Find uop to start annulment at
  int startidx = (keep_misspec_uop) ? add_index_modulo(eomidx, +1, ROB_SIZE) : somidx;
  if (startidx == ROB.tail) {
    // The uop causing the mis-speculation was the only uop in the ROB:
    // no action is necessary (but in practice this is generally not possible)
    if (logable(1)) 
      logfile << intstring(uop.uuid, 20), " misspc rob ", intstring(index(), -3), 
        ": SOM rob ", somidx, ", EOM rob ", eomidx, ": no future uops to annul", endl;
    return uop.rip;
  }

  // Find uop to stop annulment at (later in program order)
  int endidx = add_index_modulo(ROB.tail, -1, ROB_SIZE);

  // For branches, branch must always terminate the macro-op
  if (keep_misspec_uop) assert(eomidx == index());

  if (logable(1)) {
    logfile << intstring(uop.uuid, 20), " misspc rob ", intstring(index(), -3), ": SOM rob ", somidx, 
      ", EOM rob ", eomidx, ": annul from rob ", startidx, " to rob ", endidx, endl;
  }

  //
  // Pass 1: invalidate issue queue slot for the annulled ROB
  //
  idx = endidx;
  for (;;) {
    ReorderBufferEntry& annulrob = ROB[idx];
    issueq_operation_on_cluster(annulrob.cluster, annuluop(annulrob.index()));
    annulrob.iqslot = -1;
    if (idx == startidx) break;
    idx = add_index_modulo(idx, -1, ROB_SIZE);
  }

  int annulcount = 0;

  //
  // Pass 2: reconstruct the SpecRRT as it existed just before (or after)
  // the mis-speculated operation. This is done using the fast flush with
  // pseudo-commit method as follows:
  //
  // First overwrite the SpecRRT with the CommitRRT.
  //
  // Then, simulate the commit of all non-speculative ROBs up to the branch
  // by updating the SpecRRT as if it were the CommitRRT. This brings the
  // speculative RRT to the same state as if all in flight nonspeculative
  // operations before the branch had actually committed. Resume instruction 
  // fetch at the correct branch target.
  //
  // Other methods (like backwards walk) are difficult to impossible because
  // of the requirement that flag rename tables be restored even if some
  // of the required physical registers with attached flags have since been
  // freed. Therefore we don't do this.
  //
  // Technically RRT checkpointing could be used but due to the load/store
  // replay mechanism in use, this would require a checkpoint at every load
  // and store as well as branches.
  //
  foreach (i, TRANSREG_COUNT) { specrrt[i]->unspecref(i); }
  specrrt = commitrrt;
  foreach (i, TRANSREG_COUNT) { specrrt[i]->addspecref(i); }

  if (logable(1)) logfile << "Restored SpecRRT from CommitRRT; walking forward from:", specrrt, endl;

  idx = ROB.head;
  for (idx = ROB.head; idx != startidx; idx = add_index_modulo(idx, +1, ROB_SIZE)) {
    ReorderBufferEntry& rob = ROB[idx];
    rob.pseudocommit();
  }

  //
  // Pass 3: For each speculative ROB, reinitialize and free speculative ROBs
  //

  ReorderBufferEntry* lastrob = null;

  idx = endidx;
  for (;;) {
    ReorderBufferEntry& annulrob = ROB[idx];

    lastrob = &annulrob;

    if (logable(1)) {
      logfile << intstring(annulrob.uop.uuid, 20), " annul  rob ", intstring(annulrob.index(), -3), ": annul rip ", (void*)annulrob.uop.rip;
      logfile << (annulrob.uop.som ? " SOM" : "    ");
      logfile << (annulrob.uop.eom ? " EOM" : "    ");
      logfile << ": free";
    }

    //
    // Free the speculatively allocated physical register
    // See notes above on Physical Register Recycling Complications
    //
    if (logable(1)) logfile << " r", annulrob.physreg->index();
    foreach (j, MAX_OPERANDS) { annulrob.operands[j]->unref(annulrob); }
    annulrob.physreg->free();

    if (isclass(annulrob.uop.opcode, OPCLASS_LOAD|OPCLASS_STORE)) {
      if (logable(1)) logfile << " lsq", annulrob.lsq->index();
      LSQ.annul(annulrob.lsq);
    }

    if (annulrob.lfrqslot >= 0) {
      if (logable(1)) logfile << " lfrq", annulrob.lfrqslot;
      annul_lfrq_slot(annulrob.lfrqslot);
    }

    if (isbranch(annulrob.uop.opcode) && (annulrob.uop.predinfo.bptype & (BRANCH_HINT_CALL|BRANCH_HINT_RET))) {
      //
      // Return Address Stack (RAS) correction:
      // Example calls and returns in pipeline
      //
      // C1
      //   C2
      //   R2 
      //   BR (mispredicted branch)
      //   C3
      //     C4
      //
      // BR mispredicts, so everything after BR must be annulled.
      // RAS contains: C1 C3 C4, so we need to annul [C4 C3].
      //
      if (logable(1)) logfile << " annulras";
      branchpred.annulras(annulrob.uop.predinfo);
    }

    ROB.annul(annulrob);

    annulrob.changestate(rob_free_list);
    annulcount++;

    if (logable(1)) logfile << endl;

    if (idx == startidx) break;
    idx = add_index_modulo(idx, -1, ROB_SIZE);
  }

  assert(ROB[startidx].uop.som);

  stop_timer(ctannul);

  return (keep_misspec_uop) ? ROB[startidx].uop.riptaken : ROB[startidx].uop.rip;
}

//
// Issue a single ROB. 
//
// Returns:
//  +1 if issue was successful
//   0 if no functional unit was available
//  -1 if there was an exception and we should stop issuing this cycle
//

W64 issue_total_uops;

// totals 100%:
W64 issue_result_no_fu;
W64 issue_result_replay;
W64 issue_result_misspeculation;
W64 issue_result_branch_mispredict;
W64 issue_result_exception;
W64 issue_result_complete;

// totals 100%:
W64 issue_width_histogram[MAX_CLUSTERS][MAX_ISSUE_WIDTH+1];

// totals 100%:
W64 issue_opclass_histogram[OPCLASS_COUNT];

CycleTimer ctsubexec;
CycleTimer ctsubexec2;

int ReorderBufferEntry::issue() {
  struct FunctionalUnit* FU_assigned = null;

  W32 executable_on_fu = opinfo[uop.opcode].fu & clusters[cluster].fu_mask & fu_avail;

  // Are any FUs available in this cycle?
  if (!executable_on_fu) {
    if (logable(1)) { 
      logfile << intstring(uop.uuid, 20), " issnfu rob ", intstring(index(), -3), " no FUs available in cluster ", clusters[cluster].name, ": ",
        "fu_avail = ", bitstring(fu_avail, FU_COUNT, true), ", ",
        "op_fu = ", bitstring(opinfo[uop.opcode].fu, FU_COUNT, true), ", "
        "fu_cl_mask = ", bitstring(clusters[cluster].fu_mask, FU_COUNT, true), endl;
    }
    issue_result_no_fu++;
    //
    // When this (very rarely) happens, stop issuing uops to this cluster
    // and try again with the problem uop on the next cycle. In practice
    // this scenario rarely happens.
    //
    issueq_operation_on_cluster(cluster, replay(iqslot));
    return ISSUE_NEEDS_REPLAY;
  }

  start_timer(ctsubexec);

  issue_total_uops++;

  fu = lsbindex(executable_on_fu);
  clearbit(fu_avail, fu);
  robs_on_fu[fu] = this;
  cycles_left = opinfo[uop.opcode].latency;

  changestate(rob_issued_list[cluster]);

  PhysicalRegister& ra = *operands[RA];
  PhysicalRegister& rb = *operands[RB];
  PhysicalRegister& rc = *operands[RC];

  IssueState state;
  state.reg.rdflags = 0;

  IssueInput input;
  input.ra = ra.data;
  input.rb = (uop.rb == REG_imm) ? uop.rbimm : rb.data;
  input.rc = (uop.rc == REG_imm) ? uop.rcimm : rc.data;

  input.raflags = ra.flags;
  input.rbflags = (uop.rb == REG_imm) ? 0 : rb.flags;
  input.rcflags = (uop.rc == REG_imm) ? 0 : rc.flags;

  bool ld = isload(uop.opcode);
  bool st = isstore(uop.opcode);
  bool br = isbranch(uop.opcode);

  assert(operands[RA]->ready());
  if (uop.rb != REG_imm) assert(rb.ready());
  if ((!st || (st && load_store_second_phase)) && (uop.rc != REG_imm)) assert(rc.ready());
  if (!st) assert(operands[RS]->ready());

  if (ra.nonnull()) {
    ra.current_state_list->issue_source_counter++;
    ra.all_consumers_sourced_from_bypass &= (ra.current_state_list == &physreg_ready_list);
  }

  if ((!uop.rbimm) & (rb.nonnull())) { 
    rb.current_state_list->issue_source_counter++; 
    rb.all_consumers_sourced_from_bypass &= (rb.current_state_list == &physreg_ready_list);
  }

  if ((!uop.rcimm) & (rc.nonnull())) {
    rc.current_state_list->issue_source_counter++;
    rc.all_consumers_sourced_from_bypass &= (rc.current_state_list == &physreg_ready_list);
  }

  bool propagated_exception = 0;
  if ((input.raflags | input.rbflags | input.rcflags) & FLAG_INV) {
    //
    // Invalid data propagated through operands: mark output as
    // invalid and don't even execute the uop at all.
    //
    state.st.invalid = 1;
    state.reg.rdflags = FLAG_INV;
    state.reg.rddata = EXCEPTION_Propagate;
    propagated_exception = 1;
  } else {
    issue_opclass_histogram[opclassof(uop.opcode)]++;

    if (ld|st) {
      start_timer((ld ? ctload : ctstore));
      int completed = (ld) ? issueload(*lsq, input.ra, input.rb, input.rc) : issuestore(*lsq, input.ra, input.rb, input.rc, operand_ready(2));
      stop_timer((ld ? ctload : ctstore));
      if (completed == ISSUE_MISSPECULATED) {
        issue_result_misspeculation++;
        return -1;
      }
      state.reg.rddata = lsq->data;
      state.reg.rdflags = (lsq->invalid << log2(FLAG_INV)) | ((!lsq->datavalid) << log2(FLAG_WAIT));
      if (completed == ISSUE_NEEDS_REPLAY) {
        issue_result_replay++;
        return 0;
      }
    } else if (br) {
      state.brreg.riptaken = uop.riptaken;
      state.brreg.ripseq = uop.ripseq;
      call_exec_func(input, state, uop.synthop);

      if ((!isclass(uop.opcode, OPCLASS_BARRIER)) && (!asp.check((void*)state.reg.rddata, PROT_EXEC))) {
        // bogus branch
        state.reg.rdflags |= FLAG_INV;
        state.reg.rddata = EXCEPTION_PageFaultOnExec;
      }
    } else {
      call_exec_func(input, state, uop.synthop);
    }
  }

  stop_timer(ctsubexec);

  physreg->flags = state.reg.rdflags;
  physreg->data = state.reg.rddata;

  if (!physreg->valid()) {
    //
    // If the uop caused an exception, force it directly to the commit
    // state and not through writeback (this keeps dependencies waiting until 
    // they can be properly annulled by the speculation logic.) The commit 
    // stage will detect the exception and take appropriate action.
    //
    // If the exceptional uop was speculatively executed beyond a
    // branch, it will never reach commit anyway since the branch would
    // have to commit before the exception was ever seen.
    //
    cycles_left = 0;
    changestate(rob_ready_to_commit_queue);
    stall_frontend = true;
  }

  bool mispredicted = (physreg->data != uop.riptaken);

  if ((logable(1)) && (propagated_exception || (!(ld|st)))) {
    stringbuf rdstr; print_value_and_flags(rdstr, state.reg.rddata, state.reg.rdflags);
    stringbuf rastr; print_value_and_flags(rastr, ra.data, ra.flags);
    stringbuf rbstr; print_value_and_flags(rbstr, rb.data, rb.flags);
    stringbuf rcstr; print_value_and_flags(rcstr, rc.data, rc.flags);

    logfile << intstring(uop.uuid, 20), " issue  rob ", intstring(index(), -3), " on ", padstring(FU[fu].name, -4), ": r", intstring(physreg->index(), -3), " ";
    logfile << rdstr, " = ", rastr, ", ", rbstr, ", ", rcstr, " (", cycles_left, " left)";

    if (br & mispredicted) logfile << "; mispredicted (real ", (void*)physreg->data, " vs expected ", (void*)uop.riptaken, ")";
    logfile << endl;
  }

  start_timer(ctsubexec2);

  //
  // Release the issue queue entry, since we are beyond the point of no return:
  // the uop cannot possibly be replayed at this point, but may still be annulled.
  //
  release();

  stop_timer(ctsubexec2);

  if (physreg->valid()) {
    if (br) {
      int bptype = uop.predinfo.bptype;

      bool cond = bit(bptype, log2(BRANCH_HINT_COND));
      bool indir = bit(bptype, log2(BRANCH_HINT_INDIRECT));
      bool ret = bit(bptype, log2(BRANCH_HINT_RET));
        
      if (mispredicted) {
        branchpred_cond_mispred += cond;
        branchpred_indir_mispred += (indir & !ret);
        branchpred_return_mispred += ret;
        branchpred_total_mispred++;

        W64 realrip = physreg->data;

        //
        // Early misprediction handling. Annul everything after the
        // branch and restart fetching in the correct direction
        //
        annul_after();

        //
        // The fetch queue is reset and fetching is redirected to the
        // correct branch direction.
        //
        // Note that we do NOT just reissue the branch - this would be
        // pointless as we already know the correct direction since
        // it has already been issued once. Just let it writeback and
        // commit like it was predicted perfectly in the first place.
        //
        reset_fetch_unit(realrip);
        issue_result_branch_mispredict++;

        return -1;
      } else {
        branchpred_cond_correct += cond;
        branchpred_indir_correct += (indir & !ret);
        branchpred_return_correct += ret;
        branchpred_total_correct++;
        issue_result_complete++;
      }
    } else {
      issue_result_complete++;
    }
  } else {
    issue_result_exception++;
  }

  return 1;
}

//
// Process the ready to issue queue and issue as many ROBs as possible
//

CycleTimer ctissue;

static int issue(int cluster) {
  int issuecount = 0;
  ReorderBufferEntry* rob;

  start_timer(ctissue);

  int maxwidth = clusters[cluster].issue_width;

  while (issuecount < maxwidth) {
    int iqslot;
    issueq_operation_on_cluster_with_result(cluster, iqslot, issue());
  
    // Is anything ready?
    if (iqslot < 0) break;

    int robid;
    issueq_operation_on_cluster_with_result(cluster, robid, uopof(iqslot));
    assert(inrange(robid, 0, ROB_SIZE-1));
    ReorderBufferEntry& rob = ROB[robid];
    rob.iqslot = iqslot;
    int rc = rob.issue();
    // Stop issuing from this cluster once something replays or has a mis-speculation
    issuecount++;
    if (rc <= 0) break;
  }

  issue_width_histogram[cluster][min(issuecount, MAX_ISSUE_WIDTH)]++;

  stop_timer(ctissue);

  return issuecount;
}

//
// Process any ROB entries that just finished producing a result, forwarding
// data within the same cluster directly to the waiting instructions.
//
// Note that we use the target physical register as a temporary repository
// for the data. In a modern hardware implementation, this data would exist
// only "on the wire" such that back to back ALU operations within a cluster
// can occur using local forwarding.
//

CycleTimer ctcomplete;

static int complete(int cluster) {
  int completecount = 0;
  ReorderBufferEntry* rob;

  start_timer(ctcomplete);

  // 
  // Check the list of issued ROBs. If a given ROB is complete (i.e., is ready
  // for writeback and forwarding), move it to rob_completed_list.
  //
  foreach_list_mutable(rob_issued_list[cluster], rob, entry, nextentry) {
    rob->cycles_left--;

    if (rob->cycles_left <= 0) {
      if (logable(1)) {
        stringbuf rdstr; print_value_and_flags(rdstr, rob->physreg->data, rob->physreg->flags);
        logfile << intstring(rob->uop.uuid, 20), " complt rob ", intstring(rob->index(), -3), " on ", padstring(FU[rob->fu].name, -4), ": r", intstring(rob->physreg->index(), -3), " = ", rdstr, endl;
      }

      rob->changestate(rob_completed_list[cluster]);
      rob->physreg->complete();
      rob->forward_cycle = 0;
      rob->fu = 0;
      completecount++;
    }
  }

  stop_timer(ctcomplete);
  return 0;
}

//
// Forward the result of ROB 'result' to any other waiting ROBs
// dispatched to the issue queues. This is done by broadcasting
// the ROB tag to all issue queues in clusters reachable within
// N cycles after the uop issued, where N is forward_cycle. This
// technique is used to model arbitrarily complex multi-cycle
// forwarding networks.
//
int ReorderBufferEntry::forward() {
  ReorderBufferEntry* target;
  int wakeupcount = 0;

  assert(inrange(forward_cycle, 0, (MAX_FORWARDING_LATENCY+1)-1));

  W32 targets = forward_at_cycle_lut[cluster][forward_cycle];
  foreach (i, MAX_CLUSTERS) {
    if (!bit(targets, i)) continue;
    if (logable(1)) logfile << intstring(uop.uuid, 20), " brcast rob ", intstring(index(), -3), " from cluster ", clusters[cluster].name, " to cluster ", clusters[i].name, " on forwarding cycle ", forward_cycle, endl;

    issueq_operation_on_cluster(i, broadcast(index()));
  }

  return 0;
}

//
// Process ROBs in flight between completion and global forwarding/writeback.
//

CycleTimer cttransfer;

static int transfer(int cluster) {
  int wakeupcount = 0;
  ReorderBufferEntry* rob;

  start_timer(cttransfer);

  foreach_list_mutable(rob_completed_list[cluster], rob, entry, nextentry) {
    rob->forward();
    rob->forward_cycle++;
    if (rob->forward_cycle > MAX_FORWARDING_LATENCY) {
      rob->forward_cycle = MAX_FORWARDING_LATENCY;
      rob->changestate(rob_ready_to_writeback_list[rob->cluster]);
    }
  }

  stop_timer(cttransfer);

  return 0;
}

//
// Writeback at most WRITEBACK_WIDTH ROBs on rob_ready_to_writeback_list.
//

CycleTimer ctwriteback;

W64 writeback_width_histogram[MAX_CLUSTERS][WRITEBACK_WIDTH+1];
W64 writeback_total;

W64 writeback_transient;
W64 writeback_persistent;

int writeback(int cluster) {
  int writecount = 0;
  int wakeupcount = 0;
  ReorderBufferEntry* rob;

  start_timer(ctwriteback);

  foreach_list_mutable(rob_ready_to_writeback_list[cluster], rob, entry, nextentry) {
    if (writecount >= WRITEBACK_WIDTH)
      break;

    //
    // Gather statistics
    //
    bool transient = 0;

#ifdef ENABLE_TRANSIENT_VALUE_TRACKING
    if (!isclass(rob->uop.opcode, OPCLASS_STORE|OPCLASS_BRANCH)) {
      transient =
        (rob->dest_renamed_before_writeback) &&
        (rob->consumer_count <= 1) &&
        (rob->physreg->all_consumers_sourced_from_bypass) &&
        (rob->no_branches_between_renamings);

      writeback_transient += transient;
      writeback_persistent += (!transient);
    }

    rob->transient = transient;
#endif

    if (logable(1)) {
      stringbuf rdstr;
      print_value_and_flags(rdstr, rob->physreg->data, rob->physreg->flags);
      logfile << intstring(rob->uop.uuid, 20), " write  rob ", intstring(rob->index(), -3), " (", clusters[rob->cluster].name, ") r", intstring(rob->physreg->index(), -3), " = ", rdstr;
#ifdef ENABLE_TRANSIENT_VALUE_TRACKING
      if (!isclass(rob->uop.opcode, OPCLASS_STORE|OPCLASS_BRANCH)) {
        if (transient) logfile << " (transient)";
        logfile << " (", rob->consumer_count, " consumers";
        if (rob->physreg->all_consumers_sourced_from_bypass) logfile << ", all from bypass";
        if (rob->no_branches_between_renamings) logfile << ", no intervening branches";
        if (rob->dest_renamed_before_writeback) logfile << ", dest renamed before writeback";
        logfile << ")";
      }
#endif
      logfile << endl;
    }

    //
    // Catch corner case where dependent uop was scheduled
    // while producer waited in ready_to_writeback state:
    //
    wakeupcount += rob->forward();

    writecount++;

    //
    // For simulation purposes, final value is already in rob->physreg,
    // so we don't need to actually write anything back here.
    //

    rob->physreg->writeback();
    rob->cycles_left = -1;

    rob->changestate(rob_ready_to_commit_queue);

    writeback_total++;
  }

  writeback_width_histogram[cluster][writecount]++;

  stop_timer(ctwriteback);

  return writecount;
}

//
// Commit at most COMMIT_WIDTH ready to commit instructions from ROB queue,
// and commits any stores by writing to the L1 cache with write through.
// Returns:
//    -1 if we are supposed to abort the simulation
//  >= 0 for the number of instructions actually committed
//

enum {
  COMMIT_RESULT_NONE = 0,
  COMMIT_RESULT_OK = 1,
  COMMIT_RESULT_EXCEPTION = 2,
  COMMIT_RESULT_BARRIER = 3,
  COMMIT_RESULT_STOP = 4
};

W64 total_uops_committed = 0;

// See notes in handle_exception():
W64 chk_recovery_rip;

// totals 100%:
W64 commit_width_histogram[COMMIT_WIDTH+1];

// totals 100%:
W64 commit_freereg_pending;
W64 commit_freereg_free;

// totals 100%:
W64 commit_freereg_recycled;

// totals 100%:
W64 commit_result_none;
W64 commit_result_ok;
W64 commit_result_exception;
W64 commit_result_exception_skipblock;
W64 commit_result_barrier;
W64 commit_result_stop;

// totals 100%:
W64 commit_flags_set;
W64 commit_flags_unset;

// totals 100%:
W64 commit_opclass_histogram[OPCLASS_COUNT];

int ReorderBufferEntry::pseudocommit() {
  if (logable(1)) {
    logfile << intstring(uop.uuid, 20), " pseucm rob ", intstring(index(), -3), ":";
  }

  if (logable(1)) logfile << " rebuild rrt:";

  if (archdest_can_commit[uop.rd]) {
    specrrt[uop.rd]->unspecref(uop.rd);
    specrrt[uop.rd] = physreg;
    specrrt[uop.rd]->addspecref(uop.rd);
    if (logable(1)) logfile << " ", arch_reg_names[uop.rd];
  }

  if (!uop.nouserflags) {
    if (uop.setflags & SETFLAG_ZF) {
      if (logable(1)) logfile << " zf";
      specrrt[REG_zf]->unspecref(REG_zf);
      specrrt[REG_zf] = physreg;
      specrrt[REG_zf]->addspecref(REG_zf);
    }
    if (uop.setflags & SETFLAG_CF) {
      if (logable(1)) logfile << " cf";
      specrrt[REG_cf]->unspecref(REG_cf);
      specrrt[REG_cf] = physreg;
      specrrt[REG_cf]->addspecref(REG_cf);
    }
    if (uop.setflags & SETFLAG_OF) {
      if (logable(1)) logfile << " of";
      specrrt[REG_of]->unspecref(REG_of);
      specrrt[REG_of] = physreg;
      specrrt[REG_of]->addspecref(REG_of);
    }
  }

  if (logable(1)) logfile << " = r", physreg->index();

  if (logable(1)) {
    if (uop.eom) logfile << " [EOM #", total_user_insns_committed, "]";
    logfile << endl;
  }

  if (isclass(uop.opcode, OPCLASS_BARRIER))
    return COMMIT_RESULT_BARRIER;

  return COMMIT_RESULT_OK;
}

int ReorderBufferEntry::commit() {
  static int bytes_in_current_insn_to_commit;

  bool all_ready_to_commit = true;
  bool macro_op_has_exceptions = false;

  if (uop.som) bytes_in_current_insn_to_commit = uop.bytes;

  //
  // Each x86 instruction may be composed of multiple uops; none of the uops
  // may commit until ALL uops are ready to commit (either correctly or
  // if one or more uops have exceptions). 
  //
  // This is accomplished by checking if the uop at the head of the ROB (next
  // to commit) has its SOM (start of macro-op) bit set. If so, the ROB is 
  // scanned forwards from the SOM uop to the EOM (end of macro-op) uop. If
  // all uops in this range are ready to commit and are exception-free, the
  // SOM uop allowed to commit. 
  //
  // Any exceptions in the macro-op uop range immediately signals an exception
  // to the user code, and no part of the uop is committed. In any case,
  // asynchronous interrupts are only taken after committing or excepting the
  // EOM uop in a macro-op.
  //
  
  foreach_forward_from(ROB, this, j) {
    ReorderBufferEntry& subrob = ROB[j];

    if (!subrob.ready_to_commit()) {
      all_ready_to_commit = false;
      break;
    }

    if (subrob.has_exception()) {
      ctx.exception = subrob.physreg->data;
      macro_op_has_exceptions = true;
      break;
    }
    
    if (subrob.uop.eom) break;
  }

  if (!all_ready_to_commit) {
    commit_result_none++;
    return COMMIT_RESULT_NONE;
  }

  assert(ready_to_commit());

  PhysicalRegister* oldphysreg = commitrrt[uop.rd];

  //
  // Update architectural state
  //

  bool ld = isload(uop.opcode);
  bool st = isstore(uop.opcode);

  commit_opclass_histogram[opclassof(uop.opcode)]++;

  if (macro_op_has_exceptions) {
    if (logable(1)) logfile << intstring(uop.uuid, 20), " except rob ", intstring(index(), -3), " exception ", exception_name(ctx.exception), " [EOM #", total_user_insns_committed, "]", endl;

    // See notes in handle_exception():
    if ((uop.opcode == OP_chk) & (ctx.exception == EXCEPTION_SkipBlock)) {
      chk_recovery_rip = ctx.commitarf[REG_rip] + bytes_in_current_insn_to_commit;
      logfile << "SkipBlock exception commit: advancing rip ", (void*)ctx.commitarf[REG_rip], " by ", bytes_in_current_insn_to_commit, " bytes to ", 
        (void*)chk_recovery_rip, endl;
      commit_result_exception_skipblock++;
    } else {
      commit_result_exception++;
    }

    total_uops_committed++;
    total_user_insns_committed++;

    return COMMIT_RESULT_EXCEPTION;
  }
  
  if (st) assert(lsq->addrvalid && lsq->datavalid);

  W64 result = physreg->data;

  if (logable(1)) {
    stringbuf rdstr; print_value_and_flags(rdstr, physreg->data, physreg->flags);
    logfile << intstring(uop.uuid, 20), " commit rob ", intstring(index(), -3);
  }

  if (archdest_can_commit[uop.rd]) {
    commitrrt[uop.rd]->uncommitref(uop.rd);
    commitrrt[uop.rd] = physreg;
    commitrrt[uop.rd]->addcommitref(uop.rd);
    if (logable(1)) {
      logfile << " [rrt ", arch_reg_names[uop.rd], " = r", physreg->index(), " 0x", hexstring(physreg->data, 64), "]";
    }
  }

  if (uop.eom) {
    if (uop.rd == REG_rip) {
      ctx.commitarf[REG_rip] = physreg->data;
    } else {
      ctx.commitarf[REG_rip] += bytes_in_current_insn_to_commit;
    }
    if (logable(1)) logfile << " [rip = ", (void*)ctx.commitarf[REG_rip], "]";
  }

  if (!uop.nouserflags) {
    W64 flagmask = setflags_to_x86_flags[uop.setflags];
    ctx.commitarf[REG_flags] = (ctx.commitarf[REG_flags] & ~flagmask) | (physreg->flags & flagmask);

    commit_flags_set += (uop.setflags != 0);
    commit_flags_unset += (uop.setflags == 0);

    if (logable(1)) { 
      if (uop.setflags) {
        logfile << " [flags ", ((uop.setflags & SETFLAG_ZF) ? "z" : ""), 
          ((uop.setflags & SETFLAG_CF) ? "c" : ""), ((uop.setflags & SETFLAG_OF) ? "o" : ""),
          " = ", flagstring(ctx.commitarf[REG_flags]), "]";
      }
    }

    if (uop.setflags & SETFLAG_ZF) {
      commitrrt[REG_zf]->uncommitref(REG_zf);
      commitrrt[REG_zf] = physreg;
      commitrrt[REG_zf]->addcommitref(REG_zf);
    }
    if (uop.setflags & SETFLAG_CF) {
      commitrrt[REG_cf]->uncommitref(REG_cf);
      commitrrt[REG_cf] = physreg;
      commitrrt[REG_cf]->addcommitref(REG_cf);
    }
    if (uop.setflags & SETFLAG_OF) {
      commitrrt[REG_of]->uncommitref(REG_of);
      commitrrt[REG_of] = physreg;
      commitrrt[REG_of]->addcommitref(REG_of);
    }
  }
    
  if (st) {
    assert(commitstore_unlocked(*lsq) == 0);
    if (logable(1)) logfile << " [mem ", (void*)(lsq->physaddr << 3), " = ", bytemaskstring((const byte*)&lsq->data, lsq->bytemask, 8), "]";
  }

  //
  // Free physical registers, load/store queue entries, etc.
  //
  if (ld|st) {
    if (logable(1)) logfile << " [lsq ", lsq->index(), "]";
    LSQ.commit(lsq);
  }

  assert(archdest_can_commit[uop.rd]);
  assert(oldphysreg->current_state_list == &physreg_arch_list);

  if (oldphysreg->nonnull()) {
    if (logable(1)) {
      if (oldphysreg->referenced()) {
        logfile << " [pending free old r", oldphysreg->index(), " ref by";
        logfile << " refcount ", oldphysreg->refcount;
        logfile << "]";
      } else {
        logfile << " [free old r", oldphysreg->index(), "]";
      }
    }
    if (oldphysreg->referenced()) {
      oldphysreg->changestate(physreg_pendingfree_list); 
      commit_freereg_pending++;
    } else  {
      oldphysreg->free();
      commit_freereg_free++;
    }
  }
  if (logable(1)) logfile << " [commit r", physreg->index(), "]";

  physreg->changestate(physreg_arch_list);

  //
  // Unlock operand physregs since we no longer need to worry about speculation recovery
  // Technically this can be done after the issue queue entry is released, but we do it
  // here for simplicity.
  //
  foreach (i, MAX_OPERANDS) { 
    if ((logable(1)) & (operands[i]->nonnull())) logfile << " [unref r", operands[i]->index(), "]";
    operands[i]->unref(*this);
  }

  //
  // Update branch prediction
  //
  if (isclass(uop.opcode, OPCLASS_BRANCH)) {
    assert(uop.eom);
    //
    // NOTE: Technically the "branch address" refers to the rip of the *next* 
    // x86 instruction after the branch; we use this consistently since x86
    // instructions vary in length and we cannot easily calculate the next
    // instruction in sequence from within the branch predictor logic.
    //
    W64 end_of_branch_x86_insn = uop.rip + bytes_in_current_insn_to_commit;
    bool taken = (ctx.commitarf[REG_rip] != end_of_branch_x86_insn);
    bool predtaken = (uop.riptaken != end_of_branch_x86_insn);

    if (logable(1)) logfile << " [brupdate", (taken ? " tk" : " nt"), (predtaken ? " pt" : " np"), ((taken == predtaken) ? " ok" : " MP"), "]";

    branchpred.update(uop.predinfo, end_of_branch_x86_insn, ctx.commitarf[REG_rip], taken, predtaken, (taken == predtaken));
    branchpred_updates++;
  }

  if (logable(1)) {
    if (uop.eom) logfile << " [EOM #", total_user_insns_committed, "]";
    logfile << endl;
  }

  if (uop.eom)
    total_user_insns_committed++;

  total_uops_committed++;

  changestate(rob_free_list);
  ROB.commit(*this);

  if (isclass(uop.opcode, OPCLASS_BARRIER)) {
    if (logable(1)) logfile << intstring(uop.uuid, 20), " commit barrier: jump to microcode address ", hexstring(uop.riptaken, 48), endl;
    commit_result_barrier++;
    return COMMIT_RESULT_BARRIER;
  }

  commit_result_ok++;
  return COMMIT_RESULT_OK;
}

W64 last_commit_at_cycle = 0;

CycleTimer ctcommit;

int commit() {
  //
  // See notes above on Physical Register Recycling Complications
  //
  start_timer(ctcommit);

  {
    PhysicalRegister* physreg;
    foreach_list_mutable(physreg_pendingfree_list, physreg, entry, nextentry) {
      if (!physreg->referenced()) {
        if (logable(1)) logfile << padstring("", 20), " free   r", physreg->index(), " no longer referenced; moving to free state", endl;
        physreg->free();
        commit_freereg_recycled++;
      }
    }
  }

  //
  // Commit ROB entries *in program order*, stopping at the first ROB that is 
  // not ready to commit or has an exception.
  //
  int commitcount = 0;

  int rc = COMMIT_RESULT_OK;

  foreach_forward(ROB, i) {
    ReorderBufferEntry& rob = ROB[i];

    if (commitcount >= COMMIT_WIDTH) break;
    rc = rob.commit();
    if (rc == COMMIT_RESULT_OK) {
      commitcount++;
      last_commit_at_cycle = sim_cycle;
      if (total_user_insns_committed >= stop_at_user_insns) {
        stop_timer(ctcommit);
        rc = COMMIT_RESULT_STOP;
        break;
      }
    } else {
      break;
    }
  }

  commit_width_histogram[commitcount]++;

  stop_timer(ctcommit);
  return rc;
}

//
// Total simulation time, excluding syscalls on behalf of user program,
// logging activity and other non-simulation operations:
//
CycleTimer cttotal;

//
// Barriers must flush the fetchq and stall the frontend until
// after the barrier is consumed. Execution resumes at the address
// in internal register sr1 (rip after the instruction) after
// handling the barrier in microcode.
//
bool handle_barrier() {
  cttotal.stop();

  branchpred.flush();

  core_to_external_state();
  assist_func_t assist = (assist_func_t)ctx.commitarf[REG_rip];

  const char* assist_name = "unknown";

  foreach (i, ASSIST_COUNT) {
    if (assistid_to_func[i] == assist) { 
      assist_name = assist_names[i];
      break;
    }
  }

  logfile << "Barrier (", (void*)assist, " ", assist_name, " called from ", (void*)ctx.commitarf[REG_sr1], ") at ", sim_cycle, " cycles, ", total_user_insns_committed, " commits", endl, flush;

  if (logable(1)) logfile << "Calling assist function at ", (void*)assist, "...", endl, flush; 

  assist();
  ctx.commitarf[REG_rip] = ctx.commitarf[REG_sr1];
  if (logable(1)) {
    logfile << "Done with assist", endl;
    logfile << "New state:", endl;
    logfile << ctx.commitarf;
  }

  flush_pipeline(ctx.commitarf[REG_sr1]);
  external_to_core_state();

  cttotal.start();

  if (requested_switch_to_native) {
    logfile << "PTL call requested switch to native mode at rip ", (void*)ctx.commitarf[REG_rip], endl;
    return false;
  }

  return true;
}

bool handle_exception() {
  cttotal.stop();

  branchpred.flush();

  core_to_external_state();

  if (logable(1)) 
    logfile << "Exception (", exception_name(ctx.exception), " called from ", (void*)ctx.commitarf[REG_rip], 
      ") at ", sim_cycle, " cycles, ", total_user_insns_committed, " commits", endl, flush;

  //
  // CheckFailed and SkipBlock exceptions are raised by the chk uop.
  // This uop is used at the start of microcoded instructions to assert
  // that certain conditions are true so complex corrective actions can
  // be taken if the check fails.
  //
  // SkipBlock is a special case used for checks at the top of REP loops.
  // Specifically, if the %rcx register is zero on entry to the REP, no
  // action at all is to be taken; the rip should simply advance to
  // whatever is in chk_recovery_rip and execution should resume.
  //
  // CheckFailed exceptions usually indicate the processor needs to take
  // evasive action to avoid a user visible exception. For instance, 
  // CheckFailed is raised when an inlined floating point operand is
  // denormal or otherwise cannot be handled by inlined fastpath uops,
  // or when some unexpected segmentation or page table conditions
  // arise.
  //
  if (ctx.exception == EXCEPTION_SkipBlock) {
    ctx.commitarf[REG_rip] = chk_recovery_rip;
    logfile << "SkipBlock pseudo-exception: skipping to ", (void*)ctx.commitarf[REG_rip], endl, flush;
    flush_pipeline(ctx.commitarf[REG_rip]);
    external_to_core_state();
    cttotal.start();
    return true;
  }

  stringbuf sb;
  sb << exception_name(ctx.exception), " detected at fault rip ", (void*)ctx.commitarf[REG_rip], " @ ", total_user_insns_committed, " commits: genuine user exception (", exception_name(ctx.exception), "); aborting";
  logfile << sb, endl, flush;
  cerr << sb, endl, flush;
  logfile << flush;

  dump_ooo_state();
  logfile << "Aborting...", endl, flush;
  abort();

  return false;
}

void dump_ooo_state() {
  print_rename_tables(logfile);
  print_rob(logfile);
  print_list_of_state_lists<PhysicalRegister>(logfile, physreg_states, "Physical register states");
  print_list_of_state_lists<ReorderBufferEntry>(logfile, rob_states, "ROB entry states");
  print_lsq(logfile);
  logfile << "Issue Queues:", endl;
  foreach_issueq(print(logfile));
  logfile << "Physical Register File:", endl;
  logfile << physregs;
  logfile << flush;
}

void dcache_save_stats(DataStoreNode& ds);

DataStoreNode& add_histogram(DataStoreNode& ds, W64* values, int count) {
  ds.summable = 1;
  foreach (i, count) {
    stringbuf sb; sb << i;
    if (values[i]) ds.add(sb, values[i]);
  }
  return ds;
}

DataStoreNode& add_histogram(DataStoreNode& ds, const char** names, W64* values, int count) {
  ds.summable = 1;
  foreach (i, count) {
    if (values[i]) ds.add(names[i], values[i]);
  }
  return ds;
}

void ooo_capture_stats(DataStoreNode& root) {

  DataStoreNode& summary = root("summary"); {
    summary.add("cycles", sim_cycle);
    summary.add("commits", total_uops_committed);
    summary.add("usercommits", total_user_insns_committed);
    summary.add("issues", issue_total_uops);
    DataStoreNode& ipc = summary("ipc"); {
      ipc.addfloat("commit-in-uops", (double)total_uops_committed / (double)sim_cycle);
      ipc.addfloat("commit-in-user-insns", (double)total_user_insns_committed / (double)sim_cycle);
      ipc.addfloat("issue-in-uops", (double)issue_total_uops / (double)sim_cycle);
    }
  }

  DataStoreNode& simulator = root("simulator"); {
    DataStoreNode& cycles = simulator("cycles"); {
      cycles.summable = 1;
      cycles.addfloat("translate", cttrans.seconds());
      cycles.addfloat("fetch", ctfetch.seconds());
      cycles.addfloat("frontend", ctfrontend.seconds());
      cycles.addfloat("rename", ctrename.seconds());
      cycles.addfloat("dispatch", ctdispatch.seconds());
      
      cycles.addfloat("issue", ctissue.seconds());
      DataStoreNode& issue = cycles("issue"); {
        issue.summable = 1;
        issue.addfloat("store", ctstore.seconds());
        issue.addfloat("load", ctload.seconds());
        issue.addfloat("annul", ctannul.seconds());
      }

      cycles.addfloat("exec", ctsubexec.seconds());
      cycles.addfloat("exec2", ctsubexec2.seconds());

      cycles.addfloat("complete", ctcomplete.seconds());
      cycles.addfloat("transfer", cttransfer.seconds());
      cycles.addfloat("writeback", ctwriteback.seconds());
      cycles.addfloat("commit", ctcommit.seconds());
    }

    DataStoreNode& rate = simulator("rate"); {
      rate.addfloat("total-secs", cttotal.seconds());
      double seconds = cttotal.seconds();
      rate.addfloat("cycles-per-sec", (double)sim_cycle / seconds);
      rate.addfloat("issues-per-sec", (double)issue_total_uops / seconds);
      rate.addfloat("commits-per-sec", (double)total_uops_committed / seconds);
      rate.addfloat("user-commits-per-sec", (double)total_user_insns_committed / seconds);
    }

    DataStoreNode& bbcache = simulator("bbcache"); {
      bbcache.add("count", bbcache.count);
      bbcache.add("inserts", bbcache_inserts);
      bbcache.add("removes", bbcache_removes);
    }
  }

  DataStoreNode& fetch = root("fetch"); {
    add_histogram(fetch("width"), fetch_width_histogram, FETCH_WIDTH+1);

    DataStoreNode& stop = fetch("stop"); {
      stop.summable = 1;
      stop.add("icache-miss", fetch_stop_icache_miss);
      stop.add("fetchq-full", fetch_stop_fetchq_full);
      stop.add("bogus-rip", fetch_stop_bogus_rip);
      stop.add("branch-taken", fetch_stop_branch_taken);
      stop.add("full-width", fetch_stop_full_width);
    };

    fetch.add("blocks", fetch_blocks_fetched);
    fetch.add("uops", fetch_uops_fetched);
    fetch.add("user-insns", fetch_user_insns_fetched);

    add_histogram(fetch("opclass"), opclass_names, fetch_opclass_histogram, OPCLASS_COUNT);
  }

  DataStoreNode& frontend = root("frontend"); {
    DataStoreNode& status = frontend("status"); {
      status.summable = 1;
      status.add("complete", frontend_status_complete);
      status.add("fetchq-empty", frontend_status_fetchq_empty);
      status.add("rob-full", frontend_status_rob_full);
      status.add("physregs-full", frontend_status_physregs_full);
      status.add("ldq-full", frontend_status_ldq_full);
      status.add("stq-full", frontend_status_stq_full);
    }

    DataStoreNode& renamed = frontend("renamed"); {
      renamed.summable = 1;
      renamed.add("none", frontend_renamed_none);
      renamed.add("reg", frontend_renamed_reg);
      renamed.add("flags", frontend_renamed_flags);
      renamed.add("reg-and-flags", frontend_renamed_reg_and_flags);
    }

    DataStoreNode& alloc = frontend("alloc"); {
      alloc.summable = 1;
      alloc.add("reg", frontend_alloc_reg);
      alloc.add("ldreg", frontend_alloc_ldreg);
      alloc.add("sfr", frontend_alloc_sfr);
      alloc.add("br", frontend_alloc_br);
    }

    add_histogram(frontend("width"), frontend_width_histogram, FRONTEND_WIDTH+1);
  }

  DataStoreNode& dispatch = root("dispatch"); {
    DataStoreNode& source = dispatch("source"); {
      source.summable = 1;
      source.add("waiting", physreg_used_list.dispatch_source_counter);
      source.add("bypass", physreg_ready_list.dispatch_source_counter);
      source.add("physreg", physreg_written_list.dispatch_source_counter);
      source.add("archreg", physreg_arch_list.dispatch_source_counter);
    }

    DataStoreNode& cluster = dispatch("cluster"); {
      cluster.summable = 1;
      foreach (i, MAX_CLUSTERS) {
        cluster.add(clusters[i].name, dispatch_cluster_histogram[i]);
      }

      cluster.add("none", dispatch_cluster_none_avail);
    }
  }

  DataStoreNode& issue = root("issue"); {
    DataStoreNode& result = issue("result"); {
      result.summable = 1;
      result.add("no-fu", issue_result_no_fu);
      result.add("replay", issue_result_replay);
      result.add("misspeculation", issue_result_misspeculation);
      result.add("branch-mispredict", issue_result_branch_mispredict);
      result.add("exception", issue_result_exception);
      result.add("complete", issue_result_complete);
    }

    DataStoreNode& source = issue("source"); {
      source.summable = 1;
      source.add("bypass", physreg_ready_list.issue_source_counter);
      source.add("physreg", physreg_written_list.issue_source_counter);
      source.add("archreg", physreg_arch_list.issue_source_counter);
    }

    DataStoreNode& cluster = issue("width"); {
      foreach (i, MAX_CLUSTERS) {
        add_histogram(cluster(clusters[i].name), issue_width_histogram[i], MAX_ISSUE_WIDTH+1);
      }
    }

    add_histogram(issue("opclass"), opclass_names, issue_opclass_histogram, OPCLASS_COUNT);
  }

  DataStoreNode& writeback = root("writeback"); {
    writeback.add("total", writeback_total);

    DataStoreNode& transient = writeback("transient"); {
      transient.summable = 1;
      transient.add("transient", writeback_transient);
      transient.add("persistent", writeback_persistent);
    }

    DataStoreNode& cluster = writeback("width"); {
      foreach (i, MAX_CLUSTERS) {
        add_histogram(cluster(clusters[i].name), writeback_width_histogram[i], WRITEBACK_WIDTH+1);
      }
    }
  }

  DataStoreNode& commit = root("commit"); {
    commit.add("uops", total_uops_committed);
    commit.add("userinsns", total_user_insns_committed);

    DataStoreNode& freereg = commit("freereg"); {
      freereg.summable = 1;
      freereg.add("pending", commit_freereg_pending);
      freereg.add("free", commit_freereg_free);
    }

    commit.add("physreg-recycled", commit_freereg_recycled);

    DataStoreNode& result = commit("result"); {
      result.summable = 1;
      result.add("none", commit_result_none);
      result.add("ok", commit_result_ok);
      result.add("exception", commit_result_exception);
      result.add("skipblock", commit_result_exception_skipblock);
      result.add("barrier", commit_result_barrier);
      result.add("stop", commit_result_stop);
    }

    DataStoreNode& setflags = commit("setflags"); {
      setflags.summable = 1;
      setflags.add("yes", commit_flags_set);
      setflags.add("no", commit_flags_unset);
    }

    add_histogram(commit("width"), commit_width_histogram, COMMIT_WIDTH+1);

    add_histogram(commit("opclass"), opclass_names, commit_opclass_histogram, OPCLASS_COUNT);
  }

  DataStoreNode& branchpred = root("branchpred"); {
    DataStoreNode& cond = branchpred("cond"); {
      cond.summable = 1;
      cond.add("correct", branchpred_cond_correct);
      cond.add("mispred", branchpred_cond_mispred);
    }
    DataStoreNode& indir = branchpred("indirect"); {
      indir.summable = 1;
      indir.add("correct", branchpred_indir_correct);
      indir.add("mispred", branchpred_indir_mispred);
    }
    DataStoreNode& ret = branchpred("return"); {
      ret.summable = 1;
      ret.add("correct", branchpred_return_correct);
      ret.add("mispred", branchpred_return_mispred);
    }
    DataStoreNode& ras = branchpred("ras"); {
      ras.summable = 1;
      ras.add("push", branchpred_ras_pushes);
      ras.add("push-overflow", branchpred_ras_overflows);
      ras.add("pop", branchpred_ras_pops);
      ras.add("pop-underflows", branchpred_ras_underflows);
      ras.add("annuls", branchpred_ras_annuls);
    }
    DataStoreNode& summary = branchpred("summary"); {
      summary.summable = 1;
      summary.add("correct", branchpred_total_correct);
      summary.add("mispred", branchpred_total_mispred);
    }
    branchpred.add("predictions", branchpred_predictions);
    branchpred.add("updates", branchpred_updates);
  }

  dcache_save_stats(root("dcache"));

}

void ooo_capture_stats() {
  cttotal.stop();

  if (!dsroot)
    return;

  stringbuf sb;
  sb << snapshotid;
  snapshotid++;

  ooo_capture_stats((*dsroot)(sb));

  cttotal.start();
}

//
// Validate the physical register reference counters against what
// is really accessible from the various tables and operand fields.
//
// This is for debugging only.
//
void check_refcounts() {
  int refcounts[PHYS_REG_FILE_SIZE];
  memset(refcounts, 0, sizeof(refcounts));

  foreach_forward(ROB, i) {
    ReorderBufferEntry& rob = ROB[i];
    foreach (j, MAX_OPERANDS) {
      refcounts[rob.operands[j]->index()]++;
    }
  }

  foreach (i, TRANSREG_COUNT) {
    refcounts[commitrrt[i]->index()]++;
    refcounts[specrrt[i]->index()]++;
  }

  refcounts[PHYS_REG_NULL] = 0;

  bool errors = 0;

  foreach (i, PHYS_REG_FILE_SIZE) {
    if (physregs[i].refcount != refcounts[i]) {
      logfile << "ERROR: r", i, " refcount is ", physregs[i].refcount, " but should be ", refcounts[i], endl;

      foreach_forward(ROB, r) {
        ReorderBufferEntry& rob = ROB[r];
        foreach (j, MAX_OPERANDS) {
          if (rob.operands[j]->index() == i) logfile << "  ROB ", r, " operand ", j, endl;
        }
      }

      foreach (j, TRANSREG_COUNT) {
        if (commitrrt[j]->index() == i) logfile << "  CommitRRT ", arch_reg_names[j], endl;
        if (specrrt[j]->index() == i) logfile << "  SpecRRT ", arch_reg_names[j], endl;
      }

      errors = 1;
    }
  }

  if (errors) assert(false);
}

void out_of_order_core_toplevel_loop() {
  init_luts();

  logfile << "Starting out-of-order core toplevel loop", endl, flush;

  // Make sure the translator splits up unaligned loads and stores during decoding:
  split_unaligned_memops_during_translate = true;

  wakeup_func = load_filled_callback;
  icache_wakeup_func = icache_filled_callback;

  flush_pipeline(ctx.commitarf[REG_rip]);
  external_to_core_state();
  logfile << "Core State:", endl;
  logfile << ctx.commitarf;

  int oldloglevel = loglevel;
  if (start_log_at_iteration != MAX_CYCLE) loglevel = 0;

  cttotal.start();

  while ((iterations < stop_at_iteration) & (total_user_insns_committed < stop_at_user_insns)) {
    if ((iterations >= start_log_at_iteration) & (!loglevel)) {
      loglevel = oldloglevel;
      logfile << "Start logging (level ", loglevel, ") at cycle ", sim_cycle, endl, flush;
    }
    if (logable(1)) logfile << "Cycle ", sim_cycle, ":", endl;

    if ((sim_cycle - last_commit_at_cycle) > 1024) {
      logfile << "WARNING: At cycle ", sim_cycle, ", ", total_user_insns_committed, " user commits: no instructions have committed for ", (sim_cycle - last_commit_at_cycle), " cycles; the pipeline could be deadlocked", endl, flush;
      cerr << "WARNING: At cycle ", sim_cycle, ", ", total_user_insns_committed, " user commits: no instructions have committed for ", (sim_cycle - last_commit_at_cycle), " cycles; the pipeline could be deadlocked", endl, flush;
      break;
    }

    if (lowbits(sim_cycle, 16) == 0) 
      logfile << "Completed ", sim_cycle, " cycles, ", total_user_insns_committed, " commits (rip sample ", (void*)ctx.commitarf[REG_rip], ")", endl, flush;

    // All FUs are available at top of cycle:
    fu_avail = bitmask(FU_COUNT);

    dcache_clock();

    int commitrc = commit();

    for_each_cluster(i) { writeback(i); }
    for_each_cluster(i) { transfer(i); }

    for_each_cluster(i) { issue(i); complete(i); }

    dispatch();

    if (!stall_frontend) {
      frontend();
      rename();
      fetch();
    }

    foreach_issueq(clock());

#ifdef ENABLE_CHECKS
    check_refcounts();
#endif

    if (commitrc == COMMIT_RESULT_BARRIER) {
      if (!handle_barrier()) break;
    } else if (commitrc == COMMIT_RESULT_EXCEPTION) {
      if (!handle_exception()) break;
    } else if (commitrc == COMMIT_RESULT_STOP) {
      break;
    }

    iterations++;
    sim_cycle++;
  }

  cttotal.stop();

  dump_ooo_state();

  core_to_external_state();
  logfile << "Core State:", endl;
  logfile << ctx.commitarf;
  logfile << flush;
}