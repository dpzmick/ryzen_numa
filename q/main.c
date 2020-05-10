#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <sched.h>
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
#define CHUNK_LINES (32ul)

// type to use in chunk
#define CHUNK_ELEMENT_T m128

// compute the number of elements needed to fit in N cache lines
#define N_ELTS ( (CACHE_LINE*CHUNK_LINES)/sizeof( CHUNK_ELEMENT_T ) )

#define TRANSFER_MB ( ( N_CHUNKS*N_ELTS*sizeof( CHUNK_ELEMENT_T ) )/1024/1024 )

#define TRIALS (50)

typedef struct {
  uint64_t ready;
  char     pad[ CACHE_LINE- sizeof( uint64_t ) ];
} ready_bit_t;

typedef struct {
  _Alignas(CACHE_LINE) ready_bit_t     readies[ N_CHUNKS ];
  _Alignas(CACHE_LINE) CHUNK_ELEMENT_T datas[ N_CHUNKS ][ N_ELTS ];
} q_t;

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

static void __attribute__((noinline))
walk( char* mem, size_t sz )
{
  // touch every cache line to:
  // 1) make sure pages are allocated by kernel.
  // 2) bring mem into local cpu caches

  for( size_t i = 0; i < sz; i += CACHE_LINE ) {
    char tmp = mem[i];
    TAINT(tmp); // without this, the compiler removes the load and store
    mem[i] = tmp;
  }
}

typedef struct {
  size_t       core;
  uint32_t*    wtr_ready;
  uint32_t*    rdr_ready;
  q_t*         q;
  double*      out_rate;
} thread_args_t;

