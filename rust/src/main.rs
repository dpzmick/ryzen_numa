#![feature(llvm_asm)]

extern crate affinity;

use std::arch::x86_64::*;
use std::cell::UnsafeCell;
use std::mem;
use std::sync::Arc;
use std::sync::atomic::AtomicBool;
use std::sync::atomic;
use std::thread;

use affinity::*;

const CACHE_SIZE: usize = 64;
const N_BLOCKS: usize = 8;
const N_AVX_WORD: usize = (CACHE_SIZE*N_BLOCKS)/std::mem::size_of::<__m256i>();

type Block = [__m256i; N_AVX_WORD];

fn use_m256(t: __m256i) {
    unsafe {
        llvm_asm!(
            ""          /* noop */
            :           /* no writes */
            : "x"(t)    /* read avx */
        );
    }
}

fn taint_m256(mut t: __m256i) {
    unsafe {
        llvm_asm!(
            ""          /* noop */
            :           /* no writes */
            : "x"(t)    /* read avx */
        );
        llvm_asm!(
            ""          /* noop */
            : "=x"(t)   /* no writes */
        );
        llvm_asm!(
            ""          /* noop */
            :           /* no writes */
            : "x"(t)    /* read avx */
        );
    }
}

struct Chunk {
    ready: AtomicBool,
    data:  UnsafeCell<Block>, // FIXME align to next cache line?
}

impl Chunk {
    fn new() -> Self {
        Self {
            ready: AtomicBool::new(false),
            data: UnsafeCell::new(unsafe { mem::zeroed() }),
        }
    }

    fn touch_on_core(&self) {
        unsafe {
            let a = (*self.data.get())[0];
            taint_m256(a);
            (*self.data.get())[0] = a;
        }
    }

    fn write_data(&self, data: Block) {
        // FIXME enforce the assumed exactly once semantics
        // this is currently unsafe
        unsafe{ *(self.data.get()) = data };
        self.ready.store(true, atomic::Ordering::Release);
    }

    fn get_data(&self) -> &Block {
        while !self.ready.load(atomic::Ordering::Acquire) { }
        return unsafe { &*self.data.get() };
    }
}

unsafe impl Sync for Chunk {}

impl Clone for Chunk {
    fn clone(&self) -> Self {
        Chunk {
            ready: AtomicBool::new(self.ready.load(atomic::Ordering::Relaxed)),
            data:  UnsafeCell::new(
                unsafe { (*self.data.get()).clone()} ),
        }
    }
}

#[inline(never)]
fn writer_thread(core: usize, chunks: &[Chunk]) -> u64 {
    set_thread_affinity(&[core]).expect("failed to bind");

    // bring everything into local cache
    for chunk in chunks {
        chunk.touch_on_core()
    }

    let v1 = unsafe { _mm256_set_epi32(1,2,3,4,5,6,7,8) };
    let mut aux = 0;
    let mut start = 0;
    for (i, chunk) in chunks.iter().enumerate() {
        let blk = [v1; N_AVX_WORD];
        chunk.write_data(blk);

        if i == 0 {
            start = unsafe { __rdtscp(&mut aux) };
        }
    }

    let end = unsafe { __rdtscp(&mut aux) };
    return end-start;
}

#[inline(never)]
fn reader_thread(core: usize, chunks: &[Chunk]) -> u64 {
    set_thread_affinity(&[core]).expect("failed to bind");

    let mut v1 = unsafe { _mm256_set_epi32(1,2,3,4,5,6,7,8) }; // needs init
    use_m256(v1); // silence warning

    let mut aux = 0;
    let mut start = 0;
    for (i, chunk) in chunks.iter().enumerate() {
        let d = chunk.get_data();

        if i == 0 {
            start = unsafe { __rdtscp(&mut aux) };
        }

        for d in d.iter() {
            v1 = *d;
            use_m256(v1);
        }
    }
    let end = unsafe { __rdtscp(&mut aux) };
    return end-start;
}

fn ticks_to_ns(ticks: u64) -> f64 {
    const tsc_freq_khz: u64 = 3892231; // AMD
    //     (freq_hz)     (ticks arg)
    //
    //      second       ticks
    //     --------- *  -------  = seconds
    //      ticks         1

    //      1e9 nanosecond     ticks
    //     --------------- *  -------  = nanoseconds
    //         ticks             1

    // ticks is in khz

    (ticks as f64 * 1e6) / (tsc_freq_khz as f64)
}

