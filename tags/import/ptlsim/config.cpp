//
// PTLsim: Cycle Accurate x86-64 Simulator
// Configuration Management
//
// Copyright 2000-2005 Matt T. Yourst <yourst@yourst.com>
//

#include <ptlsim.h>
#include <datastore.h>

class CommandLineOptions {
public:
  char* option;
  int type;
  bool ishex;
  char* description;
  void* variable;
};

enum {
  OPTION_TYPE_NONE    = 0, 
  OPTION_TYPE_W64     = 1,
  OPTION_TYPE_FLOAT   = 2,
  OPTION_TYPE_STRING  = 3,
  OPTION_TYPE_TRAILER = 4,
  OPTION_TYPE_BOOL    = 5,
  OPTION_TYPE_SECTION = -1
};

ostream logfile;
W64 sim_cycle = 0;
W64 user_insn_commits = 0;
W64 iterations = 0;
W64 total_uops_executed = 0;
W64 total_user_insns_committed = 0;

W64 nobanner = 0;
W64 loglevel = 0;
char* log_filename = null;
W64 include_dyn_linker = 1;
W64 start_at_rip = 0;
W64 start_at_rip_repeat = 1;
W64 stop_at_iteration = MAX_CYCLE;
W64 insns_in_last_basic_block = 65536;
W64 stop_at_rip = 0;
W64 stop_at_user_insns = MAX_CYCLE;
W64 force_seq_at_iteration = MAX_CYCLE;
W64 all_sequential = 0;
W64 start_log_at_iteration = MAX_CYCLE;
W64 start_short_log_at_iteration = MAX_CYCLE;
W64 user_profile_only = 0;
W64 trigger_mode = 0;
W64 exit_after_fullsim;
char* stats_filename = null;
char* dumpcode_filename = null;
W64 perfect_cache = 0;
W64 snapshot_cycles = MAX_CYCLE;
W64 flush_interval = MAX_CYCLE;


W64 use_out_of_order_core = 1;
W64 use_out_of_order_core_dummy;


DataStoreNode* dsroot = null;
W64 snapshotid;


CommandLineOptions options[] = {
  {null,                                 OPTION_TYPE_SECTION, 0, "Logging Control", null},
  {"quiet",                              OPTION_TYPE_BOOL,    0, "Do not print PTLsim system information banner", &nobanner},
  {"logfile",                            OPTION_TYPE_STRING,  0, "Log filename (use /dev/fd/1 for stdout, /dev/fd/2 for stderr)", &log_filename},
  {"loglevel",                           OPTION_TYPE_W64,     0, "Log level", &loglevel},
  {"startlog",                           OPTION_TYPE_W64,     0, "Start logging after iteration <startlog>", &start_log_at_iteration},
  {"shortlog",                           OPTION_TYPE_W64,     0, "Start summary logging after iteration <shortlog>", &start_short_log_at_iteration},

  {null,                                 OPTION_TYPE_SECTION, 0, "Statistics Database", null},
  {"stats",                              OPTION_TYPE_STRING,  0, "Statistics data store hierarchy root", &stats_filename},
  {"snapshot",                           OPTION_TYPE_W64,     0, "Take statistical snapshot and reset every <snapshot> cycles", &snapshot_cycles},

  {null,                                 OPTION_TYPE_SECTION, 0, "Trace Start Point", null},
  {"startrip",                           OPTION_TYPE_W64,     0, "Start at rip <startrip>", &start_at_rip},
  {"startrepeat",                        OPTION_TYPE_W64,     0, "Start only after passing <startrip> at least <startrepeat> times", &start_at_rip_repeat},
  {"excludeld",                          OPTION_TYPE_BOOL,    0, "Exclude dynamic linker execution", &include_dyn_linker},
  {"trigger",                            OPTION_TYPE_BOOL,    0, "Trigger mode: wait for user process to do simcall before entering PTL mode", &trigger_mode},

  {null,                                 OPTION_TYPE_SECTION, 0, "Trace Stop Point", null},
  {"stop",                               OPTION_TYPE_W64,     0, "Stop after <stop> iterations", &stop_at_iteration},
  {"stoprip",                            OPTION_TYPE_W64,     0, "Stop before rip <stoprip> is translated for the first time", &stop_at_rip},
  {"bbinsns",                            OPTION_TYPE_W64,     0, "In final basic block, only translate <bbinsns> user instructions", &insns_in_last_basic_block},
  {"stopinsns",                          OPTION_TYPE_W64,     0, "Stop after executing <stopinsns> user instructions", &stop_at_user_insns},
  {"flushevery",                         OPTION_TYPE_W64,     0, "Flush the pipeline every N committed instructions", &flush_interval},

  {null,                                 OPTION_TYPE_SECTION, 0, "Sequential and Native Control", null},
  {"profonly",                           OPTION_TYPE_BOOL,    0, "Profile user code in native mode only; don't simulate anything", &user_profile_only},
  {"forceseq",                           OPTION_TYPE_W64,     0, "Force trace at or after iteration <forceseq> to run sequential version", &force_seq_at_iteration},
  {"allseq",                             OPTION_TYPE_BOOL,    0, "Completely disable out of order execution mode", &all_sequential},
  {"exitend",                            OPTION_TYPE_BOOL,    0, "Kill the thread after full simulation completes rather than going native", &exit_after_fullsim},

  {null,                                 OPTION_TYPE_SECTION, 0, "Debugging", null},
  {"dumpcode",                           OPTION_TYPE_STRING,  0, "Save page of user code at final rip to file <dumpcode>", &dumpcode_filename},
  {"perfect-cache",                      OPTION_TYPE_BOOL,    0, "Perfect cache hit rate", &perfect_cache},

  {"ooo",                                OPTION_TYPE_BOOL,    0, "Use out of order core instead of PT2x core", &use_out_of_order_core_dummy},
};

