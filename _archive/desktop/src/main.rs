use clap::Parser;
use std::fs;
use std::io::{self, BufRead};
use std::process::ExitCode;

#[derive(Parser, Debug)]
#[command(name = "strand-desktop")]
#[command(about = "Deterministic STRAND analysis for 128-bit hex words")]
struct Args {
    /// Path to UTF-8 text file (one hex word per line; optional 0x prefix; # comments).
    #[arg(long)]
    file: Option<String>,

    /// Run one scheduling pass instead of fixed-point replay.
    #[arg(long)]
    single_pass: bool,

    /// Maximum fixed-point iterations.
    #[arg(long, default_value_t = 32)]
    max_iterations: usize,

    /// Emit a compact text summary instead of JSON.
    #[arg(long)]
    text: bool,
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(err) => {
            eprintln!("strand-desktop: {err}");
            ExitCode::FAILURE
        }
    }
}

fn run() -> Result<(), String> {
    let args = Args::parse();
    let lines = read_lines(args.file.as_deref())?;
    let words = strand_core::parse_hex_words(&lines)?;
    let report = strand_core::analyze_words_with_config(
        &words,
        strand_core::AnalyzeConfig {
            fixed_point: !args.single_pass,
            max_iterations: args.max_iterations,
        },
    );

    if args.text {
        print_text_report(&report);
    } else {
        println!(
            "{}",
            serde_json::to_string_pretty(&report)
                .map_err(|err| format!("failed to serialize report: {err}"))?
        );
    }

    Ok(())
}

fn read_lines(path: Option<&str>) -> Result<Vec<String>, String> {
    if let Some(path) = path {
        fs::read_to_string(path)
            .map_err(|err| format!("failed to read {path}: {err}"))
            .map(|contents| contents.lines().map(str::to_string).collect())
    } else {
        io::stdin()
            .lock()
            .lines()
            .collect::<Result<Vec<_>, _>>()
            .map_err(|err| format!("failed to read stdin: {err}"))
    }
}

fn print_text_report(report: &strand_core::StrandReport) {
    println!("STRAND analysis");
    println!("words: {}", report.input_len);
    println!("masks:");
    println!("  read:     {}", report.masks.read_hex);
    println!("  write:    {}", report.masks.write_hex);
    println!("  kill:     {}", report.masks.kill_hex);
    println!(
        "thresholds: FC={} AC={} OC={}",
        report.thresholds.theta_fc, report.thresholds.theta_ac, report.thresholds.theta_oc
    );
    println!(
        "blocks: {}  edges: {} ({} unique pairs)  invisible: {}",
        report.blocks.len(),
        report.edges.len(),
        report.edge_pair_count,
        report.invisible_words.len()
    );
    println!(
        "edge mix: flow={} anti={} output={} max_weight={} total_weight={}",
        report.edge_summary.flow_edges,
        report.edge_summary.anti_edges,
        report.edge_summary.output_edges,
        report.edge_summary.max_weight,
        report.edge_summary.total_weight
    );
    println!(
        "transition cost: {} -> {} (delta {})",
        report.costs.original_transition_cost,
        report.costs.scheduled_transition_cost,
        report.costs.delta
    );
    println!(
        "best replay: pass {} cost {}  critical_path={} cycle={}",
        report.schedule_quality.selected_iteration,
        report.schedule_quality.best_transition_cost,
        report.schedule_quality.critical_path_len,
        report.schedule_quality.cycle_detected
    );
    println!("converged: {}", report.converged);
    println!(
        "verified: xor={} edge_order={} permutation={}",
        report.verification.xor_kill_projection_preserved,
        report.verification.edge_order_preserved,
        report.verification.scheduled_all_words
    );
    println!("schedule_order: {:?}", report.schedule_order);
}
