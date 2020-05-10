#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef uint32_t m128 __attribute((vector_size (16)));
static_assert( sizeof(m128)*8 == 128, "!" );

typedef uint32_t m256 __attribute((vector_size (32)));
static_assert( sizeof(m256)*8 == 256, "!" );

typedef uint32_t m512 __attribute((vector_size (64)));
static_assert( sizeof(m512)*8 == 512, "!" );

static uint64_t tsc_freq_khz = 3892687; // AMD
static uint64_t
rdtscp( void )
{
  uint32_t hi, lo;
  __asm__ volatile( "rdtscp": "=a"(lo), "=d"(hi) :: "memory", "%ecx" );
  return (uint64_t)lo | ( (uint64_t)hi << 32 );
}

// lie to the compiler indicating that and SSE value was used
// can prevent compiler from eliding a "dead" store
// this works by saying that the thing passed in (var) is an input to this asm inline
#define SSE_USED(var) __asm__ volatile( "# SSE_USED(" #var ")" :: "x"(var) )

// same idea, but this time we say that the value is read from and written to
#define TAINT(var)    __asm__ volatile( "# TAINT(" #var ")" : "+g"(var) : "g"(var) )
#define ARRAY_SIZE(v) (sizeof(v)/sizeof(*v))

#define CACHE_LINE  (64ul)
#define N_CORES     (8ul)

// how many chunks to put in the memory segment
#define N_CHUNKS    (8192ul*1024ul)

// how many cache lines to read/write for each synchronized chunk
#define CHUNK_LINES (8ul)

typedef struct {
                       atomic_bool ready;
  _Alignas(CACHE_LINE) char        space[ 4*CACHE_LINE ];
  _Alignas(CACHE_LINE) m128        data[ (CACHE_LINE*CHUNK_LINES)/sizeof( m128 ) ];
} chunk_t;

typedef struct {
  size_t   core;
  chunk_t* chunks;
} thread_args_t;

void bind( uint64_t id )
{
  cpu_set_t cpu_set;
  CPU_ZERO( &cpu_set );
  CPU_SET( id, &cpu_set );
  int ret = pthread_setaffinity_np( pthread_self(), N_CORES, &cpu_set );
  if( ret != 0 ) {
    fprintf( stderr, "Failed to bind to core\n" );
    abort();
  }
}

void __attribute((noinline))
walk( chunk_t* chunks )
{
  // touch every cache line to bring them all into local cpu's closest
  // caches. doesn't opt out due to char*

  char* bytes = (char*)chunks;
  for( size_t i = 0; i < sizeof( chunk_t )*N_CHUNKS; i += CACHE_LINE ) {
    char tmp = bytes[i];
    TAINT(tmp); // without this, the compiler removes the load/store
    bytes[i] = tmp;
  }

  /* here's what we're getting, which is correct!
	movzbl	(%rdi), %eax	# MEM[base: _11, offset: 0B], tmp
	# TAINT(tmp)
	movb	%al, (%rdi)	# tmp, MEM[base: _11, offset: 0B]
  */
}

void* writer_thread( void* arg )
{
  thread_args_t* args   = arg;
  uint64_t       core   = args->core;
  chunk_t*       chunks = args->chunks;

  bind( core );
  walk( chunks );

  m128**        data_ptrs  = malloc( N_CHUNKS*sizeof( m128* ) );
  atomic_bool** ready_ptrs = malloc( N_CHUNKS*sizeof( atomic_bool* ) );
  for( size_t i = 0; i < N_CHUNKS; ++i ) {
    data_ptrs[i]  = chunks[i].data;
    ready_ptrs[i] = &chunks[i].ready;
  }

  // the value that we will be sending to the other core
  m128 v1 = {1, 2, 3, 4};

  uint64_t start = 0;
  for( size_t i = 0; i < N_CHUNKS; ++i ) {
    // chunk_t*     chunk = &chunks[i];
    m128*        data  = data_ptrs[i];
    atomic_bool* ready = ready_ptrs[i];

    // threads might not start at same time, wait until first chunk to
    // start timing
    if( i == 1 ) start = rdtscp();

    for( size_t j = 0; j < ARRAY_SIZE( chunks[0].data ); ++j ) {
      data[j] = v1;
    }

    // mark the chunk as ready
    // FIXME this has something to do with the slowdown
    asm volatile("#NEXT_LOAD");
    atomic_store_explicit( ready, true, memory_order_release );
  }
  uint64_t end = rdtscp();

  return (void*)(end-start);
}

void* reader_thread( void* arg )
{
  thread_args_t* args   = arg;
  uint64_t       core   = args->core;
  chunk_t*       chunks = args->chunks;

  bind( core );

  m128 v1; // we will load into the register a lot

  uint64_t start = 0;
  for( size_t i = 0; i < N_CHUNKS; ++i ) {
    chunk_t*     chunk = &chunks[i];
    m128*        data  = chunk->data;
    atomic_bool* ready = &chunk->ready;

    // same idea as above, start timing late
    if( i == 1 ) start = rdtscp();

    // wait until the line is ready
    while( !atomic_load_explicit( ready, memory_order_acquire ) ) { }

    // read the entire data section into the sse register, but don't
    // do anything with it.
    // have to convince compiler to leave this code in.

    for( size_t j = 0; j < ARRAY_SIZE( chunk->data ); ++j ) {
      v1 = data[j]; SSE_USED( v1 );
    }
  }
  uint64_t end = rdtscp();

  return (void*)(end-start);
}