void*
writer_thread( void* arg )
{
  thread_args_t* args      = arg;
  uint64_t       core      = args->core;
  q_t*           q         = args->q;
  uint32_t*      wtr_ready = args->wtr_ready;
  uint32_t*      rdr_ready = args->rdr_ready;

  bind( core );
  walk( (char*)q->readies, sizeof( *q->readies )*N_CHUNKS );

  // the value that we will be sending to the other core
#if CHUNK_ELEMENT_T == m128
  CHUNK_ELEMENT_T v1 = {1, 2, 3, 4};
#elif CHUNK_ELEMENT_T == m256
  CHUNK_ELEMENT_T v1 = {1, 2, 3, 4, 5, 6, 7, 8};
#else
#error "unimpl chunk_t" #CHUNK_T
#endif

  __atomic_store_n( wtr_ready, 1, __ATOMIC_RELEASE );
  while( !__atomic_load_n( rdr_ready, __ATOMIC_ACQUIRE ) ) { }

  uint64_t start = rdtscp();
  for( size_t i = 0; i < N_CHUNKS; ++i ) {
    CHUNK_ELEMENT_T* data  = q->datas[i];
    uint64_t*        ready = &q->readies[i].ready;

    for( size_t j = 0; j < 1; ++j ) {
      data[j] = v1;
    }

    // mark the chunk as ready
    // __atomic_store_n( ready, true, __ATOMIC_RELEASE );

    uint64_t one = 1;
    __asm__ volatile("movnti %1,%0"
             : "=m" (*ready)
             : "r" (one)
             : "memory");
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
  q_t*           q         = args->q;
  uint32_t*      wtr_ready = args->wtr_ready;
  uint32_t*      rdr_ready = args->rdr_ready;

  bind( core );

  // register to load into
  CHUNK_ELEMENT_T v1;

  while( !__atomic_load_n( wtr_ready, __ATOMIC_ACQUIRE ) ) { }
  __atomic_store_n( rdr_ready, 1, __ATOMIC_RELEASE );

  uint64_t start = rdtscp();
  for( size_t i = 0; i < N_CHUNKS; ++i ) {
    CHUNK_ELEMENT_T* data  = q->datas[i];
    uint64_t*        ready = &q->readies[i].ready;

    // wait until the line is ready
    while( !__atomic_load_n( ready, __ATOMIC_ACQUIRE ) ) { }

    // read the entire data section into the sse register, but don't
    // do anything with it.
    // have to convince compiler to leave this code in.

    for( size_t j = 0; j < N_ELTS; ++j ) {
      v1 = data[j]; SSE_USED( v1 );
    }
  }
  uint64_t end = rdtscp();

  uint64_t ticks = end-start;
  double ns = ticks_to_ns( ticks );
  double sec = ns/1e9;
  *(args->out_rate) = (double)TRANSFER_MB/sec/1024;
  return NULL;
}

static double
one_to_all( size_t core1, size_t* rdr_cores, size_t n_rdr_cores, double* out_rdr_rates )
{
  bind( core1 );

  q_t* q = NULL;
  posix_memalign( (void**)&q, CACHE_LINE, sizeof( q_t ) );
  memset( q, 0, sizeof( *q ) ); // prefault

  uint32_t      wtr_ready = 0;
  uint32_t      rdr_ready = 0;
  double        write_rate;
  thread_args_t args[ n_rdr_cores+1 ];
  pthread_t     threads[ n_rdr_cores+1 ];

  for( size_t i = 1; i < n_rdr_cores+1; ++i ) {
    args[i].core      = rdr_cores[i-1];
    args[i].wtr_ready = &wtr_ready;
    args[i].rdr_ready = &rdr_ready;
    args[i].q         = q;
    args[i].out_rate  = &out_rdr_rates[i-1];
  }

  args[0].core      = core1;
  args[0].wtr_ready = &wtr_ready;
  args[0].rdr_ready = &rdr_ready;
  args[0].q         = q;
  args[0].out_rate  = &write_rate;

  for( size_t i = 1; i < n_rdr_cores+1; ++i ) {
    pthread_create( &threads[i], NULL, reader_thread, (void*)&args[i] );
  }

  pthread_create( &threads[0], NULL, writer_thread, (void*)&args[0] );

  for( size_t i = 1; i < n_rdr_cores+1; ++i ) {
    pthread_join( threads[i], NULL );
  }

  pthread_join( threads[0], NULL );
  free( q );
  return write_rate;
}

int main()
{
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
        double wtr_rate = one_to_all( wtr, &rdr, 1, &rdr_rate );

        avg_rdr += rdr_rate;
        avg_wtr += wtr_rate;
        /* printf("wtr: %f\n", wtr_rate); */
        /* printf("rdr: %f\n", rdr_rate); */
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

  /* size_t wtr   = 0; */
  /* size_t rdr[] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}; */
  /* double cum_rate[ ARRAY_SIZE( rdr ) ]; */
  /* memset( cum_rate, 0, sizeof( cum_rate ) ); */

  /* for( size_t i = 1; i < ARRAY_SIZE( rdr ); ++i ) { */
  /*   double cum_wtr = 0.0; */
  /*   for( size_t trial = 0; trial < 100; ++trial ) { */
  /*     double rdr_rate[ ARRAY_SIZE( rdr ) ]; */
  /*     double rate = one_to_all( wtr, rdr, i, rdr_rate ); */
  /*     cum_wtr += rate; */
  /*     for( size_t x = 0; x < ARRAY_SIZE( rdr_rate ); ++x ) { */
  /*       cum_rate[ x ] += rdr_rate[ x ]; */
  /*     } */
  /*   } */

  /*   printf( "wtr[%2zu] = %f\n", wtr, cum_wtr/100 ); */
  /*   for( size_t j = 0; j < i; ++j ) { */
  /*     // lies due to some readers starting late */
  /*     if( cum_rate[j]/100 < cum_wtr/100 ) { */
  /*       printf( "rdr[%2zu] = %f\n", rdr[ j ], cum_rate[ j ]/100 ); */
  /*     } */
  /*   } */
  /*   printf("\n"); */
  /* } */
}
