<script lang="ts">
  import { analyzeInput, type AnalyzeResult } from "./strand";

  const samples = {
    demo: `0000000000000000000000000000000f
000000000000000000000000000000f0
000000000000000000000000000000ff`,
    hazards: `00000000000000000000000000000003
00000000000000000000000000000007
0000000000000000000000000000000f
00000000000000000000000000000008
00000000000000000000000000000018
00000000000000000000000000000038`,
    blocks: `00000000000000000000000000000001
00000000000000000000000000000003
00000000000000000000000000000007
fffffffffffffffffffffffffffffff0
ffffffffffffffffffffffffffffffe0
ffffffffffffffffffffffffffffffc0`,
  };

  let input = samples.hazards;
  let fixedPoint = true;
  let maxIterations = 32;
  let activeSample: keyof typeof samples = "hazards";
  let result: AnalyzeResult = analyzeInput(input, { fixedPoint, maxIterations });

  $: result = analyzeInput(input, { fixedPoint, maxIterations });
  $: report = result.ok ? result.report : null;
  $: jsonReport = report ? JSON.stringify(report, null, 2) : "";
  $: verificationOk =
    report?.verification.xor_kill_projection_preserved &&
    report?.verification.edge_order_preserved &&
    report?.verification.scheduled_all_words;

  function loadSample(name: keyof typeof samples) {
    activeSample = name;
    input = samples[name];
  }

  function blockCost(id: number) {
    return report?.block_costs.find((cost) => cost.id === id);
  }
</script>

