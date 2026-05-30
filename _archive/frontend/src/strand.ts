export type ConflictKind = "flow" | "anti" | "output";

export interface Thresholds {
  theta_fc: number;
  theta_ac: number;
  theta_oc: number;
}

export interface ConflictEdge {
  source: number;
  target: number;
  kind: ConflictKind;
  weight: number;
}

export interface BlockReport {
  id: number;
  start: number;
  end_exclusive: number;
  len: number;
}

export interface BlockCostReport {
  id: number;
  original_transition_cost: number;
  scheduled_transition_cost: number;
  delta: number;
}

export interface InvisibleWord {
  index: number;
  word_hex: string;
}

export interface IterationReport {
  iteration: number;
  order: number[];
  transition_cost: number;
  changed_positions: number;
}

export interface EdgeSummary {
  flow_edges: number;
  anti_edges: number;
  output_edges: number;
  total_weight: number;
  max_weight: number;
}

export interface ScheduleQualityReport {
  selected_iteration: number;
  best_transition_cost: number;
  critical_path_len: number;
  cycle_detected: boolean;
}

export interface StrandReport {
  input_len: number;
  input_words_hex: string[];
  masks: {
    read_hex: string;
    write_hex: string;
    kill_hex: string;
    variable_hex: string;
  };
  thresholds: Thresholds;
  blocks: BlockReport[];
  block_costs: BlockCostReport[];
  edges: ConflictEdge[];
  edge_pair_count: number;
  edge_summary: EdgeSummary;
  invisible_words: InvisibleWord[];
  iterations: IterationReport[];
  converged: boolean;
  schedule_order: number[];
  scheduled_words_hex: string[];
  costs: {
    original_transition_cost: number;
    scheduled_transition_cost: number;
    delta: number;
  };
  schedule_quality: ScheduleQualityReport;
  verification: {
    xor_kill_projection_preserved: boolean;
    edge_order_preserved: boolean;
    scheduled_all_words: boolean;
    violations: string[];
  };
}

export type AnalyzeResult =
  | { ok: true; report: StrandReport }
  | { ok: false; error: string };

export interface AnalyzeOptions {
  fixedPoint: boolean;
  maxIterations: number;
}

interface Masks {
  read: bigint;
  write: bigint;
  kill: bigint;
}

const FULL_MASK = (1n << 128n) - 1n;

export function analyzeInput(input: string, options: AnalyzeOptions): AnalyzeResult {
  try {
    const words = parseWords(input);
    if (words.length === 0) {
      return { ok: false, error: "No 128-bit words parsed." };
    }
    return { ok: true, report: analyzeWords(words, options) };
  } catch (error) {
    return { ok: false, error: error instanceof Error ? error.message : String(error) };
  }
}

export function parseWords(input: string): bigint[] {
  const words: bigint[] = [];
  input.split(/\r?\n/).forEach((line, lineIndex) => {
    const token = line.split("#")[0].trim();
    if (!token) return;
    const hex = token.replace(/^0x/i, "").replaceAll("_", "");
    if (hex.length > 32) {
      throw new Error(`Line ${lineIndex + 1} exceeds 128 bits.`);
    }
    if (!/^[0-9a-fA-F]+$/.test(hex)) {
      throw new Error(`Line ${lineIndex + 1} is not valid hex.`);
    }
    words.push(BigInt(`0x${hex}`) & FULL_MASK);
  });
  return words;
}

