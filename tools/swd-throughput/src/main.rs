//! How fast is this probe's SWD, end to end through probe-rs?
//!
//! The question behind it is whether a firmware change slowed the probe down, and "slower" has two
//! separate shapes. A change to the bit loop costs per byte; a change to command dispatch or the USB
//! path costs per transfer, whatever its size. One throughput figure cannot tell them apart, so every
//! arm runs at several transfer sizes and the time is fit to `a + b * bytes`: `a` is the fixed cost of
//! a transfer and `1/b` is the ceiling a large one approaches.
//!
//! Sweeping the SWD clock on top of that says which of the two is actually binding. Throughput that
//! stops rising with the clock is bound by something off the wire, and a slower bit loop would then
//! cost nothing at all.

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::time::{Duration, Instant};

use anyhow::Context as _;
use indicatif::ProgressStyle;
use probe_rs::probe::{DebugProbeSelector, WireProtocol, list::Lister};
use probe_rs::{MemoryInterface, Permissions, Session, config::TargetSelector};
use tracing::{Span, info, info_span, warn};
use tracing_indicatif::IndicatifLayer;
use tracing_indicatif::span_ext::IndicatifSpanExt as _;
use tracing_subscriber::prelude::*;

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
enum Arm {
    Read32,
    Write32,
    Read8,
    Write8,
}

impl Arm {
    const ALL: [Arm; 4] = [Arm::Read32, Arm::Write32, Arm::Read8, Arm::Write8];

    fn name(self) -> &'static str {
        match self {
            Arm::Read32 => "read32",
            Arm::Write32 => "write32",
            Arm::Read8 => "read8",
            Arm::Write8 => "write8",
        }
    }

    /// Clocks one word costs on an idealised SWD wire: an 8-bit request, the turnarounds and
    /// acknowledge, 32 data bits and parity. The probe is free to add idle cycles, so a measured
    /// figure at or near this ceiling means the wire is the limit and nothing else can be.
    const CLOCKS_PER_WORD: f64 = 46.0;
}

#[derive(clap::Parser)]
#[command(about = "SWD throughput of a probe, decomposed into fixed and per-byte cost")]
struct Cli {
    /// Chip to attach to. The vendor's own debug sequence runs, as it would in normal use.
    #[arg(long, default_value = "MSPM0L1306")]
    chip: String,

    /// Probe selector, `VID:PID` or `VID:PID:SERIAL`. The only probe found, if left out.
    #[arg(long)]
    probe: Option<String>,

    /// SWD clock speeds to sweep, in kHz.
    #[arg(long, value_delimiter = ',', default_values_t = [1000u32, 2000, 5000, 10000, 20000, 30000, 50000])]
    speeds: Vec<u32>,

    /// Transfer sizes to measure at each speed, in bytes. Two or more are needed for the fit.
    #[arg(long, value_delimiter = ',', default_values_t = [64usize, 256, 1024, 4096])]
    sizes: Vec<usize>,

    /// Timed runs per point. The median is reported.
    #[arg(long, default_value_t = 7)]
    repeats: usize,

    /// RAM to transfer against. Written to and restored before exit.
    #[arg(long, value_parser = parse_hex, default_value = "0x20000000")]
    base: u64,

    /// Single-word reads to time per speed, for the round-trip figure.
    #[arg(long, default_value_t = 200)]
    round_trips: usize,

    /// Write the per-point rows here as CSV.
    #[arg(long)]
    csv: Option<String>,

    /// Leave the core running. It is halted by default, so the target's own bus traffic does not
    /// land in the measurement.
    #[arg(long)]
    no_halt: bool,
}

fn parse_hex(text: &str) -> Result<u64, std::num::ParseIntError> {
    match text.strip_prefix("0x").or_else(|| text.strip_prefix("0X")) {
        Some(hex) => u64::from_str_radix(hex, 16),
        None => text.parse(),
    }
}

/// One timed point: an arm at one size and one clock.
struct Point {
    speed_khz: u32,
    arm: Arm,
    bytes: usize,
    median: Duration,
}

impl Point {
    fn bytes_per_second(&self) -> f64 {
        self.bytes as f64 / self.median.as_secs_f64()
    }
}

