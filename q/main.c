#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <immintrin.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <x86intrin.h>

typedef uint32_t m128 __attribute((vector_size (16)));
static_assert( sizeof(m128)*8 == 128, "!" );

typedef uint32_t m256 __attribute((vector_size (32)));
static_assert( sizeof(m256)*8 == 256, "!" );

static uint64_t tsc_freq_khz = 3892687; // AMD
static inline uint64_t
rdtscp( void )
{
  uint32_t hi, lo;
  __asm__ volatile( "rdtscp": "=a"(lo), "=d"(hi) :: "memory", "%ecx" );
  return (uint64_t)lo | ( (uint64_t)hi << 32 );
}

static double
ticks_to_ns( uint64_t ticks )
{
  return ((double)ticks * 1e6) / (tsc_freq_khz);
}

// lie to the compiler indicating that and SSE value was used
// can prevent compiler from eliding a "dead" store
// this works by saying that the thing passed in (var) is an input to this asm inline
#define SSE_USED(var) __asm__ volatile( "# SSE_USED(" #var ")" :: "x"(var) )

// same idea, but this time we say that the value is read from and written to
#define TAINT(var)    __asm__ volatile( "# TAINT(" #var ")" : "+g"(var) : "g"(var) )

#define ARRAY_SIZE(v) (sizeof(v)/sizeof(*v))

#define CACHE_LINE  (64ul)
#define N_CORES     (16ul)

// how many chunks to put in the memory segment
#define N_CHUNKS    (8192ul)

// how many cache lines to read/write for each synchronized chunk
#define CHUNK_LINES (8ul)

// type to use in chunk
#define CHUNK_ELEMENT_T m128

// compute the number of elements needed to fit in N cache lines
#define N_ELTS ( (CACHE_LINE*CHUNK_LINES)/sizeof( CHUNK_ELEMENT_T ) )

#define TRANSFER_MB ( ( N_CHUNKS*N_ELTS*sizeof( CHUNK_ELEMENT_T ) )/1024/1024 )

// this test is very sensitive to other operations on the system
// run a very large number of trials
#define TRIALS (500)

typedef struct {
  // mark if the data in the chunk is ready
  uint64_t        ready;

  // use up the rest of the cache line that contains ready bit
  // we do not want the ready bit and the data bit to be in the same cache line
  char            pad[ CACHE_LINE - sizeof( uint64_t ) ];

  // actual data, will be the size of one cache line
  CHUNK_ELEMENT_T data[ N_ELTS ];
} chunk_t;

static void __attribute__((noinline))
bind( uint64_t id )
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

typedef struct {
  size_t       core;
  uint32_t*    wtr_ready;
  uint32_t*    rdr_ready;
  chunk_t*     chunks;
  double*      out_rate;
} thread_args_t;

void*
writer_thread( void* arg )
{
  thread_args_t* args      = arg;
  uint64_t       core      = args->core;
  chunk_t*       chunks    = args->chunks;
  uint32_t*      wtr_ready = args->wtr_ready;
  uint32_t*      rdr_ready = args->rdr_ready;

  bind( core );

  // the value that we will be sending to the other core
#if CHUNK_ELEMENT_T == m128
  CHUNK_ELEMENT_T v1 = {1, 2, 3, 4};
#elif CHUNK_ELEMENT_T == m256
  CHUNK_ELEMENT_T v1 = {1, 2, 3, 4, 5, 6, 7, 8};
#else
#error "unimpl chunk_t" #CHUNK_T
#endif

  CHUNK_ELEMENT_T c[ N_ELTS ];
  for( size_t i = 0; i < N_ELTS; ++i ) {
    c[ i ] = v1;
  }

  __atomic_store_n( wtr_ready, 1, __ATOMIC_RELEASE );
  while( !__atomic_load_n( rdr_ready, __ATOMIC_ACQUIRE ) ) { }

  uint64_t start = rdtscp();
  for( size_t i = 0; i < N_CHUNKS; ++i ) {
    chunk_t*         chunk = &chunks[i];
    CHUNK_ELEMENT_T* data  = chunk->data;
    uint64_t*        ready = &chunk->ready;

    memcpy( data, c, sizeof( c ) );

    // mark the chunk as ready
    // this doens't scale well as number of readers increases
    // may be a good use of Non Temporal Stores? https://lwn.net/Articles/255364/
    __atomic_store_n( ready, true, __ATOMIC_RELEASE );
  }
  uint64_t end = rdtscp();

  uint64_t ticks = end-start;
  double ns = ticks_to_ns( ticks );
  double sec = ns/1e9;
  *(args->out_rate) = (double)TRANSFER_MB/sec/1024;
  return NULL;
}