export function analyzeWords(words: bigint[], options: AnalyzeOptions): StrandReport {
  const variable = variableMask(words);
  const masks = deriveMasks(words);
  const blocks = deriveBlocks(words, masks.kill);
  const thresholds = deriveThresholds(words, masks, blocks);
  const edges = buildEdges(words, masks, thresholds, blocks);
  const edgeSummary = summarizeEdges(edges);
  const invisible = detectInvisible(words, masks);

  let tieRank = words.map((_, index) => index);
  let currentOrder = words.map((_, index) => index);
  const iterations: IterationReport[] = [];
  const maxIterations = Math.max(1, Math.floor(options.maxIterations || 1));
  let converged = false;
  let cycleDetected = false;
  const seenOrders = new Set<string>([orderKey(currentOrder)]);
  let bestOrder = [...currentOrder];
  let bestTransitionCost = Number.POSITIVE_INFINITY;
  let selectedIteration = 0;

  for (let iteration = 0; iteration < maxIterations; iteration++) {
    const nextOrder = schedule(words, masks, edges, blocks, tieRank);
    const changed = changedPositions(currentOrder, nextOrder);
    const iterationCost = transitionCost(words, nextOrder, masks.kill);

    if (iterationCost < bestTransitionCost) {
      bestTransitionCost = iterationCost;
      bestOrder = [...nextOrder];
      selectedIteration = iteration;
    }

    iterations.push({
      iteration,
      order: nextOrder,
      transition_cost: iterationCost,
      changed_positions: changed,
    });

    if (sameOrder(nextOrder, currentOrder)) {
      currentOrder = nextOrder;
      converged = true;
      break;
    }

    const nextKey = orderKey(nextOrder);
    if (seenOrders.has(nextKey)) {
      cycleDetected = true;
      currentOrder = nextOrder;
      break;
    }

    currentOrder = nextOrder;
    tieRank = rankForOrder(currentOrder);
    seenOrders.add(nextKey);
    if (!options.fixedPoint) {
      converged = true;
      break;
    }
  }

  if (Number.isFinite(bestTransitionCost)) {
    currentOrder = bestOrder;
  } else {
    bestTransitionCost = transitionCost(words, currentOrder, masks.kill);
  }

  const originalOrder = words.map((_, index) => index);
  const originalCost = transitionCost(words, originalOrder, masks.kill);
  const scheduledCost = transitionCost(words, currentOrder, masks.kill);
  const blockCosts = blockTransitionCosts(words, blocks, currentOrder, masks.kill);

  return {
    input_len: words.length,
    input_words_hex: words.map(hexWord),
    masks: {
      read_hex: hexWord(masks.read),
      write_hex: hexWord(masks.write),
      kill_hex: hexWord(masks.kill),
      variable_hex: hexWord(variable),
    },
    thresholds,
    blocks,
    block_costs: blockCosts,
    edges,
    edge_pair_count: uniquePairCount(edges),
    edge_summary: edgeSummary,
    invisible_words: invisible,
    iterations,
    converged,
    schedule_order: currentOrder,
    scheduled_words_hex: currentOrder.map((index) => hexWord(words[index])),
    costs: {
      original_transition_cost: originalCost,
      scheduled_transition_cost: scheduledCost,
      delta: scheduledCost - originalCost,
    },
    schedule_quality: {
      selected_iteration: selectedIteration,
      best_transition_cost: bestTransitionCost,
      critical_path_len: conflictCriticalPathLen(edges, words.length),
      cycle_detected: cycleDetected,
    },
    verification: verify(words, masks, edges, currentOrder),
  };
}

function variableMask(words: bigint[]): bigint {
  if (words.length === 0) return 0n;
  let orAll = 0n;
  let andAll = FULL_MASK;
  for (const word of words) {
    orAll |= word;
    andAll &= word;
  }
  return orAll ^ andAll;
}

function transitionMask(words: bigint[]): bigint {
  let mask = 0n;
  for (let index = 0; index < words.length - 1; index++) {
    mask |= words[index] ^ words[index + 1];
  }
  return mask;
}

function deriveMasks(words: bigint[]): Masks {
  const variable = variableMask(words);
  const kill = transitionMask(words);
  let read = 0n;
  let write = 0n;

  for (let bit = 0; bit < 128; bit++) {
    const mask = 1n << BigInt(bit);
    if ((variable & mask) === 0n) continue;
    let ones = 0;
    for (const word of words) {
      if ((word & mask) !== 0n) ones++;
    }
    const zeros = words.length - ones;
    if (ones <= zeros) read |= mask;
    if (ones >= zeros) write |= mask;
  }

  return {
    read: read === 0n ? variable : read,
    write: write === 0n ? variable : write,
    kill,
  };
}

function deriveBlocks(words: bigint[], killMask: bigint): BlockReport[] {
  if (words.length === 0) return [];
  if (words.length === 1) return [{ id: 0, start: 0, end_exclusive: 1, len: 1 }];

  const costs: number[] = [];
  for (let index = 0; index < words.length - 1; index++) {
    costs.push(popcnt((words[index] ^ words[index + 1]) & killMask));
  }
  const mean = costs.reduce((sum, value) => sum + value, 0) / costs.length;
  const variance =
    costs.reduce((sum, value) => sum + (value - mean) * (value - mean), 0) / costs.length;
  const cutoff = mean + 2 * Math.sqrt(variance);

  const blocks: BlockReport[] = [];
  let start = 0;
  for (let index = 0; index < costs.length; index++) {
    if (costs[index] > cutoff) {
      pushBlock(blocks, start, index + 1);
      start = index + 1;
    }
  }
  pushBlock(blocks, start, words.length);
  return blocks;
}

function pushBlock(blocks: BlockReport[], start: number, end: number) {
  if (start >= end) return;
  blocks.push({ id: blocks.length, start, end_exclusive: end, len: end - start });
}