fn main() -> anyhow::Result<()> {
    let cli = <Cli as clap::Parser>::parse();
    anyhow::ensure!(cli.sizes.len() >= 2, "two sizes or more, for the fit");
    anyhow::ensure!(cli.repeats > 0, "at least one timed run");

    let indicatif = IndicatifLayer::new();
    tracing_subscriber::registry()
        .with(
            tracing_subscriber::fmt::layer()
                .without_time()
                .with_target(false)
                .with_writer(indicatif.get_stderr_writer()),
        )
        .with(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("warn,swd_throughput=info")),
        )
        .with(indicatif)
        .init();

    let largest = *cli.sizes.iter().max().expect("checked non-empty");
    let mut points = Vec::new();
    let mut round_trips = BTreeMap::new();
    let mut achieved = BTreeMap::new();
    let mut failed = BTreeMap::new();

    let per_speed = (cli.sizes.len() * Arm::ALL.len() + 1) as u64;
    let sweep = info_span!("sweep");
    sweep.pb_set_style(&bar_style(
        "{spinner} {msg} {wide_bar} {pos}/{len} {elapsed}",
    )?);
    sweep.pb_set_length(cli.speeds.len() as u64 * per_speed);
    let sweep = sweep.entered();

    for &speed_khz in &cli.speeds {
        // One session per speed. Setting the clock before attaching means the whole connection,
        // including the vendor's sequence, runs at the speed being reported.
        // A speed the wire cannot carry fails at attach. That is a result about this clock, not a
        // reason to discard the speeds already measured, so the sweep records it and carries on.
        let (mut session, actual) = match attach(&cli, speed_khz) {
            Ok(session) => session,
            Err(error) => {
                warn!("{speed_khz} kHz: no attach ({error:#})");
                Span::current().pb_inc(per_speed);
                continue;
            }
        };
        achieved.insert(speed_khz, actual);
        if actual != speed_khz {
            warn!("asked for {speed_khz} kHz, probe reports {actual} kHz");
        }

        // A transfer that fails at this clock is a result about the clock as well, and the speeds
        // measured already stand.
        match measure_speed(&cli, &mut session, speed_khz, largest) {
            Ok((measured, per_access)) => {
                points.extend(measured);
                round_trips.insert(speed_khz, per_access);
            }
            Err(error) => {
                warn!("{speed_khz} kHz: failed ({error:#})");
                failed.insert(speed_khz, format!("{error:#}"));
            }
        }
        Span::current().pb_inc(per_speed);
    }

    drop(sweep);

    print!("{}", report(&cli, &points, &round_trips, &achieved));
    for (speed_khz, error) in &failed {
        println!("{speed_khz} kHz failed: {error}");
    }

    if let Some(path) = &cli.csv {
        // probe_khz is the clock the probe took, which a divider rounds down from the one asked.
        // The rt rows are single-word reads timed one at a time.
        let mut csv = String::from("speed_khz,probe_khz,arm,bytes,median_us,bytes_per_second\n");
        for point in &points {
            writeln!(
                csv,
                "{},{},{},{},{:.3},{:.1}",
                point.speed_khz,
                achieved[&point.speed_khz],
                point.arm.name(),
                point.bytes,
                point.median.as_secs_f64() * 1e6,
                point.bytes_per_second(),
            )?;
        }
        for (speed_khz, per_access) in &round_trips {
            writeln!(
                csv,
                "{},{},rt,4,{:.3},{:.1}",
                speed_khz,
                achieved[speed_khz],
                per_access.as_secs_f64() * 1e6,
                4.0 / per_access.as_secs_f64(),
            )?;
        }
        for speed_khz in failed.keys() {
            writeln!(csv, "{},{},failed,0,0,0", speed_khz, achieved[speed_khz])?;
        }
        std::fs::write(path, csv)?;
        eprintln!("wrote {path}");
    }

    Ok(())
}

/// Every arm, then the round trips, at the clock `session` was attached at. The RAM under test is
/// put back and the core resumed even when a transfer fails, so a failed clock leaves the target
/// as the sweep found it.
fn measure_speed(
    cli: &Cli,
    session: &mut Session,
    speed_khz: u32,
    largest: usize,
) -> anyhow::Result<(Vec<Point>, Duration)> {
    check_fits_in_ram(session, cli.base, largest)?;
    let mut core = session.core(0)?;
    if !cli.no_halt {
        core.halt(Duration::from_millis(500))?;
    }
    let mut original = vec![0u8; largest];
    core.read(cli.base, &mut original)
        .with_context(|| format!("reading {largest} bytes at {:#x}", cli.base))?;

    let measured = run_arms(cli, &mut core, speed_khz);
    // After a failure these can fail as well, on the same wire; the first error is the one to report.
    let restored = core.write(cli.base, &original);
    let resumed = if cli.no_halt { Ok(()) } else { core.run() };
    let measured = measured?;
    restored?;
    resumed?;
    Ok(measured)
}

