extern crate affinity;

use affinity::*;
use std::arch::x86_64::*;
use std::sync::atomic::AtomicU32;
use std::sync::atomic::Ordering;
use std::thread;

static UNINIT:    u32 = 0;
static READY:     u32 = 1;
static PING:      u32 = 2;
static PONG:      u32 = 3;
static VAR: AtomicU32 = AtomicU32::new(UNINIT);

fn ticks_to_ns(ticks: u64) -> f64 {
    const tsc_freq_khz: u64 = 3892231; // AMD
    // const tsc_freq_khz: u64 = 2599982; // intel xeon
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

struct PingPong<'a> {
    val: &'a AtomicU32,
}

impl<'a> PingPong<'a> {
    fn new(val: &'a AtomicU32) -> Self {
        Self { val }
    }

    // call from thread one
    fn ping(&self) -> f64 {
        // wait for partner
        while self.val.load(Ordering::Acquire) != READY { }

        let mut aux = 0;
        let start = unsafe { __rdtscp(&mut aux) };
        // send ping
        self.val.store(PING, Ordering::Release);

        // wait for pong
        while self.val.load(Ordering::Acquire) != PONG { }

        let end = unsafe { __rdtscp(&mut aux) };
        return ticks_to_ns(end-start);
    }

    // call from thread two
    fn pong(&self) {
        // signal partner
        self.val.store(READY, Ordering::Release);

        // wait for ping
        while self.val.load(Ordering::Acquire) != PING { }

        // send pong
        self.val.store(PONG, Ordering::Release);
    }
}

fn run_test(core1: usize, core2: usize) -> f64 {
    set_thread_affinity(&[core1]).expect("failed to bind");
    VAR.store(UNINIT, Ordering::Relaxed);

    let p1 = PingPong::new(&VAR);
    let p2 = PingPong::new(&VAR);

    thread::spawn(move || {
        set_thread_affinity(&[core2]).expect("failed to bind");
        p2.pong()
    });

    return p1.ping();
}

fn run_tests(core1: usize, core2: usize) -> (f64, f64) {
    let mut results = Vec::new();
    for _ in 0..100 {
        results.push( run_test(core1, core2) );
    }

    let sum = results.iter().sum::<f64>();
    let mean = sum/(results.len() as f64);

    let mut diffs = Vec::new();
    for result in results.iter() {
        diffs.push( (result-mean)*(result-mean) );
    }

    let var = diffs.iter().sum::<f64>()/(diffs.len() as f64);

    ( mean, var )
}

fn main() {
    let cores = (0..16).collect::<Vec<_>>();

    for &core1 in cores.iter() {
        for &core2 in cores.iter() {
            if core1 == core2 { continue; }
            let r = run_tests(core1, core2);
            println!("{},{},{},{}", core1, core2, r.0, r.1);
        }
    }
}