function deriveThresholds(words: bigint[], masks: Masks, blocks: BlockReport[]): Thresholds {
  const fc: number[] = [];
  const ac: number[] = [];
  const oc: number[] = [];
  for (const block of blocks) {
    for (let i = block.start; i < block.end_exclusive; i++) {
      for (let j = i + 1; j < block.end_exclusive; j++) {
        const forward = popcnt((words[i] & masks.write) & (words[j] & masks.read));
        const anti = popcnt((words[j] & masks.write) & (words[i] & masks.read));
        const output = popcnt((words[i] & masks.write) & (words[j] & masks.write));
        if (forward > 0) fc.push(forward);
        if (anti > 0) ac.push(anti);
        if (output > 0) oc.push(output);
      }
    }
  }
  return { theta_fc: median(fc), theta_ac: median(ac), theta_oc: median(oc) };
}

function buildEdges(
  words: bigint[],
  masks: Masks,
  thresholds: Thresholds,
  blocks: BlockReport[],
): ConflictEdge[] {
  const edges: ConflictEdge[] = [];
  for (const block of blocks) {
    for (let i = block.start; i < block.end_exclusive; i++) {
      for (let j = i + 1; j < block.end_exclusive; j++) {
        const forward = popcnt((words[i] & masks.write) & (words[j] & masks.read));
        const anti = popcnt((words[j] & masks.write) & (words[i] & masks.read));
        const output = popcnt((words[i] & masks.write) & (words[j] & masks.write));
        if (forward > thresholds.theta_fc) {
          edges.push({ source: i, target: j, kind: "flow", weight: forward });
        }
        if (anti > thresholds.theta_ac) {
          edges.push({ source: i, target: j, kind: "anti", weight: anti });
        }
        if (output > thresholds.theta_oc) {
          edges.push({ source: i, target: j, kind: "output", weight: output });
        }
      }
    }
  }
  return edges;
}

function schedule(
  words: bigint[],
  masks: Masks,
  edges: ConflictEdge[],
  blocks: BlockReport[],
  tieRank: number[],
): number[] {
  const successors = Array.from({ length: words.length }, () => [] as number[]);
  const indegree = Array.from({ length: words.length }, () => 0);
  const seenPairs = new Set<string>();
  for (const edge of edges) {
    const key = `${edge.source}:${edge.target}`;
    if (seenPairs.has(key)) continue;
    seenPairs.add(key);
    successors[edge.source].push(edge.target);
    indegree[edge.target]++;
  }

  const order: number[] = [];
  const used = Array.from({ length: words.length }, () => false);
  for (const block of blocks) {
    let previous: number | null = null;
    for (let count = block.start; count < block.end_exclusive; count++) {
      let best = -1;
      for (let index = block.start; index < block.end_exclusive; index++) {
        if (used[index] || indegree[index] !== 0) continue;
        if (best < 0 || better(index, best, previous, words, masks, tieRank)) {
          best = index;
        }
      }
      if (best < 0) break;
      used[best] = true;
      order.push(best);
      previous = best;
      for (const successor of successors[best]) {
        indegree[successor] = Math.max(0, indegree[successor] - 1);
      }
    }
  }

  for (let index = 0; index < words.length; index++) {
    if (!used[index]) order.push(index);
  }
  return order;
}

function better(
  candidate: number,
  current: number,
  previous: number | null,
  words: bigint[],
  masks: Masks,
  tieRank: number[],
): boolean {
  const candidatePriority = priority(words[candidate], masks.write);
  const currentPriority = priority(words[current], masks.write);
  if (candidatePriority !== currentPriority) return candidatePriority > currentPriority;

  if (previous !== null) {
    const candidateCost = popcnt((words[previous] ^ words[candidate]) & masks.kill);
    const currentCost = popcnt((words[previous] ^ words[current]) & masks.kill);
    if (candidateCost !== currentCost) return candidateCost < currentCost;
  }

  if (tieRank[candidate] !== tieRank[current]) return tieRank[candidate] < tieRank[current];
  return candidate < current;
}

function detectInvisible(words: bigint[], masks: Masks): InvisibleWord[] {
  const visibleMask = masks.read | masks.write;
  return words
    .map((word, index) => ({ word, index }))
    .filter(({ word }) => (word & visibleMask) === 0n)
    .map(({ word, index }) => ({ index, word_hex: hexWord(word) }));
}