fn run_arms(
    cli: &Cli,
    core: &mut probe_rs::Core<'_>,
    speed_khz: u32,
) -> anyhow::Result<(Vec<Point>, Duration)> {
    let mut points = Vec::new();
    for arm in Arm::ALL {
        for &bytes in &cli.sizes {
            let median = time_arm(core, arm, cli.base, bytes, cli.repeats)?;
            let point = Point {
                speed_khz,
                arm,
                bytes,
                median,
            };
            info!(
                "{:>6} kHz {:>7} {:>6} B {:>9.3} ms {:>10.1} KiB/s",
                speed_khz,
                arm.name(),
                bytes,
                median.as_secs_f64() * 1e3,
                point.bytes_per_second() / 1024.0,
            );
            points.push(point);
        }
    }
    let per_access = time_round_trips(core, cli.base, cli.round_trips)?;
    info!(
        "{:>6} kHz {:>7} {:>19.1} us {:>8.0} /s",
        speed_khz,
        "rt",
        per_access.as_secs_f64() * 1e6,
        1.0 / per_access.as_secs_f64(),
    );
    Ok((points, per_access))
}

fn bar_style(template: &str) -> anyhow::Result<ProgressStyle> {
    Ok(ProgressStyle::with_template(template)?)
}

/// Refuses a transfer that would run past the end of the RAM region it starts in.
///
/// Reads past the end of RAM do not fail loudly — the AP answers, the words come back, and the
/// timing is of whatever the bus does with an unmapped address rather than of the probe.
fn check_fits_in_ram(session: &Session, base: u64, bytes: usize) -> anyhow::Result<()> {
    let end = base + bytes as u64;
    let ram = session
        .target()
        .memory_map
        .iter()
        .filter_map(|region| match region {
            probe_rs::config::MemoryRegion::Ram(ram) => Some(&ram.range),
            _ => None,
        })
        .find(|range| range.contains(&base));

    match ram {
        Some(range) if end <= range.end => Ok(()),
        Some(range) => anyhow::bail!(
            "{bytes} bytes at {base:#x} runs past RAM {:#x}..{:#x}; largest size here is {}",
            range.start,
            range.end,
            range.end - base
        ),
        None => anyhow::bail!("{base:#x} is not in any RAM region of this target"),
    }
}

/// Attaches at `speed_khz`, and reports the speed the probe actually took.
fn attach(cli: &Cli, speed_khz: u32) -> anyhow::Result<(Session, u32)> {
    let lister = Lister::new();
    let selector: DebugProbeSelector = match &cli.probe {
        Some(text) => text.parse()?,
        None => {
            let found = lister.list_all();
            let only = match found.as_slice() {
                [one] => one,
                [] => anyhow::bail!("no probe found"),
                many => anyhow::bail!(
                    "{} probes found, pass --probe: {}",
                    many.len(),
                    many.iter()
                        .map(|p| format!("{:04x}:{:04x}", p.vendor_id, p.product_id))
                        .collect::<Vec<_>>()
                        .join(", ")
                ),
            };
            only.into()
        }
    };

    let mut probe = lister.open(selector)?;
    probe.select_protocol(WireProtocol::Swd)?;
    let actual = probe.set_speed(speed_khz)?;
    let session = probe.attach(
        TargetSelector::Unspecified(cli.chip.clone()),
        Permissions::default(),
    )?;
    Ok((session, actual))
}

fn time_arm(
    core: &mut probe_rs::Core<'_>,
    arm: Arm,
    base: u64,
    bytes: usize,
    repeats: usize,
) -> anyhow::Result<Duration> {
    let mut words = vec![0u32; bytes / 4];
    let mut raw = vec![0u8; bytes];

    let run = info_span!("point");
    run.pb_set_style(&bar_style("  {msg} {wide_bar} {pos}/{len}")?);
    run.pb_set_message(&format!("{} {bytes} B", arm.name()));
    run.pb_set_length(repeats as u64 + 1);
    let run = run.entered();

    let mut medians = Vec::with_capacity(repeats);
    // One untimed run first: the first transfer at a new size pays for whatever the AP's address
    // and control registers still hold from the previous one.
    for attempt in 0..=repeats {
        let started = Instant::now();
        match arm {
            Arm::Read32 => core.read_32(base, &mut words)?,
            Arm::Write32 => core.write_32(base, &words)?,
            Arm::Read8 => core.read_8(base, &mut raw)?,
            Arm::Write8 => core.write_8(base, &raw)?,
        }
        let elapsed = started.elapsed();
        if attempt > 0 {
            medians.push(elapsed);
        }
        Span::current().pb_inc(1);
    }
    drop(run);

    medians.sort_unstable();
    Ok(medians[medians.len() / 2])
}