static void
run_test( size_t core1, size_t core2 )
{
  bind( core1 ); // allocate on writer numa node

  chunk_t* chunks = NULL;
  posix_memalign( (void**)&chunks, CACHE_LINE, sizeof( chunk_t )*N_CHUNKS );
  memset( chunks, 0, sizeof( chunk_t )*N_CHUNKS );

  thread_args_t writer = (thread_args_t){
    .core = core1,
    .chunks = chunks,
  };

  thread_args_t reader = (thread_args_t){
    .core = core2,
    .chunks = chunks,
  };

  pthread_t threads[2];
  uint64_t  writer_time;
  uint64_t  reader_time;

  pthread_create( &threads[0], NULL, writer_thread, (void*)&writer );
  pthread_create( &threads[1], NULL, reader_thread, (void*)&reader );

  pthread_join( threads[0], (void*)&writer_time );
  pthread_join( threads[1], (void*)&reader_time );

  double ns_per_cycle = 1./((double)(tsc_freq_khz * 1000)/1e9);

  double writer_ns = ns_per_cycle*writer_time;
  double reader_ns = ns_per_cycle*reader_time;

  uint64_t mb = CHUNK_LINES*CACHE_LINE*N_CHUNKS/1024/1024;
  double wr_sec = writer_ns/1e9;
  double rd_sec = reader_ns/1e9;

  printf( "(%zu->%zu) writer %6.2f GiB/s\n", core1, core2, (double)mb/wr_sec/1024 );
  printf( "(%zu->%zu) reader %6.2f GiB/s\n", core1, core2, (double)mb/rd_sec/1024 );

  free( chunks );
}

static void __attribute((noinline))
bw( void ) {
  bind( 0 );

  // do a local cache copy for baseline
  size_t sz = 2ul<<28ul;
  char* bytes_a;
  char* bytes_b;
  posix_memalign( (void**)&bytes_a, CACHE_LINE, sz );
  posix_memalign( (void**)&bytes_b, CACHE_LINE, sz );

  // prefault both
  for( size_t i = 0; i < sz; i += CACHE_LINE ) {
    char tmp = bytes_a[i];
    TAINT(tmp);
    bytes_a[i] = tmp;

    tmp = bytes_b[i];
    TAINT(tmp);
    bytes_b[i] = tmp;
  }

  uint64_t start = rdtscp();
  // memcpy is faster, but the loop is more comparable to what the rest of the
  // code is doing
  memcpy( bytes_a, bytes_b, sz );
  // #define TY m128
  // for( size_t i = 0; i < sz; i += sizeof( TY ) ) {
  //   TY a = *(TY*)&bytes_a[i + sizeof( TY )*0];
  //   *(TY*)&bytes_b[i + sizeof( TY )*0] = a;
  // }
  // #undef TY
  uint64_t end = rdtscp();
  double ns_per_cycle = 1./((double)(tsc_freq_khz * 1000)/1e9);
  double ns = ns_per_cycle*(end-start);
  printf( "simple bw = %f MiB/s\n", (double)(sz/1024/1024)/(ns/1e9) );
  free( bytes_a );
  free( bytes_b );
}

int main()
{
  bw();
  printf( "\nfast tests\n" );
  run_test( 0, 1 );
  run_test( 1, 2 );
  run_test( 2, 3 );
  // 3,4 skip
  run_test( 4, 5 );
  run_test( 5, 6 );
  run_test( 6, 7 );

  printf("\nslow tests\n");
  run_test( 0, 4 );
  run_test( 4, 1 );
  run_test( 1, 5 );
  run_test( 5, 2 );
  run_test( 2, 6 );
  run_test( 6, 3 );
  run_test( 3, 7 );
}

/* AMD Ryzen 7 3800X 8-Core Processor

  L3 L#0 (16MB)
      L2 L#0 (512KB) + L1d L#0 (32KB) + L1i L#0 (32KB) + Core L#0
        PU L#0 (P#0)
        PU L#1 (P#8)
      L2 L#1 (512KB) + L1d L#1 (32KB) + L1i L#1 (32KB) + Core L#1
        PU L#2 (P#1)
        PU L#3 (P#9)
      L2 L#2 (512KB) + L1d L#2 (32KB) + L1i L#2 (32KB) + Core L#2
        PU L#4 (P#2)
        PU L#5 (P#10)
      L2 L#3 (512KB) + L1d L#3 (32KB) + L1i L#3 (32KB) + Core L#3
        PU L#6 (P#3)
        PU L#7 (P#11)
   L3 L#1 (16MB)
      L2 L#4 (512KB) + L1d L#4 (32KB) + L1i L#4 (32KB) + Core L#4
        PU L#8 (P#4)
        PU L#9 (P#12)
      L2 L#5 (512KB) + L1d L#5 (32KB) + L1i L#5 (32KB) + Core L#5
        PU L#10 (P#5)
        PU L#11 (P#13)
      L2 L#6 (512KB) + L1d L#6 (32KB) + L1i L#6 (32KB) + Core L#6
        PU L#12 (P#6)
        PU L#13 (P#14)
      L2 L#7 (512KB) + L1d L#7 (32KB) + L1i L#7 (32KB) + Core L#7
        PU L#14 (P#7)
        PU L#15 (P#15)
*/