fn run_test(writer_core: usize, reader_cores: &'static [usize]) {
    // bind to writer for the allocation
    set_thread_affinity(&[writer_core]).expect("failed to bind");
    let chunks = Arc::new(std::iter::repeat(Chunk::new())
        .take((2<<26)/N_BLOCKS)
        .collect::<Vec<_>>());

    let sz = std::mem::size_of::<Block>() * chunks.len();
    let sz_mb = sz/1024/1024;

    let mut threads = Vec::new();
    for core in reader_cores {
        let c = chunks.clone();
        threads.push( thread::spawn(move || reader_thread(*core, &c)) );
    }

    let w = chunks.clone();
    let w = thread::spawn(move || writer_thread(writer_core, &w));

    let writer_sec = ticks_to_ns(w.join().unwrap())/1e9;
    println!("writer {} rate {}",
        writer_core,
        (sz_mb as f64)/1024./writer_sec);

    for (i, reader_thread) in threads.drain(..).enumerate() {
        let reader_sec = ticks_to_ns(reader_thread.join().unwrap())/1e9;
        println!("reader {}->{} rate {}",
            writer_core, reader_cores[i], (sz_mb as f64)/1024./reader_sec);
    }

    use std::time;
    thread::sleep(time::Duration::from_secs(2));
}

fn main() {
    let t1 = thread::spawn(move || {
        run_test( 0, &[ 3, 5, 7, 9, 11, 13, 15, 17 ]);
    });

    let t2 = thread::spawn(move || {
        run_test( 1, &[ 2, 4, 6, 8, 10, 12, 14, 16 ]);
    });

    // t1.join();
    t2.join();

    // run_test( chunks.clone(), 0, &[ 2, 4, 6, 8, 10, 12, 14, 1, 3, 5, 7, 9, 11, 13, 15 ]);
    // run_test( chunks.clone(), 0, &[ 2, 3, 5 ]);
    // run_test( chunks.clone(), 0, &[ 2, 4, 6, 8, 10, 12, 14 ]);
}

/*
 Package L#0
    NUMANode L#0 (P#0 16GB)
    L3 L#0 (20MB)
      L2 L#0 (256KB) + L1d L#0 (32KB) + L1i L#0 (32KB) + Core L#0
        PU L#0 (P#0)
        PU L#1 (P#16)
      L2 L#1 (256KB) + L1d L#1 (32KB) + L1i L#1 (32KB) + Core L#1
        PU L#2 (P#2)
        PU L#3 (P#18)
      L2 L#2 (256KB) + L1d L#2 (32KB) + L1i L#2 (32KB) + Core L#2
        PU L#4 (P#4)
        PU L#5 (P#20)
      L2 L#3 (256KB) + L1d L#3 (32KB) + L1i L#3 (32KB) + Core L#3
        PU L#6 (P#6)
        PU L#7 (P#22)
      L2 L#4 (256KB) + L1d L#4 (32KB) + L1i L#4 (32KB) + Core L#4
        PU L#8 (P#8)
        PU L#9 (P#24)
      L2 L#5 (256KB) + L1d L#5 (32KB) + L1i L#5 (32KB) + Core L#5
        PU L#10 (P#10)
        PU L#11 (P#26)
      L2 L#6 (256KB) + L1d L#6 (32KB) + L1i L#6 (32KB) + Core L#6
        PU L#12 (P#12)
        PU L#13 (P#28)
      L2 L#7 (256KB) + L1d L#7 (32KB) + L1i L#7 (32KB) + Core L#7
        PU L#14 (P#14)
        PU L#15 (P#30)

  Package L#1
    NUMANode L#1 (P#1 16GB)
    L3 L#1 (20MB)
      L2 L#8 (256KB) + L1d L#8 (32KB) + L1i L#8 (32KB) + Core L#8
        PU L#16 (P#1)
        PU L#17 (P#17)
      L2 L#9 (256KB) + L1d L#9 (32KB) + L1i L#9 (32KB) + Core L#9
        PU L#18 (P#3)
        PU L#19 (P#19)
      L2 L#10 (256KB) + L1d L#10 (32KB) + L1i L#10 (32KB) + Core L#10
        PU L#20 (P#5)
        PU L#21 (P#21)
      L2 L#11 (256KB) + L1d L#11 (32KB) + L1i L#11 (32KB) + Core L#11
        PU L#22 (P#7)
        PU L#23 (P#23)
      L2 L#12 (256KB) + L1d L#12 (32KB) + L1i L#12 (32KB) + Core L#12
        PU L#24 (P#9)
        PU L#25 (P#25)
      L2 L#13 (256KB) + L1d L#13 (32KB) + L1i L#13 (32KB) + Core L#13
        PU L#26 (P#11)
        PU L#27 (P#27)
      L2 L#14 (256KB) + L1d L#14 (32KB) + L1i L#14 (32KB) + Core L#14
        PU L#28 (P#13)
        PU L#29 (P#29)
      L2 L#15 (256KB) + L1d L#15 (32KB) + L1i L#15 (32KB) + Core L#15
        PU L#30 (P#15)
        PU L#31 (P#31)
*/