void print_usage(int argc, char* argv[]) {
  cerr << "Syntax: ptlsim <executable> <arguments...>", endl;
  cerr << "All other options come from file /home/<username>/.ptlsim/path/to/executable", endl, endl;

  cerr << "Options are:", endl;
  foreach (i, lengthof(options)) {
    if (options[i].type == OPTION_TYPE_SECTION) {
      cerr << options[i].description, ":", endl;
      continue;
    }
    cerr << "  -", padstring(options[i].option, -16), " ", options[i].description, " ";
    if (!options[i].variable) {
      cerr << endl;
      continue;
    }
    cerr << "[";
    switch (options[i].type) {
    case OPTION_TYPE_NONE:
      break;
    case OPTION_TYPE_W64:
      cerr << *((W64*)(options[i].variable));
      break;
    case OPTION_TYPE_FLOAT:
      cerr << *((float*)(options[i].variable));
      break;
    case OPTION_TYPE_STRING:
      cerr << *((char**)(options[i].variable));
      break;
    case OPTION_TYPE_BOOL:
      cerr << ((*((W64**)(options[i].variable))) ? "enabled" : "disabled");
      break;
    default:
      assert(false);
    }
    cerr << "]", endl;
  }
  cerr << endl;
}

static char hostname[512] = "localhost";
static char domainname[512] = "domain";

void print_banner(ostream& os, int argc, char* argv[]) {
  gethostname(hostname, sizeof(hostname));
  getdomainname(domainname, sizeof(domainname));

  os << "//  ", endl;
  os << "//  PTLsim: Cycle Accurate x86-64 Simulator", endl;
  os << "//  Copyright 1999-2005 Matt T. Yourst <yourst@yourst.com>", endl;
  os << "// ", endl;
  os << "//  Built ", __DATE__, " ", __TIME__, " on ", stringify(BUILDHOST), " using gcc-", 
    stringify(__GNUC__), ".", stringify(__GNUC_MINOR__), endl;
  os << "//  Running on ", hostname, ".", domainname, " (", (int)math::floor(CycleTimer::gethz() / 1000000.), " MHz)", endl;
  os << "//  ", endl;
  os << "//  Arguments: ";
  foreach (i, argc) {
    os << argv[i];
    if (i != (argc-1)) os << ' ';
  }
  os << endl;
  os << "//  Thread ", getpid(), " is running in ", (ctx.use64 ? "64-bit x86-64" : "32-bit x86"), " mode", endl;
  os << "//  ", endl;
  os << endl;
}

void print_banner(int argc, char* argv[]) {
  print_banner(cerr, argc, argv);
}