void*
reader_thread( void* arg )
{
  thread_args_t* args      = arg;
  uint64_t       core      = args->core;
  chunk_t*       chunks    = args->chunks;
  uint32_t*      wtr_ready = args->wtr_ready;
  uint32_t*      rdr_ready = args->rdr_ready;

  bind( core );

  CHUNK_ELEMENT_T c[ N_ELTS ];

  while( !__atomic_load_n( wtr_ready, __ATOMIC_ACQUIRE ) ) { }
  __atomic_store_n( rdr_ready, 1, __ATOMIC_RELEASE );

  uint64_t start = rdtscp();
  for( size_t i = 0; i < N_CHUNKS; ++i ) {
    chunk_t*         chunk = &chunks[i];
    CHUNK_ELEMENT_T* data  = chunk->data;
    uint64_t*        ready = &chunk->ready;

    // wait until the line is ready
    while( !__atomic_load_n( ready, __ATOMIC_ACQUIRE ) ) { }

    // have to convince compiler to leave this code in.
    memcpy( c, data, sizeof( c ) );
    for( size_t j = 0; j < N_ELTS; ++j ) SSE_USED( c[j] );
  }
  uint64_t end = rdtscp();

  uint64_t ticks = end-start;
  double ns = ticks_to_ns( ticks );
  double sec = ns/1e9;
  *(args->out_rate) = (double)TRANSFER_MB/sec/1024;
  return NULL;
}

static double
one_to_all( size_t    core1,
            size_t*   rdr_cores,
            size_t    n_rdr_cores,
            double*   out_rdr_rates,
            chunk_t * chunks )
{
  bind( core1 );

  // clear memory and prefault unallocated pages
  memset( chunks, 0, sizeof( chunk_t )*N_CHUNKS );

  // remove the region from all layers of cache
  for( size_t i = 0; i < sizeof( chunk_t )*N_CHUNKS; i+=CACHE_LINE ) {
    char* ptr = (char*)chunks + i;
    _mm_clflush( ptr );
  }

  uint32_t      wtr_ready = 0;
  uint32_t      rdr_ready = 0;
  double        write_rate;
  thread_args_t args[ n_rdr_cores+1 ];
  pthread_t     threads[ n_rdr_cores ];

  args[0].core      = core1;
  args[0].wtr_ready = &wtr_ready;
  args[0].rdr_ready = &rdr_ready;
  args[0].chunks    = chunks;
  args[0].out_rate  = &write_rate;

  for( size_t i = 1; i < n_rdr_cores+1; ++i ) {
    args[i].core      = rdr_cores[i-1];
    args[i].wtr_ready = &wtr_ready;
    args[i].rdr_ready = &rdr_ready;
    args[i].chunks    = chunks;
    args[i].out_rate  = &out_rdr_rates[i-1];
  }

  for( size_t i = 1; i < n_rdr_cores+1; ++i ) {
    pthread_create( &threads[i-1], NULL, reader_thread, (void*)&args[i] );
  }

  writer_thread( &args[0] );

  for( size_t i = 0; i < n_rdr_cores; ++i ) {
    pthread_join( threads[i], NULL );
  }

  return write_rate;
}