/// Microseconds one single-word read costs, start to finish.
///
/// Nothing can be batched here: each read is a separate call and so a separate round trip. This is
/// the figure a change to command dispatch moves, and a change to the bit loop barely touches.
fn time_round_trips(
    core: &mut probe_rs::Core<'_>,
    base: u64,
    count: usize,
) -> anyhow::Result<Duration> {
    core.read_word_32(base)?;

    let started = Instant::now();
    for _ in 0..count {
        core.read_word_32(base)?;
    }
    Ok(started.elapsed() / count as u32)
}

/// Least squares over the size points: seconds = fixed + per_byte * bytes.
fn fit(points: &[&Point]) -> (f64, f64) {
    let n = points.len() as f64;
    let mean_x = points.iter().map(|p| p.bytes as f64).sum::<f64>() / n;
    let mean_y = points.iter().map(|p| p.median.as_secs_f64()).sum::<f64>() / n;
    let covariance: f64 = points
        .iter()
        .map(|p| (p.bytes as f64 - mean_x) * (p.median.as_secs_f64() - mean_y))
        .sum();
    let variance: f64 = points
        .iter()
        .map(|p| (p.bytes as f64 - mean_x).powi(2))
        .sum();
    let per_byte = covariance / variance;
    (mean_y - per_byte * mean_x, per_byte)
}

fn report(
    cli: &Cli,
    points: &[Point],
    round_trips: &BTreeMap<u32, Duration>,
    achieved: &BTreeMap<u32, u32>,
) -> String {
    let mut out = String::new();

    let _ = writeln!(out, "\nmedian throughput, KiB/s\n");
    let _ = write!(out, "{:>8}", "kHz");
    for arm in Arm::ALL {
        for bytes in &cli.sizes {
            let _ = write!(out, "{:>12}", format!("{} {}", arm.name(), bytes));
        }
    }
    let _ = writeln!(out, "{:>10}", "rt us");

    for speed in &cli.speeds {
        let _ = write!(out, "{speed:>8}");
        for arm in Arm::ALL {
            for bytes in &cli.sizes {
                match points
                    .iter()
                    .find(|p| p.speed_khz == *speed && p.arm == arm && p.bytes == *bytes)
                {
                    Some(point) => {
                        let _ = write!(out, "{:>12.1}", point.bytes_per_second() / 1024.0);
                    }
                    None => {
                        let _ = write!(out, "{:>12}", "-");
                    }
                }
            }
        }
        match round_trips.get(speed) {
            Some(rt) => {
                let _ = writeln!(out, "{:>10.1}", rt.as_secs_f64() * 1e6);
            }
            None => {
                let _ = writeln!(out, "{:>10}", "-");
            }
        }
    }

    let _ = writeln!(
        out,
        "\nfixed cost per transfer and the ceiling a large one approaches\n"
    );
    let _ = writeln!(
        out,
        "{:>8} {:>8} {:>10} {:>12} {:>12} {:>8}",
        "kHz", "arm", "fixed us", "ceiling KiB/s", "wire KiB/s", "of wire"
    );
    for speed in &cli.speeds {
        // The clock the probe actually took, not the one asked for: a probe that rounds down to a
        // divisor of its own clock would otherwise be charged for the difference.
        let actual = *achieved.get(speed).unwrap_or(speed) as f64;
        let wire = actual * 1e3 / Arm::CLOCKS_PER_WORD * 4.0;
        for arm in Arm::ALL {
            let selected: Vec<&Point> = points
                .iter()
                .filter(|p| p.speed_khz == *speed && p.arm == arm)
                .collect();
            if selected.len() < 2 {
                continue;
            }
            let (fixed, per_byte) = fit(&selected);
            let ceiling = 1.0 / per_byte;
            let _ = writeln!(
                out,
                "{:>8} {:>8} {:>10.1} {:>12.1} {:>12.1} {:>7.0}%",
                speed,
                arm.name(),
                fixed * 1e6,
                ceiling / 1024.0,
                wire / 1024.0,
                ceiling / wire * 100.0,
            );
        }
    }

    out
}
