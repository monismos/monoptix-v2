//! STRAND Studio core.
//!
//! Deterministic, self-derived analysis for fixed-width `u128` records:
//! entropy masks, triple conflict thresholds, conflict DAG construction,
//! block-local scheduling, fixed-point replay, and verification reports.

use serde::Serialize;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct StrandMasks {
    pub read: u128,
    pub write: u128,
    pub kill: u128,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub struct Thresholds {
    pub theta_fc: u32,
    pub theta_ac: u32,
    pub theta_oc: u32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum ConflictKind {
    Flow,
    Anti,
    Output,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct ConflictEdge {
    pub source: usize,
    pub target: usize,
    pub kind: ConflictKind,
    pub weight: u32,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct BlockReport {
    pub id: usize,
    pub start: usize,
    pub end_exclusive: usize,
    pub len: usize,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct BlockCostReport {
    pub id: usize,
    pub original_transition_cost: u64,
    pub scheduled_transition_cost: u64,
    pub delta: i64,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct InvisibleWord {
    pub index: usize,
    pub word_hex: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct IterationReport {
    pub iteration: usize,
    pub order: Vec<usize>,
    pub transition_cost: u64,
    pub changed_positions: usize,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct MaskReport {
    pub read_hex: String,
    pub write_hex: String,
    pub kill_hex: String,
    pub variable_hex: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct CostReport {
    pub original_transition_cost: u64,
    pub scheduled_transition_cost: u64,
    pub delta: i64,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct EdgeSummary {
    pub flow_edges: usize,
    pub anti_edges: usize,
    pub output_edges: usize,
    pub total_weight: u64,
    pub max_weight: u32,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct ScheduleQualityReport {
    pub selected_iteration: usize,
    pub best_transition_cost: u64,
    pub critical_path_len: usize,
    pub cycle_detected: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct VerificationReport {
    pub xor_kill_projection_preserved: bool,
    pub edge_order_preserved: bool,
    pub scheduled_all_words: bool,
    pub violations: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct StrandReport {
    pub input_len: usize,
    pub input_words_hex: Vec<String>,
    pub masks: MaskReport,
    pub thresholds: Thresholds,
    pub blocks: Vec<BlockReport>,
    pub block_costs: Vec<BlockCostReport>,
    pub edges: Vec<ConflictEdge>,
    pub edge_pair_count: usize,
    pub edge_summary: EdgeSummary,
    pub invisible_words: Vec<InvisibleWord>,
    pub iterations: Vec<IterationReport>,
    pub converged: bool,
    pub schedule_order: Vec<usize>,
    pub scheduled_words_hex: Vec<String>,
    pub costs: CostReport,
    pub schedule_quality: ScheduleQualityReport,
    pub verification: VerificationReport,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct AnalyzeConfig {
    pub fixed_point: bool,
    pub max_iterations: usize,
}

impl Default for AnalyzeConfig {
    fn default() -> Self {
        Self {
            fixed_point: true,
            max_iterations: 32,
        }
    }
}

/// Bits that vary across the corpus.
pub fn variable_bit_mask(words: &[u128]) -> u128 {
    if words.is_empty() {
        return 0;
    }
    let mut or_all = 0u128;
    let mut and_all = !0u128;
    for &w in words {
        or_all |= w;
        and_all &= w;
    }
    or_all ^ and_all
}

/// Kill-mask seed from adjacent XOR transitions.
pub fn transition_xor_mask(words: &[u128]) -> u128 {
    words.windows(2).fold(0u128, |acc, w| acc | (w[0] ^ w[1]))
}

/// Self-derived read/write/kill masks. Read and write masks are split from
/// variable bit entropy without ISA tables; balanced bits are allowed in both.
pub fn derive_masks(words: &[u128]) -> StrandMasks {
    let variable = variable_bit_mask(words);
    let kill = transition_xor_mask(words);
    if words.is_empty() {
        return StrandMasks {
            read: 0,
            write: 0,
            kill,
        };
    }

    let mut read = 0u128;
    let mut write = 0u128;
    let n = words.len() as u32;

    for bit in 0..128 {
        let mask = 1u128 << bit;
        if variable & mask == 0 {
            continue;
        }
        let ones = words.iter().filter(|&&word| word & mask != 0).count() as u32;
        let zeros = n - ones;
        if ones <= zeros {
            read |= mask;
        }
        if ones >= zeros {
            write |= mask;
        }
    }

    if read == 0 {
        read = variable;
    }
    if write == 0 {
        write = variable;
    }

    StrandMasks { read, write, kill }
}

#[inline]
pub fn priority(word: u128, mw: u128) -> u32 {
    (word & mw).count_ones()
}

pub fn format_hex_word(word: u128) -> String {
    format!("{word:032x}")
}

pub fn derive_thresholds(words: &[u128], masks: StrandMasks, blocks: &[BlockReport]) -> Thresholds {
    let mut fc = Vec::new();
    let mut ac = Vec::new();
    let mut oc = Vec::new();

    for block in blocks {
        for i in block.start..block.end_exclusive {
            for j in (i + 1)..block.end_exclusive {
                let forward = ((words[i] & masks.write) & (words[j] & masks.read)).count_ones();
                let anti = ((words[j] & masks.write) & (words[i] & masks.read)).count_ones();
                let output = ((words[i] & masks.write) & (words[j] & masks.write)).count_ones();
                if forward > 0 {
                    fc.push(forward);
                }
                if anti > 0 {
                    ac.push(anti);
                }
                if output > 0 {
                    oc.push(output);
                }
            }
        }
    }

    Thresholds {
        theta_fc: lower_median(&mut fc),
        theta_ac: lower_median(&mut ac),
        theta_oc: lower_median(&mut oc),
    }
}

pub fn derive_blocks(words: &[u128], kill_mask: u128) -> Vec<BlockReport> {
    if words.is_empty() {
        return Vec::new();
    }
    if words.len() == 1 {
        return vec![BlockReport {
            id: 0,
            start: 0,
            end_exclusive: 1,
            len: 1,
        }];
    }

    let costs: Vec<u32> = words
        .windows(2)
        .map(|pair| ((pair[0] ^ pair[1]) & kill_mask).count_ones())
        .collect();
    let mean = costs.iter().map(|&v| v as f64).sum::<f64>() / costs.len() as f64;
    let variance = costs
        .iter()
        .map(|&v| {
            let d = v as f64 - mean;
            d * d
        })
        .sum::<f64>()
        / costs.len() as f64;
    let cutoff = mean + 2.0 * variance.sqrt();

    let mut blocks = Vec::new();
    let mut start = 0usize;
    for (idx, &cost) in costs.iter().enumerate() {
        if (cost as f64) > cutoff {
            push_block(&mut blocks, start, idx + 1);
            start = idx + 1;
        }
    }
    push_block(&mut blocks, start, words.len());
    blocks
}

pub fn build_conflict_edges(
    words: &[u128],
    masks: StrandMasks,
    thresholds: Thresholds,
    blocks: &[BlockReport],
) -> Vec<ConflictEdge> {
    let mut edges = Vec::new();
    for block in blocks {
        for i in block.start..block.end_exclusive {
            for j in (i + 1)..block.end_exclusive {
                let forward = ((words[i] & masks.write) & (words[j] & masks.read)).count_ones();
                let anti = ((words[j] & masks.write) & (words[i] & masks.read)).count_ones();
                let output = ((words[i] & masks.write) & (words[j] & masks.write)).count_ones();

                if forward > thresholds.theta_fc {
                    edges.push(ConflictEdge {
                        source: i,
                        target: j,
                        kind: ConflictKind::Flow,
                        weight: forward,
                    });
                }
                if anti > thresholds.theta_ac {
                    edges.push(ConflictEdge {
                        source: i,
                        target: j,
                        kind: ConflictKind::Anti,
                        weight: anti,
                    });
                }
                if output > thresholds.theta_oc {
                    edges.push(ConflictEdge {
                        source: i,
                        target: j,
                        kind: ConflictKind::Output,
                        weight: output,
                    });
                }
            }
        }
    }
    edges
}

/// Topological, block-local STRAND schedule. Priorities are descending
/// `popcount(si & M_W)`; ties prefer lower adjacent transition cost, then
/// previous iteration rank, then index.
pub fn schedule_indices(words: &[u128]) -> Vec<usize> {
    analyze_words(words).schedule_order
}

pub fn analyze_words(words: &[u128]) -> StrandReport {
    analyze_words_with_config(words, AnalyzeConfig::default())
}

pub fn analyze_words_with_config(words: &[u128], config: AnalyzeConfig) -> StrandReport {
    let variable = variable_bit_mask(words);
    let masks = derive_masks(words);
    let blocks = derive_blocks(words, masks.kill);
    let thresholds = derive_thresholds(words, masks, &blocks);
    let edges = build_conflict_edges(words, masks, thresholds, &blocks);
    let edge_pair_count = count_edge_pairs(&edges);
    let edge_summary = summarize_edges(&edges);
    let critical_path_len = conflict_critical_path_len(&edges, words.len());
    let invisible_words = detect_invisible_words(words, masks);

    let mut tie_rank: Vec<usize> = (0..words.len()).collect();
    let mut current_order: Vec<usize> = (0..words.len()).collect();
    let max_iterations = config.max_iterations.max(1);
    let mut iterations = Vec::new();
    let mut converged = false;
    let mut cycle_detected = false;
    let mut seen_orders = vec![current_order.clone()];
    let mut best_order = current_order.clone();
    let mut best_transition_cost = u64::MAX;
    let mut selected_iteration = 0usize;

    for iteration in 0..max_iterations {
        let next_order = schedule_with_tie_rank(words, masks, &edges, &blocks, &tie_rank);
        let changed_positions = count_position_changes(&current_order, &next_order);
        let transition_cost = transition_cost_for_order(words, &next_order, masks.kill);

        if transition_cost < best_transition_cost {
            best_transition_cost = transition_cost;
            best_order = next_order.clone();
            selected_iteration = iteration;
        }

        iterations.push(IterationReport {
            iteration,
            order: next_order.clone(),
            transition_cost,
            changed_positions,
        });

        if next_order == current_order {
            converged = true;
            current_order = next_order;
            break;
        }

        if seen_orders.iter().any(|seen| seen == &next_order) {
            cycle_detected = true;
            current_order = next_order;
            break;
        }

        current_order = next_order;
        tie_rank = rank_for_order(&current_order);
        seen_orders.push(current_order.clone());

        if !config.fixed_point {
            break;
        }
    }

    if !config.fixed_point && iterations.len() == 1 {
        converged = true;
    }

    if best_transition_cost != u64::MAX {
        current_order = best_order;
    } else {
        best_transition_cost = transition_cost_for_order(words, &current_order, masks.kill);
    }

    let scheduled_words_hex = current_order
        .iter()
        .map(|&i| format_hex_word(words[i]))
        .collect::<Vec<_>>();
    let original_order = (0..words.len()).collect::<Vec<_>>();
    let original_transition_cost = transition_cost_for_order(words, &original_order, masks.kill);
    let scheduled_transition_cost = transition_cost_for_order(words, &current_order, masks.kill);
    let block_costs = block_transition_costs(words, &blocks, &current_order, masks.kill);
    let verification = verify_schedule(words, masks, &edges, &current_order);

    StrandReport {
        input_len: words.len(),
        input_words_hex: words.iter().copied().map(format_hex_word).collect(),
        masks: MaskReport {
            read_hex: format_hex_word(masks.read),
            write_hex: format_hex_word(masks.write),
            kill_hex: format_hex_word(masks.kill),
            variable_hex: format_hex_word(variable),
        },
        thresholds,
        blocks,
        block_costs,
        edges,
        edge_pair_count,
        edge_summary,
        invisible_words,
        iterations,
        converged,
        schedule_order: current_order,
        scheduled_words_hex,
        costs: CostReport {
            original_transition_cost,
            scheduled_transition_cost,
            delta: scheduled_transition_cost as i64 - original_transition_cost as i64,
        },
        schedule_quality: ScheduleQualityReport {
            selected_iteration,
            best_transition_cost,
            critical_path_len,
            cycle_detected,
        },
        verification,
    }
}

pub fn parse_hex_words(lines: &[String]) -> Result<Vec<u128>, String> {
    let mut out = Vec::new();
    for (line_no, line) in lines.iter().enumerate() {
        let without_comment = line.split('#').next().unwrap_or("");
        let t = without_comment.trim();
        if t.is_empty() {
            continue;
        }
        let h = t
            .strip_prefix("0x")
            .or_else(|| t.strip_prefix("0X"))
            .unwrap_or(t)
            .replace('_', "");
        if h.len() > 32 {
            return Err(format!("line {} exceeds 128 bits: {t}", line_no + 1));
        }
        let v = u128::from_str_radix(&h, 16)
            .map_err(|_| format!("line {} has invalid hex: {t}", line_no + 1))?;
        out.push(v);
    }
    if out.is_empty() {
        return Err("no hex words parsed".into());
    }
    Ok(out)
}

fn push_block(blocks: &mut Vec<BlockReport>, start: usize, end_exclusive: usize) {
    if start >= end_exclusive {
        return;
    }
    blocks.push(BlockReport {
        id: blocks.len(),
        start,
        end_exclusive,
        len: end_exclusive - start,
    });
}

fn lower_median(values: &mut [u32]) -> u32 {
    if values.is_empty() {
        return 0;
    }
    values.sort_unstable();
    values[(values.len() - 1) / 2]
}

fn count_edge_pairs(edges: &[ConflictEdge]) -> usize {
    if edges.is_empty() {
        return 0;
    }
    let mut pairs = edges
        .iter()
        .map(|edge| (edge.source, edge.target))
        .collect::<Vec<_>>();
    pairs.sort_unstable();
    pairs.dedup();
    pairs.len()
}

fn summarize_edges(edges: &[ConflictEdge]) -> EdgeSummary {
    let mut summary = EdgeSummary {
        flow_edges: 0,
        anti_edges: 0,
        output_edges: 0,
        total_weight: 0,
        max_weight: 0,
    };

    for edge in edges {
        match edge.kind {
            ConflictKind::Flow => summary.flow_edges += 1,
            ConflictKind::Anti => summary.anti_edges += 1,
            ConflictKind::Output => summary.output_edges += 1,
        }
        summary.total_weight += edge.weight as u64;
        summary.max_weight = summary.max_weight.max(edge.weight);
    }

    summary
}

fn conflict_critical_path_len(edges: &[ConflictEdge], len: usize) -> usize {
    if len == 0 {
        return 0;
    }

    let mut pairs = edges
        .iter()
        .filter(|edge| edge.source < len && edge.target < len)
        .map(|edge| (edge.source, edge.target))
        .collect::<Vec<_>>();
    pairs.sort_unstable();
    pairs.dedup();

    let mut depth = vec![1usize; len];
    for (source, target) in pairs {
        if source < target {
            depth[target] = depth[target].max(depth[source] + 1);
        }
    }

    depth.into_iter().max().unwrap_or(0)
}

fn detect_invisible_words(words: &[u128], masks: StrandMasks) -> Vec<InvisibleWord> {
    let visible_mask = masks.read | masks.write;
    words
        .iter()
        .enumerate()
        .filter(|(_, &word)| word & visible_mask == 0)
        .map(|(index, &word)| InvisibleWord {
            index,
            word_hex: format_hex_word(word),
        })
        .collect()
}

fn schedule_with_tie_rank(
    words: &[u128],
    masks: StrandMasks,
    edges: &[ConflictEdge],
    blocks: &[BlockReport],
    tie_rank: &[usize],
) -> Vec<usize> {
    if words.is_empty() {
        return Vec::new();
    }

    let mut successors = vec![Vec::<usize>::new(); words.len()];
    let mut indegree = vec![0usize; words.len()];
    let mut pairs = edges
        .iter()
        .map(|edge| (edge.source, edge.target))
        .collect::<Vec<_>>();
    pairs.sort_unstable();
    pairs.dedup();
    for (source, target) in pairs {
        if source < words.len() && target < words.len() {
            successors[source].push(target);
            indegree[target] += 1;
        }
    }

    let mut order = Vec::with_capacity(words.len());
    let mut scheduled = vec![false; words.len()];

    for block in blocks {
        let mut previous_in_block = None;
        for _ in block.start..block.end_exclusive {
            let mut best = None;
            for idx in block.start..block.end_exclusive {
                if scheduled[idx] || indegree[idx] != 0 {
                    continue;
                }
                best = match best {
                    None => Some(idx),
                    Some(current) => {
                        if is_better_candidate(
                            idx,
                            current,
                            previous_in_block,
                            words,
                            masks,
                            tie_rank,
                        ) {
                            Some(idx)
                        } else {
                            Some(current)
                        }
                    }
                };
            }

            let Some(chosen) = best else {
                break;
            };
            scheduled[chosen] = true;
            order.push(chosen);
            previous_in_block = Some(chosen);
            for &succ in &successors[chosen] {
                indegree[succ] = indegree[succ].saturating_sub(1);
            }
        }
    }

    for (idx, is_scheduled) in scheduled.iter().enumerate() {
        if !is_scheduled {
            order.push(idx);
        }
    }

    order
}

fn is_better_candidate(
    candidate: usize,
    current: usize,
    previous: Option<usize>,
    words: &[u128],
    masks: StrandMasks,
    tie_rank: &[usize],
) -> bool {
    let candidate_priority = priority(words[candidate], masks.write);
    let current_priority = priority(words[current], masks.write);
    if candidate_priority != current_priority {
        return candidate_priority > current_priority;
    }

    if let Some(previous) = previous {
        let candidate_cost = transition_cost_between(words, previous, candidate, masks.kill);
        let current_cost = transition_cost_between(words, previous, current, masks.kill);
        if candidate_cost != current_cost {
            return candidate_cost < current_cost;
        }
    }

    let candidate_rank = tie_rank.get(candidate).copied().unwrap_or(usize::MAX);
    let current_rank = tie_rank.get(current).copied().unwrap_or(usize::MAX);
    if candidate_rank != current_rank {
        return candidate_rank < current_rank;
    }

    candidate < current
}

fn rank_for_order(order: &[usize]) -> Vec<usize> {
    let mut rank = vec![usize::MAX; order.len()];
    for (pos, &idx) in order.iter().enumerate() {
        if idx < rank.len() {
            rank[idx] = pos;
        }
    }
    rank
}

fn count_position_changes(a: &[usize], b: &[usize]) -> usize {
    let shared = a.len().min(b.len());
    let changed = (0..shared).filter(|&i| a[i] != b[i]).count();
    changed + a.len().max(b.len()) - shared
}

fn transition_cost_for_order(words: &[u128], order: &[usize], kill_mask: u128) -> u64 {
    order
        .windows(2)
        .map(|pair| ((words[pair[0]] ^ words[pair[1]]) & kill_mask).count_ones() as u64)
        .sum()
}

fn transition_cost_between(words: &[u128], left: usize, right: usize, kill_mask: u128) -> u32 {
    ((words[left] ^ words[right]) & kill_mask).count_ones()
}

fn block_transition_costs(
    words: &[u128],
    blocks: &[BlockReport],
    scheduled_order: &[usize],
    kill_mask: u128,
) -> Vec<BlockCostReport> {
    blocks
        .iter()
        .map(|block| {
            let original_order = (block.start..block.end_exclusive).collect::<Vec<_>>();
            let scheduled_block_order = scheduled_order
                .iter()
                .copied()
                .filter(|&idx| idx >= block.start && idx < block.end_exclusive)
                .collect::<Vec<_>>();
            let original_transition_cost =
                transition_cost_for_order(words, &original_order, kill_mask);
            let scheduled_transition_cost =
                transition_cost_for_order(words, &scheduled_block_order, kill_mask);
            BlockCostReport {
                id: block.id,
                original_transition_cost,
                scheduled_transition_cost,
                delta: scheduled_transition_cost as i64 - original_transition_cost as i64,
            }
        })
        .collect()
}

fn verify_schedule(
    words: &[u128],
    masks: StrandMasks,
    edges: &[ConflictEdge],
    order: &[usize],
) -> VerificationReport {
    let mut violations = Vec::new();
    let scheduled_all_words = is_permutation(order, words.len());
    if !scheduled_all_words {
        violations.push("schedule is not a complete permutation of input indices".to_string());
    }

    let original_xor = words
        .iter()
        .fold(0u128, |acc, &word| acc ^ (word & masks.kill));
    let scheduled_xor = order
        .iter()
        .filter_map(|&idx| words.get(idx))
        .fold(0u128, |acc, &word| acc ^ (word & masks.kill));
    let xor_kill_projection_preserved = original_xor == scheduled_xor;
    if !xor_kill_projection_preserved {
        violations.push("XOR kill-mask projection changed".to_string());
    }

    let mut position = vec![usize::MAX; words.len()];
    for (pos, &idx) in order.iter().enumerate() {
        if idx < position.len() {
            position[idx] = pos;
        }
    }

    let mut edge_order_preserved = true;
    for edge in edges {
        if edge.source >= position.len()
            || edge.target >= position.len()
            || position[edge.source] >= position[edge.target]
        {
            edge_order_preserved = false;
            violations.push(format!(
                "edge {:?} {} -> {} is not preserved",
                edge.kind, edge.source, edge.target
            ));
            if violations.len() >= 16 {
                violations.push("additional edge violations omitted".to_string());
                break;
            }
        }
    }

    VerificationReport {
        xor_kill_projection_preserved,
        edge_order_preserved,
        scheduled_all_words,
        violations,
    }
}

fn is_permutation(order: &[usize], len: usize) -> bool {
    if order.len() != len {
        return false;
    }
    let mut seen = vec![false; len];
    for &idx in order {
        if idx >= len || seen[idx] {
            return false;
        }
        seen[idx] = true;
    }
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mask_and_order_stable() {
        let words = vec![0x0fu128, 0xf0u128, 0xffu128];
        let mw = variable_bit_mask(&words);
        assert_eq!(mw, 0xff);
        let ord = schedule_indices(&words);
        assert_eq!(ord, vec![2, 0, 1]);
    }

    #[test]
    fn report_preserves_conflict_edges() {
        let words = vec![0b0011u128, 0b0111u128, 0b1111u128, 0b1000u128];
        let report = analyze_words(&words);
        assert!(report.verification.scheduled_all_words);
        assert!(report.verification.edge_order_preserved);
        assert!(report.verification.xor_kill_projection_preserved);
        assert_eq!(report.scheduled_words_hex.len(), words.len());
    }

    #[test]
    fn parser_accepts_comments_and_underscores() {
        let lines = vec![
            "0x000f # first".to_string(),
            "0000_00f0".to_string(),
            "# only a comment".to_string(),
        ];
        let words = parse_hex_words(&lines).unwrap();
        assert_eq!(words, vec![0x0f, 0xf0]);
    }

    #[test]
    fn schedule_uses_transition_cost_for_priority_ties() {
        let words = vec![0b0011u128, 0b1100u128, 0b0110u128];
        let masks = StrandMasks {
            read: 0b1111,
            write: 0b1111,
            kill: 0b1111,
        };
        let blocks = vec![BlockReport {
            id: 0,
            start: 0,
            end_exclusive: words.len(),
            len: words.len(),
        }];
        let order = schedule_with_tie_rank(&words, masks, &[], &blocks, &[0, 1, 2]);
        assert_eq!(order, vec![0, 2, 1]);
    }

    #[test]
    fn report_includes_quality_and_block_costs() {
        let words = vec![0x0fu128, 0xf0u128, 0xffu128];
        let report = analyze_words(&words);
        assert_eq!(report.block_costs.len(), report.blocks.len());
        assert_eq!(
            report.schedule_quality.best_transition_cost,
            report.costs.scheduled_transition_cost
        );
        assert!(report.schedule_quality.critical_path_len >= 1);
    }
}