int main()
{
  chunk_t* chunks = NULL;
  // posix_memalign( (void**)&chunks, CACHE_LINE, sizeof( chunk_t )*N_CHUNKS );
  int fd = open( "/dev/hugepages/chunks", O_RDWR );
  if( fd < 0 ) {
    fprintf( stderr, "Failed to open %s\n", strerror( errno ) );
    return 1;
  }

  size_t sz = (sizeof( chunk_t )*N_CHUNKS + 0x2000000) & ~(0x2000000-1);
  assert( sz > sizeof( chunk_t )*N_CHUNKS );
  int ret = ftruncate( fd, sz );
  if( ret != 0 ) {
    fprintf( stderr, "Failed to truncate to %zu %s\n",
             sz,
             strerror( errno ) );
    return 1;
  }

  chunks = mmap( NULL,
                 sz,
                 PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_HUGETLB,
                 fd,
                 0 );

  if( chunks == (chunk_t*)MAP_FAILED ) {
    fprintf( stderr, "Failed to mmap %zu %s\n",
             sizeof( chunk_t )*N_CHUNKS,
             strerror( errno ) );
    return 1;
  }
  close( fd );

  FILE* writer_out = fopen("writer", "w+");
  FILE* reader_out = fopen("reader", "w+");

  for( size_t i = 0; i < 16; ++i ) {
    for( size_t j = 0; j < 16; ++j ) {
      if( i == j ) continue;
      size_t wtr = i;
      size_t rdr = j;

      double avg_wtr = 0.0;
      double avg_rdr = 0.0;

      for( size_t trial = 0; trial < TRIALS; ++trial ) {
        double rdr_rate;
        double wtr_rate = one_to_all( wtr, &rdr, 1, &rdr_rate, chunks );

        avg_rdr += rdr_rate;
        avg_wtr += wtr_rate;

        usleep(10); // space these out a bit incase system busy
      }

      fprintf( writer_out, "%zu,%zu,%f\n", i, j, avg_wtr/TRIALS);
      fprintf( reader_out, "%zu,%zu,%f\n", i, j, avg_rdr/TRIALS);

      printf( "wtr[%2zu] = %f\n", wtr, avg_wtr/TRIALS );
      printf( "rdr[%2zu] = %f\n", rdr, avg_rdr/TRIALS );
      printf( "\n" );
    }
  }

  fclose( reader_out );
  fclose( writer_out );

  // scaling test
  /* size_t wtr   = 0; */
  /* size_t rdr[] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}; */
  /* double cum_rate[ ARRAY_SIZE( rdr ) ]; */
  /* memset( cum_rate, 0, sizeof( cum_rate ) ); */

  /* for( size_t i = 1; i < ARRAY_SIZE( rdr ); ++i ) { */
  /*   double cum_wtr = 0.0; */
  /*   for( size_t trial = 0; trial < 100; ++trial ) { */
  /*     double rdr_rate[ ARRAY_SIZE( rdr ) ]; */
  /*     double rate = one_to_all( wtr, rdr, i, rdr_rate, chunks ); */
  /*     cum_wtr += rate; */
  /*     for( size_t x = 0; x < ARRAY_SIZE( rdr_rate ); ++x ) { */
  /*       cum_rate[ x ] += rdr_rate[ x ]; */
  /*     } */
  /*   } */

  /*   printf( "wtr[%2zu] = %f\n", wtr, cum_wtr/100 ); */
  /*   /\* for( size_t j = 0; j < i; ++j ) { *\/ */
  /*   /\*   printf( "rdr[%2zu] = %f\n", rdr[ j ], cum_rate[ j ]/100 ); *\/ */
  /*   /\* } *\/ */
  /*   printf("\n"); */
  /* } */

  // free( chunks );
  munmap( chunks, sz );
}