<main class="shell">
  <header class="topbar">
    <div>
      <p class="eyebrow">STRAND Studio</p>
      <h1>Deterministic Schedule Analyzer</h1>
    </div>
    <div class:ok={verificationOk} class="status-pill">
      {verificationOk ? "Verified" : "Needs review"}
    </div>
  </header>

  <section class="workspace">
    <div class="panel editor-panel">
      <div class="panel-head">
        <h2>Input</h2>
        <div class="sample-tabs" aria-label="Samples">
          <button
            class:active={activeSample === "hazards"}
            type="button"
            on:click={() => loadSample("hazards")}
          >
            Hazards
          </button>
          <button
            class:active={activeSample === "blocks"}
            type="button"
            on:click={() => loadSample("blocks")}
          >
            Blocks
          </button>
          <button
            class:active={activeSample === "demo"}
            type="button"
            on:click={() => loadSample("demo")}
          >
            Demo
          </button>
        </div>
      </div>

      <textarea
        bind:value={input}
        spellcheck="false"
        aria-label="128-bit hex words"
      />

      <div class="control-row">
        <label class="toggle">
          <input type="checkbox" bind:checked={fixedPoint} />
          <span>Fixed point</span>
        </label>
        <label class="number-field">
          <span>Iterations</span>
          <input min="1" max="128" type="number" bind:value={maxIterations} />
        </label>
      </div>
    </div>

    <div class="panel">
      <div class="panel-head">
        <h2>Analysis</h2>
        {#if report}
          <span class="mono">{report.input_len} words</span>
        {/if}
      </div>

      {#if result.ok && report}
        <div class="stat-grid">
          <div class="stat">
            <span>Blocks</span>
            <strong>{report.blocks.length}</strong>
          </div>
          <div class="stat">
            <span>Edges</span>
            <strong>{report.edges.length}</strong>
          </div>
          <div class="stat">
            <span>Pairs</span>
            <strong>{report.edge_pair_count}</strong>
          </div>
          <div class="stat">
            <span>Invisible</span>
            <strong>{report.invisible_words.length}</strong>
          </div>
          <div class="stat">
            <span>Critical Path</span>
            <strong>{report.schedule_quality.critical_path_len}</strong>
          </div>
          <div class="stat">
            <span>Best Replay</span>
            <strong>{report.schedule_quality.best_transition_cost}</strong>
          </div>
          <div class="stat">
            <span>Selected Pass</span>
            <strong>{report.schedule_quality.selected_iteration}</strong>
          </div>
          <div class="stat">
            <span>Cycle</span>
            <strong>{report.schedule_quality.cycle_detected ? "yes" : "no"}</strong>
          </div>
          <div class="stat wide">
            <span>Transition Cost</span>
            <strong>
              {report.costs.original_transition_cost} -> {report.costs.scheduled_transition_cost}
              <em>{report.costs.delta > 0 ? "+" : ""}{report.costs.delta}</em>
            </strong>
          </div>
          <div class="stat wide">
            <span>Thresholds</span>
            <strong>
              FC {report.thresholds.theta_fc} / AC {report.thresholds.theta_ac} / OC
              {report.thresholds.theta_oc}
            </strong>
          </div>
        </div>

        <div class="mask-grid">
          <div>
            <span>Read</span>
            <code>{report.masks.read_hex}</code>
          </div>
          <div>
            <span>Write</span>
            <code>{report.masks.write_hex}</code>
          </div>
          <div>
            <span>Kill</span>
            <code>{report.masks.kill_hex}</code>
          </div>
          <div>
            <span>Variable</span>
            <code>{report.masks.variable_hex}</code>
          </div>
        </div>
      {:else}
        <p class="error">{result.error}</p>
      {/if}
    </div>
  </section>

  {#if report}
    <section class="detail-grid">
      <div class="panel">
        <div class="panel-head">
          <h2>Schedule</h2>
          <span class="mono">{report.converged ? "converged" : "maxed"}</span>
        </div>
        <div class="table-wrap">
          <table>
            <thead>
              <tr>
                <th>Slot</th>
                <th>Source</th>
                <th>Word</th>
              </tr>
            </thead>
            <tbody>
              {#each report.schedule_order as originalIndex, slot}
                <tr>
                  <td>{slot}</td>
                  <td>{originalIndex}</td>
                  <td><code>{report.scheduled_words_hex[slot]}</code></td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      </div>

      <div class="panel">
        <div class="panel-head">
          <h2>Iterations</h2>
          <span class="mono">{report.iterations.length}</span>
        </div>
        <div class="table-wrap">
          <table>
            <thead>
              <tr>
                <th>Pass</th>
                <th>Cost</th>
                <th>Changed</th>
              </tr>
            </thead>
            <tbody>
              {#each report.iterations as iteration}
                <tr>
                  <td>{iteration.iteration}</td>
                  <td>{iteration.transition_cost}</td>
                  <td>{iteration.changed_positions}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      </div>

      <div class="panel">
        <div class="panel-head">
          <h2>Conflict Edges</h2>
          <span class="mono">first 18</span>
        </div>
        <div class="table-wrap">
          <table>
            <thead>
              <tr>
                <th>Kind</th>
                <th>Source</th>
                <th>Target</th>
                <th>Weight</th>
              </tr>
            </thead>
            <tbody>
              {#each report.edges.slice(0, 18) as edge}
                <tr>
                  <td><span class={`edge ${edge.kind}`}>{edge.kind}</span></td>
                  <td>{edge.source}</td>
                  <td>{edge.target}</td>
                  <td>{edge.weight}</td>
                </tr>
              {/each}
              {#if report.edges.length === 0}
                <tr><td colspan="4">No conflicts above threshold</td></tr>
              {/if}
            </tbody>
          </table>
        </div>
      </div>

      <div class="panel">
        <div class="panel-head">
          <h2>Blocks</h2>
          <span class="mono">{report.blocks.length}</span>
        </div>
        <div class="table-wrap">
          <table>
            <thead>
              <tr>
                <th>ID</th>
                <th>Start</th>
                <th>End</th>
                <th>Len</th>
                <th>Cost</th>
                <th>Delta</th>
              </tr>
            </thead>
            <tbody>
              {#each report.blocks as block}
                {@const cost = blockCost(block.id)}
                <tr>
                  <td>{block.id}</td>
                  <td>{block.start}</td>
                  <td>{block.end_exclusive}</td>
                  <td>{block.len}</td>
                  <td>{cost?.original_transition_cost ?? 0} -> {cost?.scheduled_transition_cost ?? 0}</td>
                  <td>{cost?.delta ?? 0}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      </div>

      <div class="panel">
        <div class="panel-head">
          <h2>Verification</h2>
          <span class="mono">{verificationOk ? "clean" : "violations"}</span>
        </div>
        <div class="check-list">
          <span class:pass={report.verification.xor_kill_projection_preserved}>XOR</span>
          <span class:pass={report.verification.edge_order_preserved}>Edges</span>
          <span class:pass={report.verification.scheduled_all_words}>Permutation</span>
        </div>
        {#if report.verification.violations.length > 0}
          <ul class="violations">
            {#each report.verification.violations as violation}
              <li>{violation}</li>
            {/each}
          </ul>
        {:else}
          <p class="empty">No witness violations</p>
        {/if}

        {#if report.invisible_words.length > 0}
          <div class="table-wrap compact">
            <table>
              <thead>
                <tr>
                  <th>Invisible</th>
                  <th>Word</th>
                </tr>
              </thead>
              <tbody>
                {#each report.invisible_words.slice(0, 8) as invisible}
                  <tr>
                    <td>{invisible.index}</td>
                    <td><code>{invisible.word_hex}</code></td>
                  </tr>
                {/each}
              </tbody>
            </table>
          </div>
        {/if}
      </div>
    </section>

    <section class="panel json-panel">
      <div class="panel-head">
        <h2>JSON Report</h2>
        <span class="mono">CLI-compatible</span>
      </div>
      <pre>{jsonReport}</pre>
    </section>
  {/if}
</main>

<style>
  :global(body) {
    margin: 0;
    min-width: 320px;
    background: #eef2f3;
    color: #1d2327;
    font-family:
      Inter, ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
  }

  :global(*) {
    box-sizing: border-box;
  }

  .shell {
    width: min(1440px, 100%);
    margin: 0 auto;
    padding: 24px;
  }

  .topbar,
  .workspace,
  .detail-grid {
    display: grid;
    gap: 16px;
  }

  .topbar {
    grid-template-columns: 1fr auto;
    align-items: end;
    padding: 8px 0 18px;
  }

  .eyebrow,
  .mono,
  th,
  code,
  pre {
    font-family: "SFMono-Regular", Consolas, "Liberation Mono", monospace;
  }

  .eyebrow {
    margin: 0 0 4px;
    color: #236b5e;
    font-size: 12px;
    font-weight: 700;
    letter-spacing: 0.08em;
    text-transform: uppercase;
  }

  h1,
  h2 {
    margin: 0;
    letter-spacing: 0;
  }

  h1 {
    font-size: 30px;
    line-height: 1.15;
  }

  h2 {
    font-size: 16px;
  }

  .status-pill {
    min-width: 128px;
    border: 1px solid #b8c5c9;
    border-radius: 999px;
    padding: 8px 14px;
    background: #ffffff;
    color: #9b2f30;
    font-weight: 700;
    text-align: center;
  }

  .status-pill.ok {
    border-color: #7bb09e;
    background: #eaf7f1;
    color: #236b5e;
  }

  .workspace {
    grid-template-columns: minmax(360px, 0.95fr) minmax(420px, 1.05fr);
    align-items: stretch;
  }

  .detail-grid {
    grid-template-columns: repeat(2, minmax(0, 1fr));
    margin-top: 16px;
  }

  .panel {
    border: 1px solid #cbd5d8;
    border-radius: 8px;
    background: #ffffff;
    padding: 16px;
    min-width: 0;
  }

  .panel-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
    margin-bottom: 12px;
  }

  .mono {
    color: #66747a;
    font-size: 12px;
  }

  .sample-tabs {
    display: inline-flex;
    overflow: hidden;
    border: 1px solid #b8c5c9;
    border-radius: 7px;
  }

  button {
    min-height: 34px;
    border: 0;
    border-right: 1px solid #b8c5c9;
    padding: 0 12px;
    background: #f4f7f8;
    color: #334047;
    font: inherit;
    font-size: 13px;
    font-weight: 700;
    cursor: pointer;
  }

  button:last-child {
    border-right: 0;
  }

  button.active {
    background: #245f73;
    color: white;
  }

  textarea {
    width: 100%;
    min-height: 312px;
    resize: vertical;
    border: 1px solid #b8c5c9;
    border-radius: 7px;
    background: #15191c;
    color: #e8f3ed;
    padding: 14px;
    font-family: "SFMono-Regular", Consolas, "Liberation Mono", monospace;
    font-size: 13px;
    line-height: 1.65;
    outline: none;
  }

  textarea:focus,
  input:focus {
    border-color: #245f73;
    box-shadow: 0 0 0 3px rgba(36, 95, 115, 0.18);
  }

  .control-row {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 14px;
    margin-top: 12px;
  }

  .toggle,
  .number-field {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    color: #334047;
    font-size: 14px;
    font-weight: 700;
  }

  .toggle input {
    width: 18px;
    height: 18px;
    accent-color: #245f73;
  }

  .number-field input {
    width: 78px;
    height: 34px;
    border: 1px solid #b8c5c9;
    border-radius: 7px;
    padding: 0 8px;
    background: white;
    color: #1d2327;
    font: inherit;
  }

  .stat-grid {
    display: grid;
    grid-template-columns: repeat(4, minmax(0, 1fr));
    gap: 10px;
  }

  .stat {
    min-height: 78px;
    border: 1px solid #d5dde0;
    border-radius: 7px;
    padding: 12px;
    background: #f7fafb;
  }

  .stat.wide {
    grid-column: span 2;
  }

  .stat span,
  .mask-grid span {
    display: block;
    margin-bottom: 8px;
    color: #66747a;
    font-size: 12px;
    font-weight: 700;
    text-transform: uppercase;
  }

  .stat strong {
    display: block;
    color: #1d2327;
    font-size: 24px;
    line-height: 1.15;
  }

  .stat.wide strong {
    font-size: 17px;
  }

  .stat em {
    color: #9b2f30;
    font-style: normal;
  }

  .mask-grid {
    display: grid;
    gap: 10px;
    margin-top: 12px;
  }

  .mask-grid div {
    min-width: 0;
    border-left: 3px solid #245f73;
    padding: 10px 12px;
    background: #eef6f7;
  }

  code {
    display: inline-block;
    max-width: 100%;
    overflow-wrap: anywhere;
    border-radius: 4px;
    color: #16495c;
    font-size: 12px;
  }

  .table-wrap {
    overflow: auto;
    max-height: 380px;
  }

  table {
    width: 100%;
    border-collapse: collapse;
    font-size: 14px;
  }

  th,
  td {
    border-bottom: 1px solid #e1e8ea;
    padding: 9px 8px;
    text-align: left;
    vertical-align: top;
  }

  th {
    position: sticky;
    top: 0;
    z-index: 1;
    background: #ffffff;
    color: #66747a;
    font-size: 11px;
    text-transform: uppercase;
  }

  .edge {
    display: inline-flex;
    min-width: 58px;
    justify-content: center;
    border-radius: 999px;
    padding: 3px 8px;
    color: white;
    font-size: 12px;
    font-weight: 700;
  }

  .edge.flow {
    background: #245f73;
  }

  .edge.anti {
    background: #9b2f30;
  }

  .edge.output {
    background: #52753c;
  }

  .json-panel {
    margin-top: 16px;
  }

  pre {
    max-height: 440px;
    overflow: auto;
    margin: 0;
    border-radius: 7px;
    background: #15191c;
    color: #d7eadf;
    padding: 14px;
    font-size: 12px;
    line-height: 1.55;
  }

  .error {
    margin: 0;
    border-left: 3px solid #9b2f30;
    background: #fff0f0;
    padding: 12px;
    color: #7c2425;
    font-weight: 700;
  }

  .check-list {
    display: flex;
    flex-wrap: wrap;
    gap: 8px;
    margin-bottom: 12px;
  }

  .check-list span {
    border: 1px solid #d5dde0;
    border-radius: 999px;
    padding: 5px 10px;
    background: #fff0f0;
    color: #7c2425;
    font-size: 12px;
    font-weight: 800;
  }

  .check-list span.pass {
    border-color: #7bb09e;
    background: #eaf7f1;
    color: #236b5e;
  }

  .violations {
    margin: 0 0 12px;
    padding-left: 18px;
    color: #7c2425;
    font-size: 13px;
    line-height: 1.5;
  }

  .empty {
    margin: 0 0 12px;
    color: #66747a;
    font-size: 13px;
    font-weight: 700;
  }

  .table-wrap.compact {
    max-height: 220px;
  }

  @media (max-width: 980px) {
    .workspace,
    .detail-grid {
      grid-template-columns: 1fr;
    }

    .stat-grid {
      grid-template-columns: repeat(2, minmax(0, 1fr));
    }
  }

  @media (max-width: 640px) {
    .shell {
      padding: 14px;
    }

    .topbar {
      grid-template-columns: 1fr;
      align-items: stretch;
    }

    .panel-head,
    .control-row {
      align-items: stretch;
      flex-direction: column;
    }

    .sample-tabs {
      display: grid;
      grid-template-columns: repeat(3, 1fr);
    }

    button {
      border-right: 1px solid #b8c5c9;
    }

    .stat-grid {
      grid-template-columns: 1fr;
    }

    .stat.wide {
      grid-column: span 1;
    }
  }
</style>