int parse_options(int argc, char* argv[], int realargc, char* realargv[]) {
  int i = 1;

  while (i <= argc) {
    if ((argv[i][0] == '-') && strlen(argv[i]) > 1) {
      char* option = &argv[i][1];
      i++;
      bool found = false;
      for (int j = 0; j < lengthof(options); j++) {
        if (options[j].type == OPTION_TYPE_SECTION) continue;
        if (strequal(option, options[j].option)) {
          found = true;
          void* variable = options[j].variable;
          if ((options[j].type != OPTION_TYPE_NONE) && (options[j].type != OPTION_TYPE_BOOL) && (i == (argc+1))) {
            cerr << "Warning: missing value for option '", argv[i-1], "'", endl;
            break;
          }
          switch (options[j].type) {
          case OPTION_TYPE_NONE:
            break;
          case OPTION_TYPE_W64: {
            char* p = argv[i];
            int len = strlen(p);
            W64 multiplier = 1;
            char* endp = p;
            if (!len) {
              cerr << "Warning: option ", argv[i-1], " had no argument; ignoring", endl;
              break;
            }
            if (len > 1) {
              char& c = p[len-1];
              switch (c) {
              case 'k': case 'K':
                multiplier = 1000LL; c = 0; break;
              case 'm': case 'M':
                multiplier = 1000000LL; c = 0; break;
              case 'g': case 'G':
                multiplier = 1000000000LL; c = 0; break;
              case 't': case 'T':
                multiplier = 1000000000000LL; c = 0; break;
              }
            }
            W64 v = strtoll(p, &endp, 0);
            if (endp[0] != 0) {
              cerr << "Warning: invalid value '", p, "' for option ", argv[i-1], "; ignoring", endl;
            }
            v *= multiplier;
            *((W64*)variable) = v;
            i++;
            break;
          }
          case OPTION_TYPE_FLOAT:
            *((float*)variable) = atof(argv[i++]);
            break;
          case OPTION_TYPE_STRING:
            *((char**)variable) = argv[i++];
            break;
          case OPTION_TYPE_BOOL:
            *((W64*)variable) = (!(*((W64*)variable)));
            break;
          default:
            assert(false);
          }
          break;
        }
      }
      if (!found) {
        cerr << "Warning: invalid option '", argv[i++], "'", endl;
      }
    } else {
      // trailing arguments, if any
      goto done;
    }
  }
  done:

  if (log_filename) {
    // Can also use "-logfile /dev/fd/1" to send to stdout (or /dev/fd/2 for stderr):
    logfile.open(log_filename);
  }

  if (!nobanner) print_banner(cerr, realargc, realargv);
  print_banner(logfile, realargc, realargv);

  logfile << "Active parameters:", endl;

  foreach (i, lengthof(options)) {
    if (!options[i].variable)
      continue;
    logfile << "  -", padstring(options[i].option, -12), " ";
    switch (options[i].type) {
    case OPTION_TYPE_NONE:
      break;
    case OPTION_TYPE_W64: {
      W64 v = *((W64*)(options[i].variable));
      if (v == 0) {
        logfile << 0;
      } else if (v == MAX_CYCLE) {
        logfile << "infinity";
      } else if ((v % 1000000000LL) == 0) {
        logfile << (v / 1000000000LL), " G";
      } else if ((v % 1000000LL) == 0) {
        logfile << (v / 1000000LL), " M";
      } else {
        logfile << v;
      }
      break;
    } case OPTION_TYPE_FLOAT:
      logfile << *((float*)(options[i].variable));
      break;
    case OPTION_TYPE_STRING:
      logfile << *((char**)(options[i].variable));
      break;
    case OPTION_TYPE_BOOL:
      logfile << *((W64*)(options[i].variable)) ? "enabled" : "disabled";
      break;
    default:
      assert(false);
    }
    logfile << endl;
  }

  return 0;
}

extern char** initenv;

const char* get_full_exec_filename();

int init_config(int argc, char** argv) {
  char confroot[1024] = "";
  stringbuf sb;

  // Fix up environ to copied environment strings:
  environ = initenv;

  char* homedir = getenv("HOME");

  const char* execname = get_full_exec_filename();

  sb << (homedir ? homedir : "/etc"), "/.ptlsim", execname, ".conf";

  char args[4096];
  istream is(sb);
  if (!is) {
    cerr << "ptlsim: Warning: could not find '", sb, "', using defaults", endl;
  }

  const char* simname = "ptlsim";

  for (;;) {
    is >> readline(args, sizeof(args));
    if (!is) break;
    char* p = args;
    while (*p && (*p != '#')) p++;
    if (*p == '#') *p = 0;
    if (args[0]) break;
  }

  is.close();

  char* ptlargs[1024];

  ptlargs[0] = strdup(simname);
  int ptlargc = 0;
  char* p = args;
  while (*p && (ptlargc < (lengthof(ptlargs)-1))) {
    char* pbase = p;
    while ((*p != 0) && (*p != ' ')) p++;
    ptlargc++;
    ptlargs[ptlargc] = strndup(pbase, p - pbase);
    if (*p == 0) break;
    *p++;
    while ((*p != 0) && (*p == ' ')) p++;
  }

  parse_options(ptlargc, ptlargs, argc, argv);

  if (stats_filename) {
    dsroot = new DataStoreNode("root");
    DataStoreNode& info = (*dsroot)("ptlsim");

    stringbuf sb;

    sb.reset();
    sb << __DATE__, " ", __TIME__;
    info.add("build-timestamp", sb);

    sb.reset();
    sb << stringify(BUILDHOST);
    info.add("build-hostname", sb);

    sb.reset();
    sb << "gcc-", stringify(__GNUC__), ".", stringify(__GNUC_MINOR__);
    info.add("build-compiler-version", sb);

    sb.reset();
    sb << hostname, ".", domainname;
    info.add("hostname", sb);


    info.addfloat("native-mhz", CycleTimer::gethz() / 1000000);

    info.add("executable", execname);

    sb.reset();
    foreach (i, argc) {
      sb << argv[i];
      if (i != (argc-1)) sb << ' ';
    }
    info.add("args", sb);
  }

  snapshotid = 0;

  return 0;
}