function verify(
  words: bigint[],
  masks: Masks,
  edges: ConflictEdge[],
  order: number[],
): StrandReport["verification"] {
  const violations: string[] = [];
  const scheduledAllWords = isPermutation(order, words.length);
  if (!scheduledAllWords) violations.push("schedule is not a complete permutation");

  const originalXor = words.reduce((acc, word) => acc ^ (word & masks.kill), 0n);
  const scheduledXor = order.reduce((acc, index) => {
    const word = words[index];
    return word === undefined ? acc : acc ^ (word & masks.kill);
  }, 0n);
  const xorPreserved = originalXor === scheduledXor;
  if (!xorPreserved) violations.push("XOR kill-mask projection changed");

  const positions = Array.from({ length: words.length }, () => Number.MAX_SAFE_INTEGER);
  order.forEach((index, position) => {
    if (index >= 0 && index < positions.length) positions[index] = position;
  });

  let edgeOrderPreserved = true;
  for (const edge of edges) {
    if (positions[edge.source] >= positions[edge.target]) {
      edgeOrderPreserved = false;
      if (violations.length < 16) {
        violations.push(`edge ${edge.kind} ${edge.source} -> ${edge.target} is not preserved`);
      }
    }
  }

  return {
    xor_kill_projection_preserved: xorPreserved,
    edge_order_preserved: edgeOrderPreserved,
    scheduled_all_words: scheduledAllWords,
    violations,
  };
}

function transitionCost(words: bigint[], order: number[], killMask: bigint): number {
  let total = 0;
  for (let index = 0; index < order.length - 1; index++) {
    total += popcnt((words[order[index]] ^ words[order[index + 1]]) & killMask);
  }
  return total;
}

function priority(word: bigint, writeMask: bigint): number {
  return popcnt(word & writeMask);
}

function popcnt(value: bigint): number {
  let count = 0;
  let current = value;
  while (current > 0n) {
    count += Number(current & 1n);
    current >>= 1n;
  }
  return count;
}

function median(values: number[]): number {
  if (values.length === 0) return 0;
  const sorted = [...values].sort((a, b) => a - b);
  return sorted[Math.floor((sorted.length - 1) / 2)];
}

function rankForOrder(order: number[]): number[] {
  const rank = Array.from({ length: order.length }, () => Number.MAX_SAFE_INTEGER);
  order.forEach((index, position) => {
    rank[index] = position;
  });
  return rank;
}

function changedPositions(a: number[], b: number[]): number {
  let changed = Math.abs(a.length - b.length);
  const shared = Math.min(a.length, b.length);
  for (let index = 0; index < shared; index++) {
    if (a[index] !== b[index]) changed++;
  }
  return changed;
}

function sameOrder(a: number[], b: number[]): boolean {
  return a.length === b.length && a.every((value, index) => value === b[index]);
}

function uniquePairCount(edges: ConflictEdge[]): number {
  return new Set(edges.map((edge) => `${edge.source}:${edge.target}`)).size;
}

function summarizeEdges(edges: ConflictEdge[]): EdgeSummary {
  const summary: EdgeSummary = {
    flow_edges: 0,
    anti_edges: 0,
    output_edges: 0,
    total_weight: 0,
    max_weight: 0,
  };
  for (const edge of edges) {
    if (edge.kind === "flow") summary.flow_edges++;
    if (edge.kind === "anti") summary.anti_edges++;
    if (edge.kind === "output") summary.output_edges++;
    summary.total_weight += edge.weight;
    summary.max_weight = Math.max(summary.max_weight, edge.weight);
  }
  return summary;
}

function conflictCriticalPathLen(edges: ConflictEdge[], len: number): number {
  if (len === 0) return 0;
  const pairs = Array.from(
    new Set(
      edges
        .filter((edge) => edge.source < len && edge.target < len)
        .map((edge) => `${edge.source}:${edge.target}`),
    ),
  )
    .map((key) => key.split(":").map(Number) as [number, number])
    .sort((a, b) => a[0] - b[0] || a[1] - b[1]);

  const depth = Array.from({ length: len }, () => 1);
  for (const [source, target] of pairs) {
    if (source < target) depth[target] = Math.max(depth[target], depth[source] + 1);
  }
  return Math.max(...depth);
}

function blockTransitionCosts(
  words: bigint[],
  blocks: BlockReport[],
  scheduledOrder: number[],
  killMask: bigint,
): BlockCostReport[] {
  return blocks.map((block) => {
    const originalOrder = Array.from(
      { length: block.end_exclusive - block.start },
      (_, offset) => block.start + offset,
    );
    const scheduledBlockOrder = scheduledOrder.filter(
      (index) => index >= block.start && index < block.end_exclusive,
    );
    const originalCost = transitionCost(words, originalOrder, killMask);
    const scheduledCost = transitionCost(words, scheduledBlockOrder, killMask);
    return {
      id: block.id,
      original_transition_cost: originalCost,
      scheduled_transition_cost: scheduledCost,
      delta: scheduledCost - originalCost,
    };
  });
}

function orderKey(order: number[]): string {
  return order.join(",");
}

function isPermutation(order: number[], len: number): boolean {
  if (order.length !== len) return false;
  const seen = Array.from({ length: len }, () => false);
  for (const index of order) {
    if (index < 0 || index >= len || seen[index]) return false;
    seen[index] = true;
  }
  return true;
}

function hexWord(word: bigint): string {
  return word.toString(16).padStart(32, "0");
}
