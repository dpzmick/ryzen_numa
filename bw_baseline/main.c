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

static uint64_t tsc_freq_khz = 3892687; // AMD
static uint64_t
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

#define CACHE_LINE  (64ul)
#define N_CORES     (8ul)

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

static void
run_test( size_t core1, size_t core2 )
{
  bind( core1 );

  // allocate a bunch of memory on core1, write to the entire thing
  static const size_t sz = 4*1024ul*1024ul*1024ul;
  char* mem = NULL;
  posix_memalign( (void**)&mem, CACHE_LINE, sz );

  // write to the entire region of memory
  for( size_t i = 0; i < sz; ++i ) {
    // value that won't be known at compile time
    char v = ((char)((uint64_t)mem & 0xFF)) + i;
    mem[i] = v;
  }

  // switch to core2, read the entire region carefully (and maybe slowly)
  bind( core2 );

  // Read the entire region one char at a time (read: slowly)
  char* mem2 = NULL;
  posix_memalign( (void**)&mem2, CACHE_LINE, sz );
  memset( mem2, 0, sz ); // force kernel to allocate pages for region

  uint64_t start = rdtscp();
  memcpy( mem, mem2, sz );
  uint64_t end = rdtscp();

  double ns = ticks_to_ns(end-start);
  double sec = ns/1e9;
  double gig = (double)sz/1024./1024./1024.;
  printf("%zu,%zu,%f\n", core1, core2, gig/sec);

  free( mem2 );
  free( mem );
}

int main()
{
  for( size_t i = 0; i < 16; ++i ) {
    for( size_t j = 0; j < 16; ++j ) {
      fprintf( stderr, "%zu %zu\n", i, j );
      run_test( i, j );
    }
  }
}
