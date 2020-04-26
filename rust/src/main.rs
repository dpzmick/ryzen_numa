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
const N_BLOCKS: usize = 64;
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

    // VERY UNSAFE
    fn reset(&self) {
        self.ready.store(false, atomic::Ordering::SeqCst);
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

fn run_test(chunks: Arc<Vec<Chunk>>, writer_core: usize, reader_cores: &'static [usize]) {
    let sz = std::mem::size_of::<Block>() * chunks.len();
    let sz_mb = sz/1024/1024;

    for chunk in chunks.iter() {
        chunk.reset();
    }

    let mut threads = Vec::new();
    for core in reader_cores {
        let c = chunks.clone();
        threads.push( thread::spawn(move || reader_thread(*core, &c)) );
    }

    let w = chunks.clone();
    let w = thread::spawn(move || writer_thread(writer_core, &w));

    let writer_sec = ticks_to_ns(w.join().unwrap())/1e9;
    println!("writer rate {}", (sz_mb as f64)/1024./writer_sec);

    for (i, reader_thread) in threads.drain(..).enumerate() {
        let reader_sec = ticks_to_ns(reader_thread.join().unwrap())/1e9;
        println!("reader {} rate {}", reader_cores[i], (sz_mb as f64)/1024./reader_sec);
    }

    use std::time;
    thread::sleep(time::Duration::from_secs(2));
}

fn main() {
    let chunks = std::iter::repeat(Chunk::new())
        .take((2<<27)/N_BLOCKS)
        .collect::<Vec<_>>();

    // put it in an arc so it can satisfy thread lifetime issues
    let chunks = Arc::new(chunks);

    println!("\nfast tests\n");
    run_test( chunks.clone(), 0, &[ 1, 2, 3, 4, 5, 6, 7 ]);
}
